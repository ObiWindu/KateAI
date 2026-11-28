/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sandbox.h"

#include "processenv.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <functional>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

    // How many paths a single glob token may expand to. The expansion only
    // feeds the deny scan, so a bound keeps a wide pattern in a huge directory
    // from turning the permission check into a directory walk.
    constexpr int MaxGlobExpansionResults = 512;

    // Deny names are matched case-insensitively on filesystems that are too.
    // Without this, ".ENV", "id_RSA" or "SERVER.PEM" names the same file as
    // the denied spelling on macOS/Windows but slipped past the deny list.
    static QRegularExpression::PatternOptions globPatternOptions()
    {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        return QRegularExpression::CaseInsensitiveOption;
#else
        return QRegularExpression::NoPatternOption;
#endif
    }

    static QString globToRegex(const QString &pattern)
    {
        QString regex = u"^"_s;
        QString normalized = QDir::fromNativeSeparators(pattern);
        // A trailing "/**" means "everything below this directory". The
        // directory itself has to match as well, otherwise "tar ~/.ssh" is not
        // covered by the "**/.ssh/**" rule.
        bool directoryContents = false;
        if (normalized.endsWith(u"/**"_s)) {
            normalized = normalized.left(normalized.size() - 3);
            directoryContents = true;
        }
        for (int i = 0; i < normalized.size(); ++i) {
            const QChar c = normalized.at(i);
            if (c == u'*' && i + 1 < normalized.size() && normalized.at(i + 1) == u'*') {
                ++i;
                if (i + 1 < normalized.size() && normalized.at(i + 1) == u'/') {
                    // "**/" matches zero or more whole directory segments. The
                    // slash is only absent at the very start of the path, so
                    // "**/.env" cannot match "foo.env": the dotfile has to be
                    // its own path segment, the way gitignore reads the glob.
                    regex += u"(?:.*/)?"_s;
                    ++i;
                } else {
                    regex += u".*"_s;
                }
            } else if (c == u'*') {
                regex += u"[^/]*"_s;
            } else if (c == u'?') {
                regex += u"[^/]"_s;
            } else if (c == u'[') {
                // A character class, e.g. "*.key[0-9]". Without this branch
                // the '[' was escaped as a literal and a user deny glob with
                // a class silently matched nothing.
                int close = -1;
                // A ']' directly after '[' is a literal member, in glob and
                // in PCRE alike, so the search for the class end starts
                // behind it.
                int j = i + 1;
                if (j < normalized.size() && normalized.at(j) == u']') {
                    ++j;
                }
                for (; j < normalized.size(); ++j) {
                    if (normalized.at(j) == u']') {
                        close = j;
                        break;
                    }
                    if (normalized.at(j) == u'\\' && j + 1 < normalized.size()) {
                        ++j;
                    }
                }
                if (close < 0) {
                    // No closing bracket: a literal '[' after all.
                    regex += u"\\["_s;
                    continue;
                }
                QString members = normalized.mid(i + 1, close - i - 1);
                const bool negated = !members.isEmpty() && (members.at(0) == u'!' || members.at(0) == u'^');
                if (negated) {
                    members = members.mid(1);
                }
                QString mapped;
                for (int k = 0; k < members.size(); ++k) {
                    const QChar mc = members.at(k);
                    if (mc == u'\\' && k + 1 < members.size()) {
                        mapped += u'\\';
                        mapped += members.at(k + 1);
                        ++k;
                        continue;
                    }
                    // A '-' stays: PCRE reads "a-z" as a range exactly the
                    // way glob does. Everything else the regex engine gives a
                    // meaning to is escaped; ']' cannot occur here except as
                    // the first member, which PCRE also reads as literal.
                    if (QStringLiteral(".^$()+{}|?*[]").contains(mc)) {
                        mapped += u'\\';
                    }
                    mapped += mc;
                }
                if (mapped.isEmpty()) {
                    // "[]" and "[!]": the class says nothing, so the '[' is a
                    // literal like any unmatched one.
                    regex += u"\\["_s;
                    continue;
                }
                regex += negated ? u"[^"_s : u"["_s;
                regex += mapped;
                regex += u']';
                i = close;
            } else if (QStringLiteral(".^$+()[]{}|\\").contains(c)) {
                regex += u'\\';
                regex += c;
            } else {
                regex += c;
            }
        }
        if (directoryContents) {
            regex += QStringLiteral(u"(/.*)?$");
        } else {
            regex += QLatin1Char('$');
        }
        return regex;
    }

    // Splits a command line into shell words, honouring quotes. The shell
    // operators that separate one command from the next are kept as their own
    // words so a caller can tell where an invocation ends.
    static QStringList shellTokens(const QString &command)
    {
        QStringList tokens;
        bool inSingleQuote = false;
        bool inDoubleQuote = false;
        QString currentToken;

        for (int i = 0; i < command.size(); ++i) {
            const QChar c = command.at(i);

            if (c == u'\'' && !inDoubleQuote) {
                inSingleQuote = !inSingleQuote;
                currentToken.append(c);
            } else if (c == u'"' && !inSingleQuote) {
                inDoubleQuote = !inDoubleQuote;
                currentToken.append(c);
            } else if ((c.isSpace() || c == u';' || c == u'|' || c == u'&' || c == u'<' || c == u'>') && !inSingleQuote && !inDoubleQuote) {
                if (!currentToken.isEmpty()) {
                    tokens.append(currentToken);
                    currentToken.clear();
                }
                if (c == u'|' && i + 1 < command.size() && command.at(i + 1) == u'|') {
                    tokens.append(QString(u"||"));
                    ++i;
                } else if (c == u'&' && i + 1 < command.size() && command.at(i + 1) == u'&') {
                    tokens.append(QString(u"&&"));
                    ++i;
                } else if (c == u'>' && i + 1 < command.size() && command.at(i + 1) == u'>') {
                    // An output redirection is a delimiter too: ">>file" must not
                    // glue the arrows onto the path and hide a deny-glob hit.
                    ++i;
                }
            } else {
                currentToken.append(c);
            }
        }
        if (!currentToken.isEmpty()) {
            tokens.append(currentToken);
        }
        return tokens;
    }

    // True when a word carries a shell glob metacharacter, so it names a set of
    // paths rather than one path.
    static bool containsGlobMeta(const QString &word)
    {
        return word.contains(u'*') || word.contains(u'?') || word.contains(u'[');
    }

    // A word that separates one command from the next.
    static bool isShellSeparator(const QString &word)
    {
        return word == u";" || word == u"&&" || word == u"||" || word == u"|" || word == u"&";
    }

    // True when an "rm" invocation removes the filesystem root or the home
    // directory. The flags may be short ("-rf", "-fr"), long ("--recursive",
    // "--force") and in any order, "--" may separate them from the target, and
    // the target may be the root, the home directory, or something below it,
    // spelled with "~", "$HOME" or the resolved home path.
    static bool rmTargetsRootOrHome(const QString &command)
    {
        const QString home = QDir::homePath();
        const QStringList words = shellTokens(command);
        for (int i = 0; i < words.size(); ++i) {
            const QString &word = words.at(i);
            // The quotes are stripped before the command is looked at, so
            // "\"/bin/rm\"" is recognised like the bare spelling.
            QString bareWord = word;
            bareWord.remove(u'"');
            bareWord.remove(u'\'');
            const QString base = QFileInfo(bareWord).fileName();
            if (base != u"rm"_s) {
                continue;
            }
            bool recursive = false;
            bool force = false;
            bool endOfFlags = false;
            QString target;
            for (int j = i + 1; j < words.size(); ++j) {
                const QString &arg = words.at(j);
                if (isShellSeparator(arg)) {
                    break;
                }
                if (!endOfFlags && arg == u"--"_s) {
                    endOfFlags = true;
                    continue;
                }
                if (!endOfFlags && arg.startsWith(u'-') && arg.size() > 1) {
                    if (arg.startsWith(u"--"_s)) {
                        recursive = recursive || arg == u"--recursive"_s;
                        force = force || arg == u"--force"_s;
                    } else {
                        for (const QChar flag : arg) {
                            recursive = recursive || flag == u'r';
                            force = force || flag == u'f';
                        }
                    }
                    continue;
                }
                target = arg;
                break;
            }
            if (!recursive && !force) {
                continue;
            }
            // The quotes are stripped before the target is compared, so
            // "rm -rf \"/\"" is recognised like the bare spelling.
            QString bare = target;
            bare.remove(u'"');
            bare.remove(u'\'');
            if (bare.isEmpty()) {
                continue;
            }
            const bool isRoot = bare == u"/"_s || bare.startsWith(u"/*"_s)
                || bare.startsWith(u"//"_s)
                || QDir::cleanPath(bare) == u"/"_s
                || QDir::cleanPath(bare).startsWith(u"/*"_s);
            const bool isHome = bare == u"~"_s || bare.startsWith(u"~/"_s) || bare == u"$HOME"_s || bare.startsWith(u"$HOME/"_s)
                || bare == u"${HOME}"_s || bare.startsWith(u"${HOME}/"_s) || bare == home || bare.startsWith(home + u'/');
            if (isRoot || isHome) {
                return true;
            }
        }
        return false;
    }

    Sandbox::Sandbox(QString workspaceRoot, SandboxProfile profile, QStringList extraDenyGlobs, bool allowOutsideReads)
    : m_profile(profile)
    , m_extraDenyGlobs(std::move(extraDenyGlobs))
    , m_allowOutsideReads(allowOutsideReads)
    {
        // An empty root stays empty. QDir("").absolutePath() is the process's
        // working directory, which would silently root the sandbox there, and
        // every guard that checks for an empty root would then never fire.
        m_workspaceRoot = workspaceRoot.trimmed().isEmpty()
            ? QString()
            : QDir::cleanPath(QDir::fromNativeSeparators(QDir(workspaceRoot).absolutePath()));
        // Canonicalize the workspace root to resolve symlinks (e.g., /var vs /private/var on macOS)
        if (!m_workspaceRoot.isEmpty()) {
            const QString canonicalRoot = QFileInfo(m_workspaceRoot).canonicalFilePath();
            if (!canonicalRoot.isEmpty()) {
                m_workspaceRoot = QDir::cleanPath(QDir::fromNativeSeparators(canonicalRoot));
            }
        }
    }

    QStringList Sandbox::defaultDenyGlobs()
    {
        return {
            u"**/.env"_s,
            u"**/.env.*"_s,
            u"**/*.pem"_s,
            u"**/*.key"_s,
            u"**/id_rsa"_s,
            u"**/id_ecdsa"_s,
            u"**/id_ed25519"_s,
            u"**/.ssh/**"_s,
            u"**/.aws/credentials"_s,
            u"**/.aws/config"_s,
            u"**/.gnupg/**"_s,
            u"**/.netrc"_s,
            u"**/*.p12"_s,
            u"**/*.pfx"_s,
            // The MCP server list is a program the plugin starts. An agent
            // that could edit it could add a server of its own, whose tools
            // would then run with the user's privileges.
            u"**/.kateai/mcp.json"_s,
        };
    }

    QStringList Sandbox::denyGlobs() const
    {
        return defaultDenyGlobs() + m_extraDenyGlobs;
    }

bool Sandbox::globMatch(const QString &pattern, const QString &path)
{
    const QString haystack = QDir::fromNativeSeparators(path);
    QRegularExpression re(globToRegex(pattern), globPatternOptions());
    if (re.match(haystack).hasMatch()) {
        return true;
    }
    // Only match against filename if pattern is a simple filename pattern (no path separators)
    // This prevents false positives like "**/.env" matching "/home/user/.env" when checking filename only
    if (!pattern.contains(u'/') && !pattern.contains(u"**"_s)) {
        const QString name = QFileInfo(haystack).fileName();
        if (re.match(name).hasMatch()) {
            return true;
        }
    }
    // Also check against relative path from workspace root
    if (!pattern.startsWith(u'/') && !pattern.startsWith(u"**"_s)) {
        QRegularExpression nested(globToRegex(u"**/"_s + pattern), globPatternOptions());
        if (nested.match(haystack).hasMatch()) {
            return true;
        }
    }
    return false;
}

// Runs a compiled glob regex, caching the compilation. The deny list is small
// and fixed for the sandbox's lifetime, so each pattern is compiled once.
static bool matchGlobRegex(const QString &regex, const QString &haystack, QHash<QString, QRegularExpression> &cache)
{
    auto it = cache.constFind(regex);
    if (it == cache.constEnd()) {
        it = cache.insert(regex, QRegularExpression(regex, globPatternOptions()));
    }
    return it->match(haystack).hasMatch();
}

    bool Sandbox::globMatchCached(const QString &pattern, const QString &path) const
    {
        const QString haystack = QDir::fromNativeSeparators(path);
        if (matchGlobRegex(globToRegex(pattern), haystack, m_globCache)) {
            return true;
        }
        // Only match against filename if pattern is a simple filename pattern (no path separators)
        // This prevents false positives like "**/.env" matching "/home/user/.env" when checking filename only
        if (!pattern.contains(u'/') && !pattern.contains(u"**"_s)) {
            if (matchGlobRegex(globToRegex(pattern), QFileInfo(haystack).fileName(), m_globCache)) {
                return true;
            }
        }
        // Also check against relative path from workspace root
        if (!pattern.startsWith(u'/') && !pattern.startsWith(u"**"_s)) {
            if (matchGlobRegex(globToRegex(u"**/"_s + pattern), haystack, m_globCache)) {
                return true;
            }
        }
        return false;
    }

    bool Sandbox::isDenied(const QString &path) const
    {
        const QString unix = QDir::fromNativeSeparators(path);
        if (m_cachedDenyGlobs.isEmpty()) {
            m_cachedDenyGlobs = denyGlobs();
        }
        for (const QString &glob : m_cachedDenyGlobs) {
            if (globMatchCached(glob, unix)) {
                return true;
            }
            if (!m_workspaceRoot.isEmpty()) {
                QString relative = unix;
                // The root has to be a directory boundary: with the root
                // "/home/user/proj" the path "/home/user/proj2/x" is not inside
                // the workspace, and treating it as such produced the bogus
                // relative path "2/x" for the deny globs.
                if (relative.startsWith(m_workspaceRoot + u'/')) {
                    relative = relative.mid(m_workspaceRoot.size() + 1);
                    if (globMatchCached(glob, relative) || globMatchCached(glob, u"/"_s + relative)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool Sandbox::pathInsideWorkspace(const QString &absolutePath) const
    {
        // With no root there is no workspace to be inside. Treating "" as "."
        // made every absolute path start with root + '/' and count as inside.
        if (m_workspaceRoot.trimmed().isEmpty()) {
            return false;
        }
        const QString root = QDir::cleanPath(m_workspaceRoot);
        QString path = QDir::cleanPath(absolutePath);

        // Helper to canonicalize a path, falling back to parent directory
        // canonicalization for paths that don't exist yet (e.g., new files).
        auto canonicalize = [](const QString &p) -> QString {
            const QString canon = QFileInfo(p).canonicalFilePath();
            if (!canon.isEmpty()) {
                return QDir::cleanPath(canon);
            }
            // File doesn't exist: try canonicalizing the parent directory
            const QString parent = QFileInfo(p).absolutePath();
            const QString parentCanon = QFileInfo(parent).canonicalFilePath();
            if (!parentCanon.isEmpty()) {
                return QDir::cleanPath(parentCanon + u'/' + QFileInfo(p).fileName());
            }
            return QString();
        };

#if defined(Q_OS_WIN)
        // Windows filesystems are case-insensitive: a path that differs from
        // the workspace root only in casing still names a file inside it.
        // macOS may be either, so it uses the canonical comparison below.
        if (path.compare(root, Qt::CaseInsensitive) == 0) {
            return true;
        }
        if (path.startsWith(root + u'/', Qt::CaseInsensitive)) {
            return true;
        }
        // Try canonicalizing both paths for symlink resolution (e.g., /var vs /private/var on macOS)
        const QString canonicalRoot = canonicalize(root);
        const QString canonicalPath = canonicalize(path);
        if (!canonicalRoot.isEmpty() && !canonicalPath.isEmpty()) {
            const QString cRoot = canonicalRoot;
            const QString cPath = canonicalPath;
            if (cPath.compare(cRoot, Qt::CaseInsensitive) == 0) {
                return true;
            }
            if (cPath.startsWith(cRoot + u'/', Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
#else
        if (path == root) {
            return true;
        }
        if (path.startsWith(root + u'/')) {
            return true;
        }
        // Try canonicalizing both paths for symlink resolution
        const QString canonicalRoot = canonicalize(root);
        const QString canonicalPath = canonicalize(path);
        if (!canonicalRoot.isEmpty() && !canonicalPath.isEmpty()) {
            const QString cRoot = canonicalRoot;
            const QString cPath = canonicalPath;
            if (cPath == cRoot) {
                return true;
            }
            if (cPath.startsWith(cRoot + u'/')) {
                return true;
            }
        }
        return false;
#endif
    }

static QString cleanCanonicalPath(const QString &canonical)
{
    // QFileInfo::canonicalFilePath() may return native separators (backslashes
    // on Windows). Normalize to "/" so startsWith()/endsWith() checks and the
    // paths returned by resolve() behave identically on every platform.
    if (canonical.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(canonical));
}

static QString windowsAbsoluteCandidate(const QString &workspaceRoot, QString candidate)
{
#ifdef Q_OS_WIN
    // A Unix-style absolute path ("/tmp/x", "/etc/passwd") is drive-relative
    // on Windows: QFileInfo resolves it against the *current* drive, which may
    // not be the workspace drive. Anchor it to the workspace drive instead so
    // containment checks see a fully qualified path. QDir::isAbsolutePath()
    // may already report "/tmp" as absolute on Windows, so the leading-slash
    // test comes first and deliberately ignores it. UNC paths ("\\server\..")
    // are left alone.
    if (!workspaceRoot.isEmpty() && !candidate.isEmpty()
        && (candidate.startsWith(u'/') || candidate.startsWith(u'\\'))
        && !candidate.startsWith(u"\\\\"_s)) {
        QString driveRoot;
        if (workspaceRoot.size() >= 2 && workspaceRoot.at(1) == u':') {
            driveRoot = workspaceRoot.left(3);
        } else {
            driveRoot = QDir::rootPath();
        }
        if (!driveRoot.endsWith(u'/') && !driveRoot.endsWith(u'\\')) {
            driveRoot += u'/';
        }
        // Strip every leading slash so "//tmp" does not produce "C://tmp".
        int lead = 0;
        while (lead < candidate.size()
               && (candidate.at(lead) == u'/' || candidate.at(lead) == u'\\')) {
            ++lead;
        }
        candidate = driveRoot + candidate.mid(lead);
    }
#else
    Q_UNUSED(workspaceRoot);
#endif
    return candidate;
}

QString Sandbox::normalizePath(const QString &path, QString *error, bool forWrite) const
{
    if (m_workspaceRoot.isEmpty()) {
        if (error) {
            *error = u"Workspace root is not set."_s;
        }
        return {};
    }

    QString candidate = path.trimmed();
    if (candidate.isEmpty()) {
        candidate = m_workspaceRoot;
    }
    candidate = windowsAbsoluteCandidate(m_workspaceRoot, candidate);

    const QFileInfo info(QDir(m_workspaceRoot), candidate);
    QString absolute = QDir::cleanPath(QDir::fromNativeSeparators(info.absoluteFilePath()));

    // The containment check comes first, on the lexical path. The canonical
    // paths below only exist when the file or its parent directory is already
    // on disk: a target whose directory does not exist yet has no canonical
    // path, and the path would leave the workspace unchecked.
    const bool mustStayInWorkspace = m_profile == SandboxProfile::Strict || (m_profile != SandboxProfile::Off && forWrite);
    if (mustStayInWorkspace && !pathInsideWorkspace(absolute)) {
        if (error) {
            *error = u"Path resolves outside the workspace: %1"_s.arg(absolute);
        }
        return {};
    }

    // Resolve symlinks safely using canonicalFilePath, then verify the result.
    // This narrows the classic escape (workspace/link -> /etc) to the window
    // between this check and the tool's own open(); a fully race-free check
    // would need the tool to open the path through an O_NOFOLLOW-style
    // handle, which the document bridge's Kate integration cannot offer.
    QFileInfo fileInfo(absolute);
    QString canonical = cleanCanonicalPath(fileInfo.canonicalFilePath());
    if (!canonical.isEmpty()) {
        // Verify the canonical path is still within workspace and not denied
        if (mustStayInWorkspace && !pathInsideWorkspace(canonical)) {
            if (error) {
                *error = u"Path resolves outside the workspace: %1"_s.arg(canonical);
            }
            return {};
        }
        if (isDenied(canonical)) {
            if (error) {
                *error = u"Access to this path is blocked by a deny rule: %1"_s.arg(canonical);
            }
            return {};
        }
        absolute = canonical;
    } else if (!fileInfo.exists()) {
        // File doesn't exist yet - resolve parent directory
        QString parent = fileInfo.absolutePath();
        QFileInfo parentInfo(parent);
        QString parentCanon = cleanCanonicalPath(parentInfo.canonicalFilePath());
        if (!parentCanon.isEmpty()) {
            absolute = QDir::cleanPath(parentCanon + u'/' + fileInfo.fileName());
            // Re-check workspace containment for new file path
            const bool mustStayInWorkspace = m_profile == SandboxProfile::Strict || (m_profile != SandboxProfile::Off && forWrite);
            if (mustStayInWorkspace && !pathInsideWorkspace(absolute)) {
                if (error) {
                    *error = u"Path resolves outside the workspace: %1"_s.arg(absolute);
                }
                return {};
            }
        }
    }

    if (isDenied(absolute)) {
        if (error) {
            *error = u"Access to this path is blocked by a deny rule: %1"_s.arg(absolute);
        }
        return {};
    }

    return absolute;
}

    QString Sandbox::resolve(const QString &path, QString *error, bool forWrite) const
    {
        return normalizePath(path, error, forWrite);
    }

    bool Sandbox::allowsRead(const QString &path, QString *error) const
    {
        if (m_profile == SandboxProfile::Off) {
            const QString resolvedPath = windowsAbsoluteCandidate(m_workspaceRoot, path);
            const QFileInfo info(QDir(m_workspaceRoot), resolvedPath);
            QString absolute = QDir::cleanPath(QDir::fromNativeSeparators(info.absoluteFilePath()));
            if (QFileInfo::exists(absolute)) {
                const QString canonical = cleanCanonicalPath(QFileInfo(absolute).canonicalFilePath());
                if (!canonical.isEmpty()) {
                    absolute = canonical;
                }
            } else {
                // Try to canonicalize parent
                const QString parent = QFileInfo(absolute).absolutePath();
                const QString parentCanon = cleanCanonicalPath(QFileInfo(parent).canonicalFilePath());
                if (!parentCanon.isEmpty()) {
                    absolute = QDir::cleanPath(parentCanon + u'/' + QFileInfo(absolute).fileName());
                }
            }
            if (isDenied(absolute)) {
                if (error) {
                    *error = u"Access to this path is blocked by a deny rule: %1"_s.arg(absolute);
                }
                return false;
            }
            return true;
        }

        if (m_profile == SandboxProfile::Workspace || m_profile == SandboxProfile::ReadOnly) {
            const QString resolvedPath = windowsAbsoluteCandidate(m_workspaceRoot, path);
            const QFileInfo info(QDir(m_workspaceRoot), resolvedPath);
            QString absolute = QDir::cleanPath(QDir::fromNativeSeparators(info.absoluteFilePath()));
            if (QFileInfo::exists(absolute)) {
                const QString canonical = cleanCanonicalPath(QFileInfo(absolute).canonicalFilePath());
                if (!canonical.isEmpty()) {
                    absolute = canonical;
                }
            } else {
                // Try to canonicalize parent directory for files that don't exist yet
                const QString parent = QFileInfo(absolute).absolutePath();
                const QString parentCanon = cleanCanonicalPath(QFileInfo(parent).canonicalFilePath());
                if (!parentCanon.isEmpty()) {
                    absolute = QDir::cleanPath(parentCanon + u'/' + QFileInfo(absolute).fileName());
                }
            }
            if (isDenied(absolute)) {
                if (error) {
                    *error = u"Access to this path is blocked by a deny rule: %1"_s.arg(absolute);
                }
                return false;
            }
            // The read tools are approved without a prompt, so a profile that
            // does not confine reads would let the agent read any file of the
            // host. The workspace is the default; reading outside it is an
            // explicit opt-in.
            if (!m_allowOutsideReads && !pathInsideWorkspace(absolute)) {
                if (error) {
                    *error = u"Path is outside the workspace: %1"_s.arg(absolute);
                }
                return false;
            }
            return true;
        }

        return !normalizePath(path, error, false).isEmpty();
    }

    bool Sandbox::allowsWrite(const QString &path, QString *error) const
    {
        if (m_profile == SandboxProfile::ReadOnly) {
            if (error) {
                *error = u"Sandbox is read-only; writes are not allowed."_s;
            }
            return false;
        }
        return !normalizePath(path, error, true).isEmpty();
    }

    QString Sandbox::primaryCommand(const QString &command)
    {
        QString rest = command.trimmed();
        static const QRegularExpression chain(uR"(\s*(?:&&|\|\||;|\||\n)\s*)"_s);
    rest = rest.split(chain).value(0).trimmed();

    if (rest.startsWith(u"sudo "_s)) {
        rest = rest.mid(5).trimmed();
    }

    QString token;
    if (rest.startsWith(u'"') || rest.startsWith(u'\'')) {
        const QChar quote = rest.at(0);
        const int end = rest.indexOf(quote, 1);
        token = rest.mid(1, end > 0 ? end - 1 : rest.size() - 1);
    } else {
        token = rest.section(u' ', 0, 0);
    }
    int slash = token.lastIndexOf(u'/');
    const int backslash = token.lastIndexOf(u'\\');
    if (backslash > slash) {
        slash = backslash;
    }
    if (slash >= 0) {
        token = token.mid(slash + 1);
    }
#ifdef Q_OS_WIN
    if (token.endsWith(u".exe"_s, Qt::CaseInsensitive) || token.endsWith(u".bat"_s, Qt::CaseInsensitive)
        || token.endsWith(u".cmd"_s, Qt::CaseInsensitive) || token.endsWith(u".com"_s, Qt::CaseInsensitive)) {
        token = token.left(token.lastIndexOf(u'.'));
    }
#endif
    return token;
    }

// True when the command can send output into a file. "2>&1" only duplicates a
// descriptor and is left alone, but ">", ">>" and "&>" all end bytes in a file,
// so the command is not read-only however harmless its executable looks.
static bool hasOutputRedirection(const QString &command)
{
    for (int i = 0; i < command.size(); ++i) {
        const QChar c = command.at(i);
        if (c == u'&' && i + 1 < command.size() && command.at(i + 1) == u'>') {
            return true; // &>file redirects both streams into a file
        }
        if (c != u'>') {
            continue;
        }
        const bool descriptorDuplication = i > 0 && command.at(i - 1).isDigit() && i + 1 < command.size()
            && command.at(i + 1) == u'&';
        if (!descriptorDuplication) {
            return true;
        }
    }
    return false;
}

// Flags that disqualify an otherwise read-only command from auto-approval.
// They either write to disk, or (tail -f) never terminate, which would burn the
// whole timeout of a turn that was approved without a second thought. Only
// commands already on the read-only list are checked against this, so a flag
// can never pull a new command onto the allowlist - it can only take one off it.
static bool hasWriteFlag(const QString &primary, const QString &command)
{
    static const QHash<QString, QStringList> writeFlags = {
        {u"find"_s,
         {u"-delete"_s, u"-exec"_s, u"-execdir"_s, u"-ok"_s, u"-okdir"_s,
          u"-fls"_s, u"-fprint"_s, u"-fprint0"_s, u"-fprintf"_s}},
        {u"sort"_s, {u"-o"_s, u"--output"_s, u"--compress-program"_s, u"--temporary-directory"_s}},
        // -s/--set writes the system clock; -f/--file only reads dates from a
        // file and stays read-only.
        {u"date"_s, {u"-s"_s, u"--set"_s}},
        {u"tail"_s, {u"-f"_s, u"-F"_s, u"--follow"_s}},
        // rg --pre runs an arbitrary program for every file it searches.
        {u"rg"_s, {u"--pre"_s}},
        // file -C compiles a magic database next to the source.
        {u"file"_s, {u"-C"_s, u"--compile"_s}},
    };
    const QStringList flags = writeFlags.value(primary);
    if (flags.isEmpty()) {
        return false;
    }
    const QStringList tokens = command.split(QRegularExpression(uR"(\s+)"_s), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        for (const QString &flag : flags) {
            // Match the bare flag, "--flag=value", and the attached short form
            // "-ovalue"/"-fn100". The last case used to slip past the exact
            // token comparison and let a "read-only" sort/tail write or hang.
            if (token == flag || token.startsWith(flag + u'=')) {
                return true;
            }
            if (flag.startsWith(u'-') && !flag.startsWith(u"--"_s)
                && token.startsWith(flag) && token.size() > flag.size()) {
                return true;
            }
        }
    }
    return false;
}

bool Sandbox::isReadOnlyCommand(const QString &command) const
{
    static const QStringList readOnly = {
        u"ls"_s,     u"cat"_s,     u"pwd"_s,     u"date"_s,      u"whoami"_s, u"hostname"_s, u"uptime"_s, u"ps"_s,
        u"head"_s,   u"tail"_s,    u"wc"_s,      u"sort"_s,      u"uniq"_s,   u"tr"_s,       u"cut"_s,    u"grep"_s,
        u"rg"_s,     u"find"_s,    u"file"_s,    u"stat"_s,      u"diff"_s,   u"echo"_s,     u"printf"_s, u"which"_s,
        // "env" is deliberately absent: with no command it prints the
        // environment, but "env bash -c …" executes an arbitrary program, so a
        // read-only allowlist entry would auto-approve code execution.
        // "printenv" only ever prints.
        u"type"_s,   u"printenv"_s, u"dir"_s,    u"where"_s,     u"where.exe"_s,
        u"Get-ChildItem"_s, u"Get-Content"_s, u"Get-Location"_s, u"Get-Date"_s, u"Get-Process"_s,
    };

    const QString cmd = command.trimmed();
    // Privilege escalation is never read-only, whatever the wrapped program
    // does. The old code stripped "sudo " before classifying, so "sudo cat
    // /etc/shadow" was treated as a plain cat and auto-approved in Ask mode.
    static const QRegularExpression escalation(
        uR"(^\s*(?:sudo|doas|pkexec)\b)"_s, QRegularExpression::CaseInsensitiveOption);
    if (escalation.match(cmd).hasMatch()) {
        return false;
    }
    static const QRegularExpression writers(
        uR"(\b(?:tee|rm|mv|cp|chmod|chown|mkdir|touch|dd|del|erase|rd|rmdir|copy|move|ren|rename|New-Item|Set-Content|Out-File|Remove-Item|Move-Item|Copy-Item)\b)"_s);
    if (writers.match(cmd).hasMatch()) {
        return false;
    }
    // A command substitution runs another program, whose name the writer list
    // cannot see: "cat \"$(python3 -c 'import os;os.system(\"…\")')\"" has a
    // read-only executable and writes anyway. The same holds for backticks and
    // for a backgrounded command, whose result is never inspected.
    if (cmd.contains(u"$("_s) || cmd.contains(u"`"_s) || cmd.contains(u"${"_s)) {
        return false;
    }
    // Process substitution runs a program inline, exactly like "$(…)" does:
    // "diff <(curl …) b" has a read-only executable but still executes (and
    // with it, any network or file access) what is inside the parens.
    if (cmd.contains(u"<("_s) || cmd.contains(u">("_s)) {
        return false;
    }
    // A single "&" backgrounds the command, so its result is never inspected.
    // "2>&1" is a redirection, not a background job, and "&&" chains commands
    // (which the chain handling below deals with).
    static const QRegularExpression background(uR"((?:^|[^>&\d])&(?!&))"_s);
    if (background.match(cmd).hasMatch()) {
        return false;
    }
    // The writers list only knows command names. "cat notes > ~/.bashrc" has a
    // read-only executable but still writes, so redirection decides as well.
    if (hasOutputRedirection(cmd)) {
        return false;
    }
    // A newline separates commands exactly like ';' does. Leaving it out made
    // "ls\npython3 -c '…'" a read-only "ls" that the shell then ran as two
    // commands, so the second one executed with no prompt.
    if (cmd.contains(u"&&"_s) || cmd.contains(u"||"_s) || cmd.contains(u';')
        || cmd.contains(u'|') || cmd.contains(u'\n') || cmd.contains(u'\r')) {
        const QStringList parts = cmd.split(QRegularExpression(uR"(\s*(?:&&|\|\||;|\||\r\n|\r|\n)\s*)"_s));
        if (parts.size() <= 1) {
            // The separator produced no split (e.g. an unknown control
            // character); do not recurse into the same string forever.
            return false;
        }
        for (const QString &part : parts) {
            if (!part.trimmed().isEmpty() && !isReadOnlyCommand(part)) {
                return false;
            }
        }
        return true;
    }

    const QString primary = primaryCommand(cmd);
    if (primary == u"git"_s) {
        const QString sub = cmd.trimmed().section(QRegularExpression(uR"(\s+)"_s), 1, 1);
        static const QStringList gitRead = {
            u"status"_s, u"log"_s, u"diff"_s, u"ls-files"_s, u"show"_s, u"rev-parse"_s,
            u"blame"_s,  u"describe"_s, u"shortlog"_s, u"cat-file"_s, u"ls-tree"_s, u"show-ref"_s, u"rev-list"_s,
        };
        // "branch" is not read-only: "git branch new" creates a ref and
        // "git branch -D main" deletes one. Git's --output options write a
        // file, and several subcommands accept it.
        if (!gitRead.contains(sub)) {
            return false;
        }
        const QStringList gitTokens = cmd.split(QRegularExpression(uR"(\s+)"_s), Qt::SkipEmptyParts);
        for (const QString &token : gitTokens) {
            if (token == u"--output"_s || token.startsWith(u"--output="_s)) {
                return false;
            }
        }
        return !hasWriteFlag(primary, cmd);
    }
    if (!readOnly.contains(primary)) {
        return false;
    }
    // The executable only reads, but a flag can still tell it to write:
    // "find . -delete" and "sort -o out.txt" both modify files, so Ask mode
    // must not treat them as read-only and auto-approve them.
    return !hasWriteFlag(primary, cmd);
}

bool Sandbox::isDangerousCommand(const QString &command) const
{
    const QString cmd = command.trimmed();
    static const QRegularExpression dangerous(
        uR"(\b(?:sudo|su|chmod\s+-R|chown\s+-R|mkfs|shutdown|reboot|systemctl|useradd|userdel|passwd|diskpart|format|cipher|reg\s+delete|Remove-Item\s+(-Recurse|-Force)|rmdir\s+/s|del\s+/s)\b)"_s,
        QRegularExpression::CaseInsensitiveOption);
    return dangerous.match(cmd).hasMatch() || isAlwaysDeniedCommand(cmd);
}

bool Sandbox::isAlwaysDeniedCommand(const QString &command) const
{
    const QString cmd = command.trimmed();
    // A nested shell hides the real command from the rm parser. Inspect the
    // script a shell is asked to run, so "sh -c 'rm -rf /'" is blocked too.
    const QStringList tokens = shellTokens(cmd);
    for (int i = 0; i + 2 < tokens.size(); ++i) {
        const QString program = QFileInfo(tokens.at(i)).fileName().toLower();
        if (tokens.at(i + 1) != u"-c"_s) {
            continue;
        }
        if (program != u"sh"_s && program != u"bash"_s && program != u"zsh"_s
            && program != u"dash"_s && program != u"ksh"_s && program != u"fish"_s) {
            continue;
        }
        QString script = tokens.at(i + 2);
        if (script.size() >= 2
            && ((script.front() == u'\'' && script.back() == u'\'')
                || (script.front() == u'"' && script.back() == u'"'))) {
            script = script.mid(1, script.size() - 2);
        }
        if (!script.trimmed().isEmpty() && script != cmd && isAlwaysDeniedCommand(script)) {
            return true;
        }
    }
    // The rm branch covers a recursive or forced removal of the filesystem
    // root, of everything directly below it ("rm -rf /*"), or of the home
    // directory ("rm -rf ~", "rm -rf ~/…"). It is parsed rather than matched
    // with one regular expression: the flags may appear in any order, "--" may
    // separate them from the target ("rm -rf -- /"), and the target may be
    // spelled with a shell variable ("rm -rf $HOME"). A single anchored
    // pattern missed those spellings, and a command that slipped past was not
    // even classified as dangerous.
    if (rmTargetsRootOrHome(cmd)) {
        return true;
    }
    // Everything else the policy blocks outright. Deeper absolute paths are
    // not matched here: they are a normal cleanup the permission prompt can
    // judge.
    static const QRegularExpression denied(
        uR"((\bmkfs\b)|(\bdd\s+.*\bof=/dev/)|(:\(\)\s*\{\s*:\|:&\s*;\s*\})|(\b(curl|wget)\b.*\|\s*(?:sudo\s+)?(sh|bash|zsh|fish|dash|ksh|cmd|cmd\.exe|powershell|powershell\.exe|pwsh|python[0-9.]*|perl|ruby|node)\b)|(\bformat\s+[a-zA-Z]:)|(\bdiskpart\b)|(\brmdir\s+/s\s+/q\s+[a-zA-Z]:\\)|(\bRemove-Item\s+.*-Recurse.*[A-Z]:\\))"_s,
        QRegularExpression::CaseInsensitiveOption);
    return denied.match(cmd).hasMatch();
}

QStringList Sandbox::expandGlobToken(const QString &pattern) const
{
    QStringList results;
    if (pattern.isEmpty() || m_workspaceRoot.isEmpty()) {
        return results;
    }

    const QString native = QDir::fromNativeSeparators(pattern);
    const bool absolute = native.startsWith(u'/');
    QStringList segments = native.split(u'/');
    if (absolute && !segments.isEmpty()) {
        segments.removeFirst();
    }
    // A trailing slash means "the directory itself", not its contents.
    while (!segments.isEmpty() && segments.last().isEmpty()) {
        segments.removeLast();
    }
    if (segments.isEmpty()) {
        return results;
    }

    // The base the pattern is relative to: the workspace root, because that is
    // the working directory of every sandboxed command.
    QStringList base;
    if (absolute) {
        base.append(u"/"_s);
    } else {
        base.append(m_workspaceRoot);
    }

    // Walk the segments one at a time. A literal segment is descended into, a
    // patterned one is matched against the directory listing.
    std::function<void(const QString &, int)> walk = [&](const QString &dir, int index) {
        if (index >= segments.size() || results.size() >= MaxGlobExpansionResults) {
            return;
        }
        const QString &segment = segments.at(index);
        const bool last = index == segments.size() - 1;
        QDir current(dir);
        if (!current.exists()) {
            return;
        }
        if (!containsGlobMeta(segment)) {
            const QString next = QDir(current).filePath(segment);
            if (last) {
                if (QFileInfo::exists(next)) {
                    results.append(next);
                }
            } else if (QFileInfo(next).isDir()) {
                walk(next, index + 1);
            }
            return;
        }
        const QRegularExpression nameRe(globToRegex(segment), globPatternOptions());
        const QFileInfoList entries = current.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo &entry : entries) {
            if (!nameRe.match(entry.fileName()).hasMatch()) {
                continue;
            }
            const QString next = entry.absoluteFilePath();
            if (last) {
                results.append(next);
            } else if (entry.isDir()) {
                walk(next, index + 1);
            }
            if (results.size() >= MaxGlobExpansionResults) {
                return;
            }
        }
    };

    for (const QString &part : base) {
        walk(part, 0);
    }
    return results;
}

bool Sandbox::patternCouldMatchDeniedName(const QString &pattern) const
{
    const QString native = QDir::fromNativeSeparators(pattern);
    const QStringList segments = native.split(u'/');
    const QString nameSegment = segments.isEmpty() ? pattern : segments.last();
    if (nameSegment.isEmpty()) {
        return false;
    }
    const QRegularExpression nameRe(globToRegex(nameSegment), globPatternOptions());
    for (const QString &glob : denyGlobs()) {
        const QStringList globSegments = QDir::fromNativeSeparators(glob).split(u'/');
        const QString denyName = globSegments.isEmpty() ? glob : globSegments.last();
        if (denyName.isEmpty()) {
            continue;
        }
        // The pattern can produce the denied name, so the command may read it.
        if (nameRe.match(denyName).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool Sandbox::commandTouchesDeniedPath(const QString &command) const
{
    if (m_workspaceRoot.isEmpty()) {
        return false;
    }
    // This is not a shell parser. It scans tokens and asks the deny rules about
    // each one, which is enough to stop "cat ~/.ssh/id_rsa" and "cat .env" from
    // walking around the guard the file tools apply. Over-blocking is the safe
    // direction here.
    const QStringList tokens = shellTokens(command);

    // The candidate a token names after the shell's own quoting/escaping, or
    // an empty string when the token is not path-shaped.
    auto candidateFor = [](QString token) -> QString {
        if (token.startsWith(u'-')) {
            const int eq = token.indexOf(u'=');
            const int colon = token.indexOf(u':');
            int valueStart = -1;
            if (eq >= 0 && (colon < 0 || eq < colon)) {
                valueStart = eq + 1;
            } else if (colon >= 0) {
                valueStart = colon + 1;
            }
            if (valueStart < 0 || valueStart >= token.size()) {
                return {};
            }
            token = token.mid(valueStart);
        }
        token.remove(u'"');
        token.remove(u'\'');
#ifndef Q_OS_WIN
        // sh turns ".\env" into ".env" and "a\ b" into "a b". The scanner used
        // to look at the backslash spelling and miss both.
        token.remove(u'\\');
#endif
        // A subshell or group does not change the path it contains.
        while (!token.isEmpty() && (token.front() == u'(' || token.front() == u'{')) {
            token.remove(0, 1);
        }
        while (!token.isEmpty()
               && (token.back() == u')' || token.back() == u'}' || token.back() == u',' || token.back() == u';')) {
            token.remove(token.size() - 1, 1);
        }
        if (token.isEmpty() || token == u"-"_s) {
            return {};
        }
        if (token == u"~"_s) {
            token = QDir::homePath();
        } else if (token.startsWith(u"~/"_s)) {
            token = QDir::homePath() + token.mid(1);
        }
        static const QRegularExpression homeVar(uR"(\$\{HOME\}|\$HOME(?![A-Za-z0-9_]))"_s);
        token.replace(homeVar, QDir::homePath());
        return token;
    };

    for (int tokenIndex = 0; tokenIndex < tokens.size(); ++tokenIndex) {
        const QString raw = tokens.at(tokenIndex);
        const QString program = QFileInfo(raw).fileName().toLower();
        // A nested shell hides the real command in the next token. Inspect the
        // script itself, not the quoted string as if it were one filename.
        if (tokenIndex + 2 < tokens.size() && tokens.at(tokenIndex + 1) == u"-c"_s
            && (program == u"sh"_s || program == u"bash"_s || program == u"zsh"_s
                || program == u"dash"_s || program == u"ksh"_s || program == u"fish"_s)) {
            QString script = tokens.at(tokenIndex + 2);
            if (script.size() >= 2
                && ((script.front() == u'\'' && script.back() == u'\'')
                    || (script.front() == u'"' && script.back() == u'"'))) {
                script = script.mid(1, script.size() - 2);
            }
            if (commandTouchesDeniedPath(script)) {
                return true;
            }
            continue;
        }
        if (tokenIndex + 1 < tokens.size() && (program == u"eval"_s || program == u"command"_s)) {
            if (commandTouchesDeniedPath(tokens.at(tokenIndex + 1))) {
                return true;
            }
            continue;
        }

        const QString candidate = candidateFor(raw);
        if (candidate.isEmpty()) {
            continue;
        }
        // A glob token names a set of files, not one path. Checking the pattern
        // text alone lets "cat .e*" past the "**/.env" rule while the shell
        // expands it to the dotfile, so the pattern is expanded against the
        // filesystem and every match is checked.
        if (containsGlobMeta(candidate)) {
            const QStringList expanded = expandGlobToken(candidate);
            if (expanded.isEmpty()) {
                // Nothing on disk matches, so there is nothing to read either -
                // but the pattern could still cover a denied name, and
                // over-blocking is the safe direction here.
                if (patternCouldMatchDeniedName(candidate)) {
                    return true;
                }
            } else if (expanded.size() >= MaxGlobExpansionResults) {
                // The expansion was truncated. A denied file beyond the cap
                // would be invisible to the scan, so fail closed.
                return true;
            }
            for (const QString &match : expanded) {
                const QString absolute = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(match).absoluteFilePath()));
                if (isDenied(absolute)) {
                    return true;
                }
                const QString resolved = normalizePath(absolute, nullptr, false);
                if (!resolved.isEmpty() && resolved != absolute && isDenied(resolved)) {
                    return true;
                }
            }
        }
        const QString adjustedCandidate = windowsAbsoluteCandidate(m_workspaceRoot, candidate);
        // Check the path as written, then the symlink-resolved form, so a denied
        // name is caught even when the file does not exist yet.
        const QString literal = QDir::cleanPath(
            QDir::fromNativeSeparators(QFileInfo(QDir(m_workspaceRoot), adjustedCandidate).absoluteFilePath()));
        if (isDenied(literal)) {
            return true;
        }
        // Check both read and write resolution - shell commands can write via
        // redirections (>, >>, &>) even if the command itself is read-only.
        const QString resolvedRead = normalizePath(adjustedCandidate, nullptr, false);
        if (!resolvedRead.isEmpty() && resolvedRead != literal && isDenied(resolvedRead)) {
            return true;
        }
        const QString resolvedWrite = normalizePath(adjustedCandidate, nullptr, true);
        if (!resolvedWrite.isEmpty() && resolvedWrite != literal && resolvedWrite != resolvedRead && isDenied(resolvedWrite)) {
            return true;
        }
    }
    return false;
}

bool Sandbox::commandTouchesOutsideWorkspace(const QString &command) const
{
    // The setting that permits reads outside the workspace also covers the
    // shell, and an Off profile makes no confinement promise at all.
    if (m_allowOutsideReads || m_profile == SandboxProfile::Off || m_workspaceRoot.isEmpty()) {
        return false;
    }
    const QStringList tokens = shellTokens(command);
    for (int tokenIndex = 0; tokenIndex < tokens.size(); ++tokenIndex) {
        const QString raw = tokens.at(tokenIndex);
        const QString program = QFileInfo(raw).fileName().toLower();
        if (tokenIndex + 2 < tokens.size() && tokens.at(tokenIndex + 1) == u"-c"_s
            && (program == u"sh"_s || program == u"bash"_s || program == u"zsh"_s
                || program == u"dash"_s || program == u"ksh"_s || program == u"fish"_s)) {
            QString script = tokens.at(tokenIndex + 2);
            if (script.size() >= 2
                && ((script.front() == u'\'' && script.back() == u'\'')
                    || (script.front() == u'"' && script.back() == u'"'))) {
                script = script.mid(1, script.size() - 2);
            }
            if (commandTouchesOutsideWorkspace(script)) {
                return true;
            }
            continue;
        }
        QString candidate = raw;
        if (candidate.startsWith(u'-')) {
            const int eq = candidate.indexOf(u'=');
            if (eq < 0 || eq + 1 >= candidate.size()) {
                continue;
            }
            candidate = candidate.mid(eq + 1);
        }
        candidate.remove(u'"');
        candidate.remove(u'\'');
#ifndef Q_OS_WIN
        candidate.remove(u'\\');
#endif
        while (!candidate.isEmpty() && (candidate.front() == u'(' || candidate.front() == u'{')) {
            candidate.remove(0, 1);
        }
        while (!candidate.isEmpty() && (candidate.back() == u')' || candidate.back() == u'}')) {
            candidate.remove(candidate.size() - 1, 1);
        }
        if (candidate.isEmpty() || candidate == u"-"_s || candidate.contains(u"://"_s)) {
            continue;
        }
        if (candidate == u"~"_s) {
            candidate = QDir::homePath();
        } else if (candidate.startsWith(u"~/"_s)) {
            candidate = QDir::homePath() + candidate.mid(1);
        }
        static const QRegularExpression homeVar(uR"(\$\{HOME\}|\$HOME(?![A-Za-z0-9_]))"_s);
        candidate.replace(homeVar, QDir::homePath());
        // Only path-shaped operands matter: a bare word is a pattern/option,
        // not a path the shell will open. A dot-leading name is path-shaped
        // because that is how .env and friends are spelled.
        const bool pathShaped = candidate.startsWith(u'/') || candidate.contains(u'/')
            || candidate.startsWith(u'.') || QDir(candidate).isAbsolute();
        if (!pathShaped || candidate.startsWith(u':' /* Gentoo-style colon paths */)) {
            continue;
        }
        if (containsGlobMeta(candidate)) {
            const QStringList expanded = expandGlobToken(candidate);
            if (expanded.size() >= MaxGlobExpansionResults) {
                return true;
            }
            for (const QString &match : expanded) {
                const QString absolute = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(match).absoluteFilePath()));
                if (!pathInsideWorkspace(absolute)) {
                    return true;
                }
            }
            continue;
        }
        const QString adjusted = windowsAbsoluteCandidate(m_workspaceRoot, candidate);
        const QString absolute = QDir::cleanPath(
            QDir::fromNativeSeparators(QFileInfo(QDir(m_workspaceRoot), adjusted).absoluteFilePath()));
        if (!pathInsideWorkspace(absolute)) {
            return true;
        }
    }
    return false;
}

static QString firstExisting(const QStringList &candidates)
{
    for (const QString &path : candidates) {
        if (path.isEmpty()) {
            continue;
        }
        const QFileInfo info(path);
        if (info.exists() && info.isExecutable()) {
            return path;
        }
    }
    return {};
}

static QString findShellExecutable()
{
#ifdef Q_OS_WIN
    const QStringList names = {u"bash"_s, u"sh"_s, u"pwsh"_s, u"powershell"_s, u"cmd"_s};
    for (const QString &name : names) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) {
            return found;
        }
    }
    const QString extra = firstExisting({
        u"C:/Program Files/Git/bin/bash.exe"_s,
        u"C:/Program Files (x86)/Git/bin/bash.exe"_s,
        u"C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"_s,
        u"C:/Windows/System32/cmd.exe"_s,
    });
    return extra.isEmpty() ? u"cmd.exe"_s : extra;
#else
    QString shell = QStandardPaths::findExecutable(u"sh"_s);
    if (shell.isEmpty()) {
        shell = firstExisting({u"/bin/sh"_s, u"/usr/bin/sh"_s, u"/usr/local/bin/sh"_s});
    }
    return shell.isEmpty() ? u"/bin/sh"_s : shell;
#endif
}

static QString posixShellQuote(const QString &value)
{
    if (value.isEmpty()) {
        return u"''"_s;
    }
    static const QRegularExpression safe(u"^[A-Za-z0-9_./:@%+=,-]+$"_s);
    if (safe.match(value).hasMatch()) {
        return value;
    }
    QString escaped = value;
    escaped.replace(u"'"_s, u"'\\''"_s);
    return u"'"_s + escaped + u"'"_s;
}

static QString argvAsShellLine(const QString &program, const QStringList &args)
{
    QStringList parts{posixShellQuote(program)};
    for (const QString &arg : args) {
        parts.append(posixShellQuote(arg));
    }
    return parts.join(u' ');
}

static QStringList unsandboxedArgv(const QString &command)
{
    const QString shell = findShellExecutable();
#ifdef Q_OS_WIN
    const QString base = QFileInfo(shell).completeBaseName();
    if (base.compare(u"cmd"_s, Qt::CaseInsensitive) == 0) {
        return {shell, u"/S"_s, u"/C"_s, command};
    }
    if (base.compare(u"powershell"_s, Qt::CaseInsensitive) == 0 || base.compare(u"pwsh"_s, Qt::CaseInsensitive) == 0) {
        return {shell, u"-NoProfile"_s, u"-NonInteractive"_s, u"-Command"_s, command};
    }
#endif
    return {shell, u"-lc"_s, command};
}

#if defined(Q_OS_MACOS)
static QString seatbeltQuote(const QString &path)
{
    QString quoted = path;
    quoted.replace(QChar(u'\\'), u"\\\\"_s);
    quoted.replace(QChar(u'"'), u"\\\""_s);
    return quoted;
}
#endif

// Used by the cross-platform working-directory confinement check in
// wrapCommand() and wrapArgv(), so it must not live behind a platform guard.
static bool pathIsWithinOrEqual(const QString &path, const QString &root)
{
    if (path.isEmpty() || root.isEmpty()) {
        return false;
    }
    const QString normalizedPath = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QString normalizedRoot = QDir::cleanPath(QDir::fromNativeSeparators(root));
    return normalizedPath == normalizedRoot || normalizedPath.startsWith(normalizedRoot + u'/');
}

#if defined(Q_OS_UNIX)
// The home directory a strict sandbox gets. It is a fresh, empty directory
// inside the sandbox's private tmpfs, so commands cannot read the user's
// dotfiles or write caches into the real home.
static QString sandboxHomePath()
{
#if defined(Q_OS_MACOS)
    return QDir::cleanPath(QDir::tempPath() + u"/kateai-home"_s);
#else
    return u"/tmp/kateai-home"_s;
#endif
}

// A private runtime directory for sockets a command creates itself. The host
// runtime directory is never mounted: it contains the session bus, the SSH and
// GPG agents, and the display-server sockets.
static QString sandboxRuntimePath(SandboxProfile profile)
{
#if defined(Q_OS_MACOS)
    Q_UNUSED(profile);
    return QDir::cleanPath(QDir::tempPath() + u"/kateai-runtime"_s);
#else
    return profile == SandboxProfile::Strict ? sandboxHomePath() + u"/run"_s
                                             : u"/tmp/kateai-runtime"_s;
#endif
}

// Credential stores and session channels that must not be reachable from a
// sandboxed command. Deny globs cover the file tools and command arguments;
// this list closes the dynamic-path hole ("cat ~/$d/id_rsa") by mounting an
// empty view over the real path. Only existing paths are returned.
static QStringList sensitiveSandboxPaths()
{
    QStringList paths;
    const QString home = QDir::homePath();

    static const QStringList homeDirectories = {
        u".ssh"_s,
        u".gnupg"_s,
        u".aws"_s,
        u".azure"_s,
        u".config/gcloud"_s,
        u".config/gh"_s,
        u".config/kateai"_s,
        u".docker"_s,
        u".kube"_s,
        u".password-store"_s,
        u".local/share/keyrings"_s,
        u".local/share/kateai"_s,
        u".mozilla"_s,
        u".config/google-chrome"_s,
        u".config/chromium"_s,
        u".config/BraveSoftware"_s,
        u".config/microsoft-edge"_s,
        u".config/containers"_s,
        u".config/hub"_s,
        u".config/heroku"_s,
        u".terraform.d"_s,
    };
    for (const QString &relative : homeDirectories) {
        const QString path = home + u'/' + relative;
        if (QFileInfo(path).isDir()) {
            paths.append(path);
        }
    }

    // XDG_CONFIG_HOME / XDG_DATA_HOME can move the KateAI config and the
    // fallback credential store away from ~/.config and ~/.local/share.
    QStringList xdgKateAiDirs;
    const QString xdgConfig = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString xdgData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!xdgConfig.isEmpty()) {
        xdgKateAiDirs.append(xdgConfig + u"/kateai"_s);
    }
    if (!xdgData.isEmpty()) {
        xdgKateAiDirs.append(xdgData + u"/kateai"_s);
    }
    for (const QString &path : xdgKateAiDirs) {
        if (QFileInfo(path).isDir() && !paths.contains(path)) {
            paths.append(path);
        }
    }

    static const QStringList homeFiles = {
        u".netrc"_s,
        u".git-credentials"_s,
        u".npmrc"_s,
        u".pypirc"_s,
        u".cargo/credentials"_s,
        u".cargo/credentials.toml"_s,
        u".config/gh/hosts.yml"_s,
        u".config/gcloud/application_default_credentials.json"_s,
        u".docker/config.json"_s,
        u".kube/config"_s,
        u".vault-token"_s,
        u".ansible/galaxy_token"_s,
        u".gradle/gradle.properties"_s,
        u".m2/settings.xml"_s,
        u".nuget/NuGet/NuGet.Config"_s,
        u".local/share/kateai/credentials.json"_s,
        u".local/share/kateai/credentials.conf"_s,
    };
    for (const QString &relative : homeFiles) {
        const QString path = home + u'/' + relative;
        if (QFileInfo(path).isFile()) {
            paths.append(path);
        }
    }

#if defined(Q_OS_UNIX)
    // The per-user runtime directory holds the session bus, PipeWire/PulseAudio
    // sockets, the GPG agent and the SSH agent. Use the environment value only
    // when it belongs to this user: a stale XDG_RUNTIME_DIR from another login
    // must not make us mount a tmpfs over someone else's directory.
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty() || QFileInfo(runtime).ownerId() != getuid()) {
        runtime = u"/run/user/"_s + QString::number(getuid());
    }
    if (QFileInfo(runtime).isDir() && QFileInfo(runtime).ownerId() == getuid()) {
        paths.append(runtime);
    }
#endif

    static const QStringList systemSockets = {
        u"/run/dbus"_s,
        u"/run/docker.sock"_s,
        u"/run/podman"_s,
        u"/run/containers"_s,
        u"/run/snapd.socket"_s,
        u"/run/NetworkManager"_s,
        u"/run/wpa_supplicant"_s,
        u"/run/systemd/private"_s,
        u"/run/systemd/journal/socket"_s,
    };
    for (const QString &path : systemSockets) {
        if (QFileInfo::exists(path)) {
            paths.append(path);
        }
    }

    // Resolve symlinks and de-duplicate: /var/run is usually a link to /run,
    // and mounting the same target twice makes bubblewrap fail.
    QStringList unique;
    QSet<QString> seen;
    for (const QString &path : paths) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        const QString key = canonical.isEmpty() ? QDir::cleanPath(path) : QDir::cleanPath(canonical);
        if (!seen.contains(key)) {
            seen.insert(key);
            unique.append(canonical.isEmpty() ? path : canonical);
        }
    }
    return unique;
}
#endif

#if defined(Q_OS_LINUX)
// A stable, owner-only empty file used to blank out a single credential file
// inside the sandbox. A directory is masked with tmpfs instead.
static QString sandboxEmptyMaskFile()
{
    static const QString path = []() -> QString {
        QString directory = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
        if (directory.isEmpty()) {
            directory = QDir::homePath() + u"/.cache"_s;
        }
        directory += u"/kateai"_s;
        if (!QDir().mkpath(directory)) {
            return {};
        }
        QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QString file = directory + u"/sandbox-empty"_s;
        QFile empty(file);
        if (!empty.exists() && empty.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            empty.close();
        }
        if (!QFileInfo(file).isFile()) {
            return {};
        }
        QFile::setPermissions(file, QFile::ReadOwner);
        return file;
    }();
    return path;
}

static void appendLinuxMaskMount(QStringList &args, const QString &path, int depth = 0)
{
    if (depth > 3) {
        return;
    }
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return;
    }
    // Mount over the symlink target, not the link itself: binding a tmpfs over
    // a symlink can hide an unrelated directory after resolution.
    if (info.isSymLink()) {
        const QString target = info.canonicalFilePath();
        if (!target.isEmpty() && QDir::cleanPath(target) != QDir::cleanPath(path)) {
            appendLinuxMaskMount(args, target, depth + 1);
        }
        return;
    }
    if (info.isDir()) {
        args << u"--tmpfs"_s << QDir::cleanPath(path);
        return;
    }
    // Sockets, FIFOs and device nodes are not files, but they are just as
    // sensitive (the Docker/Podman socket is root-equivalent). Any existing
    // non-directory is blanked with an empty regular file.
    const QString empty = sandboxEmptyMaskFile();
    if (!empty.isEmpty()) {
        args << u"--ro-bind"_s << empty << QDir::cleanPath(path);
    } else {
        // The empty-file helper needs a writable cache directory. /dev/null is
        // a safe fallback even when reading the mask yields EACCES: the point
        // is that the original path is no longer reachable.
        args << u"--ro-bind"_s << u"/dev/null"_s << QDir::cleanPath(path);
    }
}
#endif

#if defined(Q_OS_LINUX)
// Bind a runtime directory, or recreate a runtime symlink. Distributions differ
// on whether /bin, /lib and /lib64 are real directories or links into /usr;
// both forms have to look the same inside the sandbox.
static void appendBubblewrapRuntimePath(QStringList &args, const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink()) {
        const QString target = info.symLinkTarget();
        if (!target.isEmpty()) {
            args << u"--symlink"_s << target << path;
        }
    } else if (info.isDir()) {
        args << u"--ro-bind"_s << path << path;
    }
}
#endif

#if defined(Q_OS_LINUX)
// True when the current user may open the directory for reading, which is
// more than the execute permission a plain path traversal needs.
static bool directoryOpensForReading(const QString &path)
{
    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    return true;
}

// The ancestors of a mount destination, outermost first, without the root and
// without the destination itself. These are exactly the directories bubblewrap
// opens before it resolves the mount point.
static QStringList mountParentChain(const QString &destination)
{
    QStringList chain;
    QString path = QDir::cleanPath(QDir::fromNativeSeparators(destination));
    while (true) {
        const QString parent = QFileInfo(path).absolutePath();
        if (parent.isEmpty() || parent == path || parent == u"/"_s) {
            break;
        }
        chain.prepend(parent);
        path = parent;
    }
    return chain;
}

// bubblewrap 0.12 opens every parent directory of a mount point with
// O_DIRECTORY, so it needs read permission on the whole chain and not just the
// search permission a path traversal needs. A home directory below a directory
// the user may only traverse (pam_tmpdir's a+x, a shared home) therefore made
// every mount inside it fail with "Can't mkdir parents ... Permission denied".
// For each ancestor bubblewrap cannot open, cover it with a private tmpfs and
// bind the child leading to the destination back read-only: the destination
// stays reachable and can still be masked or rebound writable afterwards,
// while nothing else from the unreadable directory is exposed.
static QStringList mountableDestinationPrep(const QStringList &destinations)
{
    QStringList blockers;
    QHash<QString, QStringList> restore;
    for (const QString &destination : destinations) {
        // /tmp is a private tmpfs by the time any of these mounts is created,
        // so a destination below it only ever walks directories we made.
        if (pathIsWithinOrEqual(destination, u"/tmp"_s)) {
            continue;
        }
        for (const QString &ancestor : mountParentChain(destination)) {
            if (directoryOpensForReading(ancestor)) {
                continue;
            }
            if (!restore.contains(ancestor)) {
                blockers.append(ancestor);
                restore.insert(ancestor, {});
            }
            const QString relative = destination.mid(ancestor.size() + 1);
            const QString child = ancestor + u'/' + relative.section(u'/', 0, 0);
            if (!restore[ancestor].contains(child)) {
                restore[ancestor].append(child);
            }
            break;
        }
    }
    // An outer blocker is restored first; a deeper one is only ever visible
    // after its own ancestors were brought back.
    std::sort(blockers.begin(), blockers.end(), [](const QString &left, const QString &right) {
        return left.size() < right.size();
    });
    QStringList args;
    for (const QString &ancestor : blockers) {
        args << u"--tmpfs"_s << ancestor;
        const QStringList children = restore.value(ancestor);
        for (const QString &child : children) {
            args << u"--ro-bind"_s << child << child;
        }
    }
    return args;
}
#endif

#if defined(Q_OS_LINUX)
static QStringList linuxBubblewrapPrefix(const QString &workspaceRoot, SandboxProfile profile,
                                        const QString &workingDirectory, QString *error)
{
    const QString bwrap = QStandardPaths::findExecutable(u"bwrap"_s);
    if (bwrap.isEmpty()) {
        if (error) {
            *error = u"bubblewrap (bwrap) is required for sandboxed commands and was not found."_s;
        }
        return {};
    }
    if (workspaceRoot.trimmed().isEmpty()) {
        if (error) {
            *error = u"Workspace root is not set; refusing to run a sandboxed command."_s;
        }
        return {};
    }

    const bool strict = profile == SandboxProfile::Strict;
    const bool readOnlyFs = profile == SandboxProfile::ReadOnly;
    QString workspace = QDir::cleanPath(QDir::fromNativeSeparators(workspaceRoot));
    const QString canonicalWorkspace = QFileInfo(workspace).canonicalFilePath();
    if (!canonicalWorkspace.isEmpty()) {
        workspace = QDir::cleanPath(QDir::fromNativeSeparators(canonicalWorkspace));
    }
    if (strict && workspace == QDir::rootPath()) {
        if (error) {
            *error = u"The strict profile cannot use the filesystem root as its workspace."_s;
        }
        return {};
    }
    // The strict profile has a writable tmpfs root, so it can create the
    // stable mount point. The other profiles mount the host root read-only,
    // where /workspace does not exist; a private /tmp tmpfs is writable and
    // gives them a safe place for the shallow mount.
    const QString workspaceMount = strict ? u"/workspace"_s : u"/tmp/kateai-workspace"_s;
    QString chdir = workspaceMount;
    if (!workingDirectory.trimmed().isEmpty()) {
        QString cwd = QDir::cleanPath(QDir::fromNativeSeparators(workingDirectory));
        const QString canonical = QFileInfo(cwd).canonicalFilePath();
        if (!canonical.isEmpty()) {
            cwd = QDir::cleanPath(QDir::fromNativeSeparators(canonical));
        }
        if (!QFileInfo(cwd).isDir() || !pathIsWithinOrEqual(cwd, workspace)) {
            if (error) {
                *error = u"The working directory is not a directory inside the workspace: %1"_s.arg(workingDirectory);
            }
            return {};
        }
        chdir = cwd;
    }

    QStringList args;
    args << bwrap
         // A new session blocks TIOCSTI-style injection into the terminal that
         // started Kate, and --die-with-parent makes cancellation reliable.
         << u"--die-with-parent"_s << u"--new-session"_s
         // A user namespace gives the sandbox an unmapped-uid view even when
         // bubblewrap is not setuid. -try keeps setuid installations working on
         // kernels that disable unprivileged user namespaces.
         << u"--unshare-user-try"_s
         << u"--unshare-pid"_s << u"--unshare-ipc"_s << u"--unshare-uts"_s
         << u"--unshare-cgroup-try"_s
         // A sandboxed build never needs capabilities; dropping them also keeps
         // a setuid bubblewrap from retaining anything after setup.
         << u"--cap-drop"_s << u"ALL"_s
         // The UTS namespace is already unshared; a stable hostname keeps the
         // real one out of command output.
         << u"--hostname"_s << u"kateai-sandbox"_s;

    if (strict || readOnlyFs) {
        args << u"--unshare-net"_s;
    }

    if (strict) {
        // A tmpfs root means the host filesystem is not mounted at all. Only
        // the runtime needed to start and run programs is exposed read-only.
        args << u"--tmpfs"_s << u"/"_s;
        static const QStringList runtimePaths = {
            u"/usr"_s, u"/bin"_s, u"/sbin"_s, u"/lib"_s, u"/lib64"_s,
            u"/lib32"_s, u"/libx32"_s, u"/etc"_s, u"/opt"_s, u"/nix"_s,
            u"/snap"_s,
        };
        for (const QString &path : runtimePaths) {
            if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
                appendBubblewrapRuntimePath(args, path);
            }
        }
    } else {
        // Workspace and ReadOnly can read the host, but never write it: the
        // root is mounted read-only and the workspace is rebound below.
        args << u"--ro-bind"_s << u"/"_s << u"/"_s;
    }

    args << u"--proc"_s << u"/proc"_s << u"--dev"_s << u"/dev"_s;
    // A private /tmp for every profile. Binding the host /tmp made the profile
    // table's "workspace only" promise false and left files that outlive the
    // command visible to every process on the machine.
    args << u"--tmpfs"_s << u"/tmp"_s;

    // The credential stores that are hidden inside the sandbox. They are
    // collected before the workspace mounts because the mount points below may
    // need their ancestors restored first.
    QStringList maskedPaths;
    if (!strict) {
        for (const QString &path : sensitiveSandboxPaths()) {
            // Masking an ancestor of the workspace would hide the project.
            if (pathIsWithinOrEqual(workspace, path)) {
                continue;
            }
            maskedPaths.append(path);
        }
    }

    // A mount destination below a directory that may only be traversed cannot
    // be created by bubblewrap; restore those ancestors first. The strict
    // profile builds its own root, so its parents are directories we create.
    if (!strict) {
        QStringList mountDestinations = maskedPaths;
        if (workspace != workspaceMount) {
            mountDestinations.append(workspace);
        }
        args << mountableDestinationPrep(mountDestinations);
    }

    // The workspace is available under a stable, shallow path and under its
    // real absolute path, so commands that spell out the project directory keep
    // working. Both point at the same bind mount.
    const QString workspaceMode = readOnlyFs ? u"--ro-bind"_s : u"--bind"_s;
    if (workspace != workspaceMount) {
        args << workspaceMode << workspace << workspace;
    }
    args << workspaceMode << workspace << workspaceMount;

    // The strict profile has no host home mounted, so it needs a fresh one.
    // The other profiles keep the real HOME for tool configuration, but the
    // sensitive stores inside it are hidden.
    if (strict) {
        const QString home = sandboxHomePath();
        const QString runtime = sandboxRuntimePath(profile);
        args << u"--dir"_s << home
             << u"--dir"_s << home + u"/.cache"_s
             << u"--dir"_s << home + u"/.config"_s
             << u"--dir"_s << home + u"/.local"_s
             << u"--dir"_s << home + u"/.local/share"_s
             << u"--dir"_s << home + u"/.local/state"_s
             << u"--dir"_s << runtime;
    } else {
        args << u"--dir"_s << sandboxRuntimePath(profile);
        for (const QString &path : maskedPaths) {
            appendLinuxMaskMount(args, path);
        }
    }

    args << u"--chdir"_s << chdir;

    // These override ambient values even when a caller started bwrap directly
    // instead of going through Sandbox::processEnvironment().
    args << u"--setenv"_s << u"TMPDIR"_s << u"/tmp"_s
         << u"--setenv"_s << u"XDG_RUNTIME_DIR"_s << sandboxRuntimePath(profile);
    if (strict) {
        const QString home = sandboxHomePath();
        QString path = u"/usr/local/bin:/usr/bin:/bin"_s;
        const QString nixProfile = u"/nix/var/nix/profiles/default/bin"_s;
        if (QFileInfo(nixProfile).isDir()) {
            path.prepend(nixProfile + u':');
        }
        args << u"--setenv"_s << u"HOME"_s << home
             << u"--setenv"_s << u"XDG_CACHE_HOME"_s << home + u"/.cache"_s
             << u"--setenv"_s << u"XDG_CONFIG_HOME"_s << home + u"/.config"_s
             << u"--setenv"_s << u"XDG_DATA_HOME"_s << home + u"/.local/share"_s
             << u"--setenv"_s << u"XDG_STATE_HOME"_s << home + u"/.local/state"_s
             << u"--setenv"_s << u"PATH"_s << path;
    }
    return args;
}

static QStringList wrapLinuxBubblewrap(const QString &workspaceRoot, SandboxProfile profile,
                                      const QString &command, const QString &workingDirectory, QString *error)
{
    QStringList args = linuxBubblewrapPrefix(workspaceRoot, profile, workingDirectory, error);
    if (args.isEmpty()) {
        return {};
    }
    // -c, not -lc: a login shell reads the user's profile, which could
    // re-export the credentials and session channels the environment policy
    // just removed. Off keeps -lc for compatibility.
    args << u"--"_s << findShellExecutable() << u"-c"_s << command;
    return args;
}

static QStringList wrapLinuxBubblewrapArgv(const QString &workspaceRoot, SandboxProfile profile,
                                          const QString &program, const QStringList &argv,
                                          const QString &workingDirectory, QString *error)
{
    QStringList args = linuxBubblewrapPrefix(workspaceRoot, profile, workingDirectory, error);
    if (args.isEmpty()) {
        return {};
    }
    args << u"--"_s << program;
    args += argv;
    return args;
}
#endif

#if defined(Q_OS_MACOS)
static QString macSandboxProfile(const QString &workspaceRoot, SandboxProfile profile)
{
    const QString ws = seatbeltQuote(QDir::cleanPath(workspaceRoot));
    QString profileText;

    if (profile == SandboxProfile::Strict) {
        // Deny by default, then allow only the runtime needed to execute
        // programs and touch the workspace and private temporary directories.
        profileText = u"(version 1)\n"
                      "(deny default)\n"
                      "(allow process*)\n"
                      "(allow signal)\n"
                      "(allow sysctl-read)\n"
                      "(allow mach-lookup)\n"
                      "(allow ipc-posix-shm)\n"
                      "(allow file-read-metadata)\n"_s;
        profileText += u"(allow file-read* (subpath \"/usr\") (subpath \"/bin\") (subpath \"/sbin\") "
                       "(subpath \"/opt\") (subpath \"/Library\") (subpath \"/System\") "
                       "(subpath \"/private/etc\") (subpath \"/etc\") "
                       "(subpath \"/private/var/db\") (subpath \"/private/var/folders\") "
                       "(subpath \"/var/folders\") (subpath \"/private/tmp\") (subpath \"/tmp\") "
                       "(subpath \"/dev\") (subpath \""_s
                      + ws + u"\"))\n"_s;
        profileText += u"(allow file-write* (subpath \""_s + ws
                       + u"\") (subpath \"/private/tmp\") (subpath \"/tmp\") "
                         "(subpath \"/private/var/folders\") (subpath \"/var/folders\") "
                         "(subpath \"/dev\"))\n"
                         "(deny network*)\n"_s;
        return profileText;
    }

    // Workspace can write only the workspace and temporary directories;
    // ReadOnly can write only temporary directories (never the workspace).
    // Sensitive credential stores are denied after the broad allow rule.
    profileText = u"(version 1)\n(allow default)\n(deny file-write*)\n"_s;
    if (profile == SandboxProfile::ReadOnly) {
        profileText += u"(allow file-write* (subpath \"/tmp\") (subpath \"/private/tmp\") "
                         "(subpath \"/private/var/folders\") (subpath \"/var/folders\") "
                         "(subpath \"/dev\"))\n"_s;
    } else {
        profileText += u"(allow file-write* (subpath \""_s + ws
                       + u"\") (subpath \"/tmp\") (subpath \"/private/tmp\") "
                         "(subpath \"/private/var/folders\") (subpath \"/var/folders\") "
                         "(subpath \"/dev\"))\n"_s;
    }
    for (const QString &path : sensitiveSandboxPaths()) {
        if (pathIsWithinOrEqual(QDir::cleanPath(workspaceRoot), path)) {
            continue;
        }
        const QString quoted = seatbeltQuote(path);
        if (QFileInfo(path).isDir()) {
            profileText += u"(deny file-read* (subpath \""_s + quoted + u"\"))\n"_s;
            profileText += u"(deny file-write* (subpath \""_s + quoted + u"\"))\n"_s;
        } else {
            profileText += u"(deny file-read* (literal \""_s + quoted + u"\"))\n"_s;
            profileText += u"(deny file-write* (literal \""_s + quoted + u"\"))\n"_s;
        }
    }
    if (profile == SandboxProfile::ReadOnly) {
        profileText += u"(deny network*)\n"_s;
    }
    return profileText;
}

static QStringList macSandboxPrefix(const QString &workspaceRoot, SandboxProfile profile, QString *error)
{
    QString sandboxExec = QStandardPaths::findExecutable(u"sandbox-exec"_s);
    if (sandboxExec.isEmpty()) {
        sandboxExec = u"/usr/bin/sandbox-exec"_s;
    }
    if (!QFileInfo(sandboxExec).isExecutable()) {
        if (error) {
            *error = u"sandbox-exec is required for sandboxed commands on macOS and was not found."_s;
        }
        return {};
    }
    if (workspaceRoot.trimmed().isEmpty()) {
        if (error) {
            *error = u"Workspace root is not set; refusing to run a sandboxed command."_s;
        }
        return {};
    }
    return {sandboxExec, u"-p"_s, macSandboxProfile(workspaceRoot, profile)};
}

static QStringList wrapMacSandboxExec(const QString &workspaceRoot, SandboxProfile profile, const QString &command, QString *error)
{
    QStringList args = macSandboxPrefix(workspaceRoot, profile, error);
    if (args.isEmpty()) {
        return {};
    }
    // -c, not -lc: see the Linux wrapper. A sandboxed command must not source
    // the user's login profile and re-add the environment we removed.
    args << findShellExecutable() << u"-c"_s << command;
    return args;
}

static QStringList wrapMacSandboxExecArgv(const QString &workspaceRoot, SandboxProfile profile,
                                         const QString &program, const QStringList &argv, QString *error)
{
    QStringList args = macSandboxPrefix(workspaceRoot, profile, error);
    if (args.isEmpty()) {
        return {};
    }
    args << program;
    args += argv;
    return args;
}
#endif

bool Sandbox::bubblewrapAvailable() const
{
    return !QStandardPaths::findExecutable(u"bwrap"_s).isEmpty();
}

bool Sandbox::isolationAvailable() const
{
#if defined(Q_OS_LINUX)
    return bubblewrapAvailable();
#elif defined(Q_OS_MACOS)
    return QFileInfo(u"/usr/bin/sandbox-exec"_s).isExecutable()
        || !QStandardPaths::findExecutable(u"sandbox-exec"_s).isEmpty();
#else
    return false;
#endif
}

QProcessEnvironment Sandbox::processEnvironment() const
{
    if (m_profile == SandboxProfile::Off) {
        // Off is explicitly unsandboxed; callers that need the old behaviour
        // keep it. Every other profile starts from a scrubbed environment.
        return QProcessEnvironment::systemEnvironment();
    }

    QProcessEnvironment env = sanitizedProcessEnvironment();

    // Session and credential channels are not secrets by name, but they grant
    // access to the host's session: DBus can activate services, the SSH/GPG
    // agents hold keys, and DISPLAY/WAYLAND reach the user's desktop. None of
    // them belong in a sandboxed build command.
    static const QStringList blocked = {
        u"DBUS_SESSION_BUS_ADDRESS"_s,
        u"DBUS_SYSTEM_BUS_ADDRESS"_s,
        u"DBUS_STARTER_ADDRESS"_s,
        u"DBUS_STARTER_BUS_TYPE"_s,
        u"AT_SPI_BUS_ADDRESS"_s,
        u"NOTIFY_SOCKET"_s,
        u"XDG_ACTIVATION_TOKEN"_s,
        u"DESKTOP_STARTUP_ID"_s,
        u"DISPLAY"_s,
        u"WAYLAND_DISPLAY"_s,
        u"XAUTHORITY"_s,
        u"SSH_AUTH_SOCK"_s,
        u"SSH_AGENT_PID"_s,
        u"SSH_ASKPASS"_s,
        u"SUDO_ASKPASS"_s,
        u"GPG_AGENT_INFO"_s,
        u"KRB5CCNAME"_s,
        u"KRB5_KTNAME"_s,
        u"PULSE_SERVER"_s,
        u"PULSE_COOKIE"_s,
        u"PIPEWIRE_REMOTE"_s,
        u"PIPEWIRE_RUNTIME_DIR"_s,
        u"LD_PRELOAD"_s,
        u"LD_AUDIT"_s,
        u"LD_LIBRARY_PATH"_s,
        u"DYLD_INSERT_LIBRARIES"_s,
        u"DYLD_LIBRARY_PATH"_s,
        u"BASH_ENV"_s,
        u"ENV"_s,
        u"GIT_ASKPASS"_s,
        u"GIT_EXTERNAL_DIFF"_s,
        u"GIT_SSH_COMMAND"_s,
        u"GIT_CONFIG_GLOBAL"_s,
        u"GIT_CONFIG_SYSTEM"_s,
        u"PAGER"_s,
        u"LESSOPEN"_s,
        u"MANPAGER"_s,
        u"NODE_OPTIONS"_s,
        u"PYTHONSTARTUP"_s,
        u"PERL5OPT"_s,
        u"RUBYOPT"_s,
        u"JAVA_TOOL_OPTIONS"_s,
        u"PWD"_s,
        u"OLDPWD"_s,
    };
    for (const QString &name : blocked) {
        env.remove(name);
    }
    const QStringList names = env.keys();
    for (const QString &name : names) {
        if (name.startsWith(u"XDG_SESSION_"_s)) {
            env.remove(name);
        }
    }

#if defined(Q_OS_UNIX)
    const QString home = sandboxHomePath();
    const QString runtime = sandboxRuntimePath(m_profile);
    env.insert(u"TMPDIR"_s, u"/tmp"_s);
    env.insert(u"XDG_RUNTIME_DIR"_s, runtime);
    if (env.value(u"PATH"_s).isEmpty()) {
        env.insert(u"PATH"_s, u"/usr/local/bin:/usr/bin:/bin"_s);
    }
    if (env.value(u"SHELL"_s).isEmpty()) {
        env.insert(u"SHELL"_s, findShellExecutable());
    }

    if (m_profile == SandboxProfile::Strict) {
        env.insert(u"HOME"_s, home);
        env.insert(u"XDG_CACHE_HOME"_s, home + u"/.cache"_s);
        env.insert(u"XDG_CONFIG_HOME"_s, home + u"/.config"_s);
        env.insert(u"XDG_DATA_HOME"_s, home + u"/.local/share"_s);
        env.insert(u"XDG_STATE_HOME"_s, home + u"/.local/state"_s);
        QString path = u"/usr/local/bin:/usr/bin:/bin"_s;
        const QString nixProfile = u"/nix/var/nix/profiles/default/bin"_s;
        if (QFileInfo(nixProfile).isDir()) {
            path.prepend(nixProfile + u':');
        }
        env.insert(u"PATH"_s, path);
    } else if (env.value(u"HOME"_s).isEmpty()) {
        env.insert(u"HOME"_s, QDir::homePath());
    }

#if !defined(Q_OS_LINUX)
    // macOS has no mount namespace to create these for us. The strict profile
    // allows writes below /tmp and /private/var/folders, so the directories
    // can be created before the profile starts.
    if (m_profile == SandboxProfile::Strict) {
        QDir().mkpath(home);
        QFile::setPermissions(home, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QDir().mkpath(runtime);
    QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
#endif
#endif

    return env;
}

QStringList Sandbox::wrapCommand(const QString &command, QString *error, const QString &workingDirectory) const
{
    if (isAlwaysDeniedCommand(command)) {
        if (error) {
            *error = u"Command is blocked by the safety policy."_s;
        }
        return {};
    }

    // Deny globs are documented as always blocked, so a command that names a
    // denied path is refused here rather than left to the permission prompt.
    if (commandTouchesDeniedPath(command)) {
        if (error) {
            *error = u"Command touches a path blocked by the deny rules."_s;
        }
        return {};
    }

    // macOS' sandbox wrapper does not receive the working directory, so the
    // confinement check has to happen here for every platform.
    if (!workingDirectory.isEmpty() && m_profile != SandboxProfile::Off
        && !pathIsWithinOrEqual(workingDirectory, m_workspaceRoot)) {
        if (error) {
            *error = u"The working directory is outside the workspace."_s;
        }
        return {};
    }

    if (m_profile == SandboxProfile::Off) {
        return unsandboxedArgv(command);
    }

#if defined(Q_OS_LINUX)
    return wrapLinuxBubblewrap(m_workspaceRoot, m_profile, command, workingDirectory, error);
#elif defined(Q_OS_MACOS)
    Q_UNUSED(workingDirectory);
    return wrapMacSandboxExec(m_workspaceRoot, m_profile, command, error);
#else
    Q_UNUSED(workingDirectory);
    // There is no isolation mechanism on this platform. Strict and ReadOnly
    // make promises about filesystem and network access that a policy check
    // cannot keep, so they are refused instead of running unsandboxed.
    // Workspace remains policy-only here and is documented as such; selecting
    // Off is the explicit way to run completely unsandboxed.
    if (m_profile == SandboxProfile::Strict || m_profile == SandboxProfile::ReadOnly) {
        if (error) {
            *error = u"The %1 sandbox profile is not available on this platform, so the command was not run."_s
                         .arg(m_profile == SandboxProfile::Strict ? u"strict"_s : u"read-only"_s);
        }
        return {};
    }
    return unsandboxedArgv(command);
#endif
}

QStringList Sandbox::wrapArgv(const QString &program, const QStringList &args, QString *error,
                              const QString &workingDirectory) const
{
    const QString reconstructed = argvAsShellLine(program, args);
    if (isAlwaysDeniedCommand(reconstructed)) {
        if (error) {
            *error = u"Command is blocked by the safety policy."_s;
        }
        return {};
    }
    if (commandTouchesDeniedPath(reconstructed)) {
        if (error) {
            *error = u"Command touches a path blocked by the deny rules."_s;
        }
        return {};
    }

    if (!workingDirectory.isEmpty() && m_profile != SandboxProfile::Off
        && !pathIsWithinOrEqual(workingDirectory, m_workspaceRoot)) {
        if (error) {
            *error = u"The working directory is outside the workspace."_s;
        }
        return {};
    }

    if (m_profile == SandboxProfile::Off) {
        QStringList out{program};
        out += args;
        return out;
    }

#if defined(Q_OS_LINUX)
    return wrapLinuxBubblewrapArgv(m_workspaceRoot, m_profile, program, args, workingDirectory, error);
#elif defined(Q_OS_MACOS)
    Q_UNUSED(workingDirectory);
    return wrapMacSandboxExecArgv(m_workspaceRoot, m_profile, program, args, error);
#else
    Q_UNUSED(workingDirectory);
    // The same as in wrapCommand(): Strict and ReadOnly cannot be honoured
    // without an isolation mechanism.
    if (m_profile == SandboxProfile::Strict || m_profile == SandboxProfile::ReadOnly) {
        if (error) {
            *error = u"The %1 sandbox profile is not available on this platform, so the command was not run."_s
                         .arg(m_profile == SandboxProfile::Strict ? u"strict"_s : u"read-only"_s);
        }
        return {};
    }
    QStringList out{program};
    out += args;
    return out;
#endif
}

} // namespace KateAi
