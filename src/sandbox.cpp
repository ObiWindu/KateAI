/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sandbox.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

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

    Sandbox::Sandbox(Sandbox &&other) noexcept
        : m_workspaceRoot(std::move(other.m_workspaceRoot))
        , m_profile(other.m_profile)
        , m_extraDenyGlobs(std::move(other.m_extraDenyGlobs))
        , m_allowOutsideReads(other.m_allowOutsideReads)
    {
        // Fresh mutex and empty caches: the compiled deny globs describe the
        // source's configuration and are rebuilt lazily for the new owner.
    }

    Sandbox &Sandbox::operator=(Sandbox &&other) noexcept
    {
        if (this != &other) {
            const QMutexLocker locker(&m_globMutex);
            m_workspaceRoot = std::move(other.m_workspaceRoot);
            m_profile = other.m_profile;
            m_extraDenyGlobs = std::move(other.m_extraDenyGlobs);
            m_allowOutsideReads = other.m_allowOutsideReads;
            m_globCache.clear();
            m_cachedDenyGlobs.clear();
        }
        return *this;
    }

    Sandbox::Sandbox(const Sandbox &other)
    {
        // Fresh mutex and empty caches (see move ctor): the source's compiled
        // globs are rebuilt lazily. Copy the configuration under the source's
        // lock so a concurrent move/copy assignment cannot hand us torn state.
        const QMutexLocker locker(&other.m_globMutex);
        m_workspaceRoot = other.m_workspaceRoot;
        m_profile = other.m_profile;
        m_extraDenyGlobs = other.m_extraDenyGlobs;
        m_allowOutsideReads = other.m_allowOutsideReads;
    }

    Sandbox &Sandbox::operator=(const Sandbox &other)
    {
        if (this != &other) {
            const QMutexLocker locker(&m_globMutex);
            m_workspaceRoot = other.m_workspaceRoot;
            m_profile = other.m_profile;
            m_extraDenyGlobs = other.m_extraDenyGlobs;
            m_allowOutsideReads = other.m_allowOutsideReads;
            m_globCache.clear();
            m_cachedDenyGlobs.clear();
        }
        return *this;
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
            u"**/id_dsa"_s,
            u"**/*.ppk"_s,
            u"**/*_sk"_s,
            u"**/.ssh/**"_s,
            u"**/.aws/credentials"_s,
            u"**/.aws/config"_s,
            u"**/.aws/cli/cache/**"_s,
            u"**/.gnupg/**"_s,
            u"**/.netrc"_s,
            u"**/_netrc"_s,
            u"**/.docker/config.json"_s,
            u"**/.kube/config"_s,
            u"**/.npmrc"_s,
            u"**/.pypirc"_s,
            u"**/.git-credentials"_s,
            u"**/secrets.yaml"_s,
            u"**/*token*.json"_s,
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
    // Deny globs are case-insensitive: ".SSH/ID_RSA" names the same secret as
    // ".ssh/id_rsa" on case-insensitive filesystems and is an obvious bypass
    // spelling everywhere else.
    QRegularExpression re(globToRegex(pattern), QRegularExpression::CaseInsensitiveOption);
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
        QRegularExpression nested(globToRegex(u"**/"_s + pattern), QRegularExpression::CaseInsensitiveOption);
        if (nested.match(haystack).hasMatch()) {
            return true;
        }
    }
    return false;
}

// Runs a compiled glob regex, caching the compilation. The deny list is small
// and fixed for the sandbox's lifetime, so each pattern is compiled once.
// Compiled case-insensitively: deny matching is case-insensitive on all
// platforms so ".ENV"/".Ssh/ID_RSA" cannot bypass the rules.
static bool matchGlobRegex(const QString &regex, const QString &haystack, QHash<QString, QRegularExpression> &cache)
{
    auto it = cache.constFind(regex);
    if (it == cache.constEnd()) {
        // Bound the cache (LRU-ish): deny patterns are few, but search tools
        // can feed arbitrary user patterns through globMatchCached().
        constexpr int kMaxGlobCacheEntries = 512;
        if (cache.size() >= kMaxGlobCacheEntries) {
            cache.clear();
        }
        it = cache.insert(regex, QRegularExpression(regex, QRegularExpression::CaseInsensitiveOption));
    }
    return it->match(haystack).hasMatch();
}

    bool Sandbox::globMatchCached(const QString &pattern, const QString &path) const
    {
        QMutexLocker locker(&m_globMutex);
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
        // The workspace root itself is never denied: a deny glob such as
        // "**/.env" must not block opening the project directory, only the
        // secrets inside it.
        if (!m_workspaceRoot.isEmpty()) {
            const QString rootClean = QDir::cleanPath(m_workspaceRoot);
            const QString pathClean = QDir::cleanPath(unix);
            // Case-insensitive on all platforms for deny matching: the deny
            // decision must not depend on filesystem case semantics. The
            // containment checks in pathInsideWorkspace() keep their own
            // platform-specific casing.
            if (pathClean.compare(rootClean, Qt::CaseInsensitive) == 0) {
                return false;
            }
        }
        QMutexLocker locker(&m_globMutex);
        if (m_cachedDenyGlobs.isEmpty()) {
            m_cachedDenyGlobs = denyGlobs();
        }
        // Copy under lock; matching below re-enters globMatchCached (same
        // mutex is recursive? No — QMutex is non-recursive by default, so
        // release before matching). Take a snapshot instead.
        const QStringList denyList = m_cachedDenyGlobs;
        locker.unlock();
        for (const QString &glob : denyList) {
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
        if (m_workspaceRoot.isEmpty()) {
            return false;
        }
        const QString root = QDir::cleanPath(m_workspaceRoot);
        if (root.isEmpty() || root == u"."_s) {
            return false;
        }
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

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        // These filesystems are case-insensitive: a path that differs from the
        // workspace root only in casing still names a file inside it.
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
        // File doesn't exist yet - resolve iteratively up to the root so a
        // multi-level symlink chain (a/b -> /etc with a missing) cannot
        // escape with only one level resolved.
        QString suffix;
        QString ancestor = fileInfo.absolutePath();
        QString parentCanon;
        for (int depth = 0; depth < 32; ++depth) {
            parentCanon = cleanCanonicalPath(QFileInfo(ancestor).canonicalFilePath());
            if (!parentCanon.isEmpty()) {
                break;
            }
            const QString next = QFileInfo(ancestor).absolutePath();
            if (next == ancestor) {
                break;
            }
            suffix = suffix.isEmpty() ? QFileInfo(ancestor).fileName()
                                      : QFileInfo(ancestor).fileName() + u'/' + suffix;
            ancestor = next;
        }
        if (!parentCanon.isEmpty()) {
            QString rebuilt = parentCanon;
            if (!suffix.isEmpty()) {
                rebuilt += u'/' + suffix;
            }
            rebuilt += u'/' + fileInfo.fileName();
            absolute = QDir::cleanPath(rebuilt);
            // Re-check workspace containment for new file path
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
        if (m_workspaceRoot.isEmpty() && m_profile != SandboxProfile::Off) {
            if (error) {
                *error = u"Workspace root is not set."_s;
            }
            return false;
        }
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

    {
        static const QRegularExpression sudoPrefix(uR"(^sudo(?:\s+-[^\s]+)*\s+)"_s);
        const auto m = sudoPrefix.match(rest);
        if (m.hasMatch()) {
            rest = rest.mid(m.capturedLength(0)).trimmed();
        }
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
// Strips one surrounding pair of matching single/double quotes:
// "'-delete'" spells the flag -delete and must not slip the write-flag scan.
static QString unquoteFlagToken(const QString &raw)
{
    QString t = raw;
    if (t.size() >= 2) {
        const QChar first = t.at(0);
        const QChar last = t.at(t.size() - 1);
        if ((first == u'"' && last == u'"') || (first == u'\'' && last == u'\'')) {
            return t.mid(1, t.size() - 2);
        }
    }
    return t;
}

static bool hasWriteFlag(const QString &primary, const QString &command)
{
    const QStringList rawTokens = command.split(QRegularExpression(uR"(\s+)"_s), Qt::SkipEmptyParts);
    for (const QString &raw : rawTokens) {
        const QString token = unquoteFlagToken(raw);
        if (token.isEmpty()) {
            continue;
        }
        // A flag value can hide behind '=' ("--output=file"); check the value
        // part too for the find actions.
        QString afterEq;
        const int eq = token.indexOf(u'=');
        if (eq >= 0) {
            afterEq = unquoteFlagToken(token.mid(eq + 1));
        }
        if (primary == u"find"_s) {
            static const QStringList findWrites = {
                u"-delete"_s, u"-exec"_s, u"-execdir"_s, u"-ok"_s, u"-okdir"_s,
                u"-fls"_s, u"-fprint"_s, u"-fprint0"_s, u"-fprintf"_s,
            };
            if (findWrites.contains(token) || (!afterEq.isEmpty() && findWrites.contains(afterEq))) {
                return true;
            }
            // Glued shorts: "-fls" contains -fls, "-print0-fls" etc. Only
            // flag-shaped tokens are substring-checked so a path like
            // "./my-exec-file" cannot trip the write-action scan.
            if (token.startsWith(u'-')) {
                for (const QString &w : findWrites) {
                    if (token.contains(w) || (!afterEq.isEmpty() && afterEq.contains(w))) {
                        return true;
                    }
                }
            }
        } else if (primary == u"sort"_s) {
            if (token == u"-o"_s || token == u"--output"_s || token.startsWith(u"--output="_s)
                || (token.startsWith(u"-o"_s) && token.size() > 2)) {
                return true;
            }
        } else if (primary == u"date"_s) {
            if (token == u"-s"_s || token == u"--set"_s || token.startsWith(u"--set="_s)
                || token.startsWith(u"-s"_s)) {
                return true;
            }
        } else if (primary == u"tail"_s) {
            if (token == u"-f"_s || token == u"-F"_s || token == u"--follow"_s
                || token.startsWith(u"--follow="_s)) {
                return true;
            }
            // Combined shorts: "-qF", "-n5F", "-Fq" all follow.
            if (token.startsWith(u'-') && !token.startsWith(u"--"_s)) {
                const QString flags = token.mid(1);
                // Strip a numeric count tail ("-n5F" -> flags contain F).
                if (flags.contains(u'f') || flags.contains(u'F')) {
                    // Only single-dash clusters: "--foo" handled above.
                    // tail -f/-F never terminates, so it is never read-only.
                    bool hasFollow = false;
                    for (const QChar ch : flags) {
                        if (ch == u'f' || ch == u'F') {
                            hasFollow = true;
                            break;
                        }
                    }
                    if (hasFollow) {
                        return true;
                    }
                }
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
        // "printenv" only ever prints (no command execution), so it stays
        // read-only; note the child inherits the environment, so API keys in
        // env are visible to approved printenv output by design.
        u"printenv"_s,
        u"type"_s,   u"dir"_s,    u"where"_s,     u"where.exe"_s,
        u"Get-ChildItem"_s, u"Get-Content"_s, u"Get-Location"_s, u"Get-Date"_s, u"Get-Process"_s,
    };

    const QString cmd = command.trimmed();
    // "sudo ls" must not auto-approve: primaryCommand() strips a leading
    // "sudo " for display, but the allowlist decision must see the privilege
    // escalation. Any sudo/su prefix is never read-only.
    if (cmd.startsWith(u"sudo "_s) || cmd.startsWith(u"sudo\t"_s) || cmd.startsWith(u"su "_s) || cmd.startsWith(u"su\t"_s)) {
        return false;
    }
    static const QRegularExpression writers(
        uR"(\b(?:tee|rm|mv|cp|chmod|chown|mkdir|touch|dd|del|erase|rd|rmdir|copy|move|ren|rename|New-Item|Set-Content|Out-File|Remove-Item|Move-Item|Copy-Item)\b)"_s,
        QRegularExpression::CaseInsensitiveOption);
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
    if (cmd.contains(u"&&"_s) || cmd.contains(u"||"_s) || cmd.contains(u';') || cmd.contains(u'|') || cmd.contains(u'\n')) {
        const QStringList parts = cmd.split(QRegularExpression(uR"(\s*(?:&&|\|\||;|\||\n)\s*)"_s));
        for (const QString &part : parts) {
            if (!isReadOnlyCommand(part)) {
                return false;
            }
        }
        return true;
    }

    const QString primary = primaryCommand(cmd);
    if (primary == u"git"_s) {
        const QString sub = cmd.trimmed().section(QRegularExpression(uR"(\s+)"_s), 1, 1);
        // "git status" can refresh the index, but it never changes tracked
        // content, so it stays on the read-only list (chained commands are
        // still split and judged part by part above, so "echo hi; rm -rf ~"
        // is not readonly: the rm part fails the allowlist).
        static const QStringList gitRead = {
            u"status"_s, u"branch"_s, u"log"_s,       u"diff"_s,      u"ls-files"_s, u"show"_s,      u"rev-parse"_s,
            u"blame"_s,  u"describe"_s, u"shortlog"_s, u"cat-file"_s, u"ls-tree"_s,  u"show-ref"_s,  u"rev-list"_s,
        };
        return gitRead.contains(sub);
    }
    if (!readOnly.contains(primary)) {
        return false;
    }
    // The executable only reads, but a flag can still tell it to write:
    // "find . -delete" and "sort -o out.txt" both modify files, so Ask mode
    // must not treat them as read-only and auto-approve them.
    if (hasWriteFlag(primary, cmd)) {
        return false;
    }
    // Path-blindness fix: a read-only executable is only auto-allow when it
    // stays inside the workspace and off the deny list. "cat /etc/passwd" and
    // "cat ~/.ssh/id_rsa" are reads, but they must prompt (or be denied by
    // commandTouchesDeniedPath), never auto-approve.
    if (commandTouchesDeniedPath(cmd)) {
        return false;
    }
    if (!m_workspaceRoot.isEmpty()) {
        // Scan path-like operands; an absolute path (or ~) outside the
        // workspace disqualifies the read-only fast path.
        const QStringList rawTokens = cmd.split(QRegularExpression(uR"(\s+)"_s), Qt::SkipEmptyParts);
        for (QString raw : rawTokens) {
            raw = unquoteFlagToken(raw);
            QString candidate = raw;
            const int eq = candidate.indexOf(u'=');
            if (eq >= 0 && eq + 1 < candidate.size()) {
                candidate = unquoteFlagToken(candidate.mid(eq + 1));
            }
            candidate.remove(u'"');
            candidate.remove(u'\'');
            if (candidate.isEmpty() || candidate.startsWith(u'-')) {
                continue;
            }
            QString abs;
            if (candidate == u"~"_s) {
                abs = QDir::homePath();
            } else if (candidate.startsWith(u"~/"_s)) {
                abs = QDir::homePath() + candidate.mid(1);
            } else if (candidate.startsWith(u'/')) {
                abs = QDir::cleanPath(QDir::fromNativeSeparators(candidate));
            } else if (candidate.size() >= 3 && candidate.at(1) == u':' && (candidate.at(2) == u'/' || candidate.at(2) == u'\\')) {
                abs = QDir::cleanPath(QDir::fromNativeSeparators(candidate));
            } else {
                continue; // workspace-relative operand: inside by construction
            }
            if (!pathInsideWorkspace(abs)) {
                return false;
            }
            if (isDenied(abs)) {
                return false;
            }
        }
    }
    return true;
}

bool Sandbox::isDangerousCommand(const QString &command) const
{
    const QString cmd = command.trimmed();
    static const QRegularExpression dangerous(
        uR"(\b(?:sudo|su|chmod\s+-R|chown\s+-R|mkfs|shutdown|reboot|systemctl|useradd|userdel|passwd|diskpart|format|cipher|reg\s+delete|Remove-Item\s+(-Recurse|-Force)|rmdir\s+/(s|q)|del\s+/s)\b)"_s,
        QRegularExpression::CaseInsensitiveOption);
    // "xargs rm", "find … -exec rm" and "find … -delete" run rm without naming
    // it as the primary command. They are dangerous (prompted), not
    // always-denied: the target may be an ordinary build directory. A "/" or
    // "~" target is still caught by the always-denied rm guard.
    static const QRegularExpression rmViaExec(
        uR"(\bxargs\b[^\n;|&]*\brm\b|\bfind\b[^\n;|&]*(-exec(dir)?\s+.*\brm\b|-ok(dir)?\s+.*\brm\b|-delete\b))"_s,
        QRegularExpression::CaseInsensitiveOption);
    return dangerous.match(cmd).hasMatch() || rmViaExec.match(cmd).hasMatch() || isAlwaysDeniedCommand(cmd);
}

// Programmatic rm-root check to cover what the rm regex cannot spell:
// flags after the target ("rm / -rf"), "--" terminators, "//", "/./" and
// "/tmp/.." spellings (all clean to "/"), quoted targets, "~" and $HOME.
static bool rmTargetsRoot(const QString &command)
{
    // Tokenize respecting single/double quotes so quoted targets stay whole.
    QStringList tokens;
    QString cur;
    bool inSingle = false;
    bool inDouble = false;
    for (int i = 0; i < command.size(); ++i) {
        const QChar c = command.at(i);
        if (c == u'\'' && !inDouble) {
            inSingle = !inSingle;
            cur.append(c);
        } else if (c == u'"' && !inSingle) {
            inDouble = !inDouble;
            cur.append(c);
        } else if ((c.isSpace() || c == u';' || c == u'|' || c == u'&' || c == u'\n') && !inSingle && !inDouble) {
            if (!cur.isEmpty()) {
                tokens.append(cur);
                cur.clear();
            }
            if ((c == u'|' || c == u'&') && i + 1 < command.size() && command.at(i + 1) == c) {
                tokens.append(QString(c) + QString(c));
                ++i;
            } else if (!c.isSpace() && c != u'\n') {
                tokens.append(QString(c));
            }
        } else {
            cur.append(c);
        }
    }
    if (!cur.isEmpty()) {
        tokens.append(cur);
    }

    auto stripQuotes = [](QString t) -> QString {
        // Strip surrounding quote pairs and any stray quote characters, so
        // "'-delete'"-style quoting cannot hide a target either.
        if (t.size() >= 2) {
            const QChar f = t.at(0);
            const QChar l = t.at(t.size() - 1);
            if ((f == u'"' && l == u'"') || (f == u'\'' && l == u'\'')) {
                t = t.mid(1, t.size() - 2);
            }
        }
        t.remove(u'"');
        t.remove(u'\'');
        return t;
    };
    auto baseName = [](const QString &t) -> QString {
        QString s = t;
        const int slash = s.lastIndexOf(u'/');
        const int backslash = s.lastIndexOf(u'\\');
        const int sep = qMax(slash, backslash);
        if (sep >= 0) {
            s = s.mid(sep + 1);
        }
#ifdef Q_OS_WIN
        if (s.endsWith(u".exe"_s, Qt::CaseInsensitive)) {
            s = s.left(s.size() - 4);
        }
#endif
        return s;
    };

    const QString home = QDir::homePath();
    // Expand $VAR/${VAR} spellings (including quoted "${HOME}/x") using the
    // process environment, so "$HOME/foo" and "${HOME}/foo" resolve like "~".
    auto expandEnv = [](QString s, bool *hadUnset = nullptr) -> QString {
        static const QRegularExpression braced(uR"(\$\{([A-Za-z_][A-Za-z0-9_]*)\})"_s);
        auto it = braced.globalMatch(s);
        // Collect first to avoid mutating while matching.
        struct Rep { qsizetype start; qsizetype len; QString val; bool unset = false; };
        QList<Rep> reps;
        while (it.hasNext()) {
            const auto m = it.next();
            const QString var = m.captured(1);
            const QByteArray v = qgetenv(var.toUtf8().constData());
            const bool isUnset = !qEnvironmentVariableIsSet(var.toUtf8().constData());
            reps.append({m.capturedStart(0), m.capturedLength(0), isUnset ? m.captured(0) : QString::fromLocal8Bit(v), isUnset});
        }
        for (int k = reps.size() - 1; k >= 0; --k) {
            s.replace(reps.at(k).start, reps.at(k).len, reps.at(k).val);
            if (reps.at(k).unset && hadUnset) {
                *hadUnset = true;
            }
        }
        static const QRegularExpression plain(uR"(\$([A-Za-z_][A-Za-z0-9_]*))"_s);
        it = plain.globalMatch(s);
        reps.clear();
        while (it.hasNext()) {
            const auto m = it.next();
            const QString var = m.captured(1);
            const QByteArray v = qgetenv(var.toUtf8().constData());
            // Fail closed: an unset variable keeps its "$VAR" spelling instead
            // of emptying to "" (which would turn "$EMPTY/foo" into "/foo").
            const bool isUnset = !qEnvironmentVariableIsSet(var.toUtf8().constData());
            reps.append({m.capturedStart(0), m.capturedLength(0), isUnset ? m.captured(0) : QString::fromLocal8Bit(v), isUnset});
        }
        for (int k = reps.size() - 1; k >= 0; --k) {
            s.replace(reps.at(k).start, reps.at(k).len, reps.at(k).val);
            if (reps.at(k).unset && hadUnset) {
                *hadUnset = true;
            }
        }
        return s;
    };
    for (int i = 0; i < tokens.size(); ++i) {
        QString tok = stripQuotes(tokens.at(i));
        if (tok.isEmpty()) {
            continue;
        }
        if (baseName(tok) != u"rm"_s) {
            continue;
        }
        // Found an rm invocation: scan its arguments until a chain delimiter.
        bool endOfFlags = false;
        for (int j = i + 1; j < tokens.size(); ++j) {
            const QString rawNext = tokens.at(j);
            if (rawNext == u";"_s || rawNext == u"|"_s || rawNext == u"&"_s
                || rawNext == u"&&"_s || rawNext == u"||"_s) {
                break;
            }
            QString t = stripQuotes(rawNext);
            if (t.isEmpty()) {
                continue;
            }
            if (!endOfFlags && t == u"--"_s) {
                endOfFlags = true;
                continue;
            }
            if (!endOfFlags && t.startsWith(u'-') && t.size() > 1 && t != u"-"_s) {
                continue; // a flag; flags may also follow targets
            }
            // A target candidate.
            bool hadUnset = false;
            QString expanded = expandEnv(t, &hadUnset);
            // Fail closed: "$UNSET/foo" or a bare "$UNSET" would otherwise
            // empty to "/foo"/"" and slip the checks below. Deny destructive
            // rm when the target could not be fully resolved.
            if (hadUnset || expanded.contains(u'$')) {
                return true;
            }
            if (expanded == u"~"_s) {
                expanded = home;
            } else if (expanded.startsWith(u"~/"_s)) {
                expanded = home + expanded.mid(1);
            } else if (expanded.startsWith(u'~') && expanded.size() > 1) {
                // "~other/foo" names another user's home: it is never inside
                // the workspace, so treat it as home-targeting (deny).
                return true;
            } else {
                static const QRegularExpression homeVar(uR"(\$\{HOME\}|\$HOME(?![A-Za-z0-9_]))"_s);
                expanded.replace(homeVar, home);
            }
            if (!home.isEmpty() && expanded == home) {
                return true; // bare "~" (or its $HOME spellings resolved above)
            }
            // Only the home directory itself is denied here — not every path
            // below it. A workspace under $HOME (the common case) must stay
            // usable; "rm -rf ~/project/build" inside the workspace is a
            // normal operation, while "rm -rf ~" or "rm -rf $HOME" is not.
            const QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(expanded));
            // Deny filesystem-root removals: "/", "/*", "//", "/./",
            // "/tmp/..", quoted and "--"-terminated spellings alike.
            if (cleaned == u"/"_s) {
                return true;
            }
            if (expanded.endsWith(u"/*"_s) && QDir::cleanPath(QDir::fromNativeSeparators(expanded.left(expanded.size() - 2))) == u"/"_s) {
                return true;
            }
        }
    }
    return false;
}

// A fork bomb defines a function and backgrounds it inside the body, e.g.
// ":(){ :|:& };:", "bomb(){ bomb & bomb;}", "x(){ x|y& };x". The classic
// regex only matched the ";…}" spelling; anything with "&" inside "(){}" is
// denied instead, which covers the no-semicolon and multi-command variants.
static bool isForkBomb(const QString &command)
{
    // Covers ":(){ :|:& };:", "bomb(){ bomb & bomb;}", "x(){ x|y& };x",
    // no-semicolon "(){ a& }", and backtick/$() payloads via the & in braces.
    static const QRegularExpression bomb(uR"(\(\s*\)\s*\{[^}]*(&|\|)[^}]*\})"_s);
    if (bomb.match(command).hasMatch()) {
        return true;
    }
    // "bomb(){ bomb&bomb };bomb" without spaces, and ":(){:|:&};:" minimal
    static const QRegularExpression bombTight(uR"(\(\s*\)[^;|&\n]*\{[^}]*(?:&\s*;?|\|\s*:?)[^}]*\})"_s);
    return bombTight.match(command).hasMatch();
}

bool Sandbox::isAlwaysDeniedCommand(const QString &command) const
{
    const QString cmd = command.trimmed();
    // "dd … of=/dev/null" is an explicit null sink and is allowed; every other
    // "of=/dev/…" names a device and stays denied. The negative lookahead
    // exempts only "null" as a full basename (quotes/spaces tolerated).
    static const QRegularExpression denied(
        uR"((rm\s+((?:-[a-zA-Z]*[rf][a-zA-Z]*|--no-preserve-root|--force|--recursive)\s+)+(/(\s|$|\*+)|["']/["']?(\s|$|\*+)|~(/\S*)?(\s|$)))|(\bmkfs\b)|(\bdd\s+.*\bof\s*=\s*["']?/dev/(?!null(\s|$|"|'|;|&|\||>))[^"'\s]*)|([A-Za-z_:][A-Za-z0-9_:]*\(\)\s*\{\s*[A-Za-z_:][A-Za-z0-9_:]*\|[A-Za-z_:][A-Za-z0-9_:]*&\s*;\s*[^}]*\})|(\b(curl|wget)\b.*\|\s*(?:(?:sudo(?:\s+-[^\s]+)*|env|nohup|timeout(?:\s+[^\s]+)+|setsid|xargs(?:\s+[^\s]+)*)\s+)*(?:[\w./-]*\/)?(sh|bash|zsh|fish|dash|ksh|cmd|cmd\.exe|powershell|powershell\.exe|pwsh|python[0-9.]*|perl|ruby|node)\b)|(\|\s*(?:(?:sudo(?:\s+-[^\s]+)*|env|nohup|timeout(?:\s+[^\s]+)+|setsid|xargs(?:\s+[^\s]+)*)\s+)*(?:[\w./-]*\/)?(sh|bash|dash|zsh|fish|python\d*|perl|ruby|php|pwsh|powershell)\b)|(\bformat\s+[a-zA-Z]:)|(\bdiskpart\b)|(\brmdir\b[^\n;|&]*?[/\-]s\b)|(\brmdir\b[^\n;|&]*?[/\-]q\b)|(\bRemove-Item\b[^\n;|&]*?-Recurse\b[^\n;|&]*?([A-Z]:[\\/]|~([\\/]|$)|\s/(\s|$|;|&|\|))))"_s,
        QRegularExpression::CaseInsensitiveOption);
    return denied.match(cmd).hasMatch() || rmTargetsRoot(cmd) || isForkBomb(cmd);
}

bool Sandbox::commandTouchesDeniedPath(const QString &command) const
{
    // Fail closed when there is no root to resolve against: allowsRead() fails
    // closed on an empty root, and this scan must agree — returning false here
    // would let a shell read the deny-listed paths the file tools refuse.
    if (m_workspaceRoot.isEmpty()) {
        return true;
    }
    // A huge command string is a syscall-amplification vector (millions of
    // tokens each asking isDenied()/normalizePath()). Fail closed instead.
    if (command.size() > 100000) {
        return true;
    }
    // This is not a shell parser. It scans tokens and asks the deny rules about
    // each one, which is enough to stop "cat ~/.ssh/id_rsa" and "cat .env" from
    // walking around the guard the file tools apply.
    //
    // Bare words are checked against the workspace root too, because that is the
    // working directory of every sandboxed command: "cat backup.pem" is a real
    // deny-glob hit. Over-blocking is the safe direction here — the agent has the
    // glob tool for pattern searches, and a blocked call reports why.

    // A "sh -c 'payload'" (or bash/dash/zsh/fish -c) hides the real command in
    // one quoted argument. Scan the payload recursively so
    // "sh -c 'cat ~/.ssh/id_rsa'" cannot smuggle a denied path past the token
    // scan. Same for "env VAR=.. sh -c …": the payload is what runs.
    // "--" between the flags and "-c" ("sh -- -c payload", "bash -l -- -c …")
    // is skipped the same way real shells skip it.
    {
        static const QRegularExpression shPayload(
            uR"(\b(?:sh|bash|dash|zsh|fish)\b(?:\s+(?:--|[^\s;|&'"]*))*\s+-c\s+(['"])(.*?)\1)"_s,
            QRegularExpression::DotMatchesEverythingOption);
        auto it = shPayload.globalMatch(command);
        while (it.hasNext()) {
            const auto m = it.next();
            if (commandTouchesDeniedPath(m.captured(2))) {
                return true;
            }
        }
        // Unquoted "-c payload" (single next word): scan that word too.
        static const QRegularExpression shPayloadBare(
            uR"(\b(?:sh|bash|dash|zsh|fish)\b(?:\s+(?:--|[^\s;|&'"]*))*\s+-c\s+([^\s;|&'"]+))"_s);
        auto jt = shPayloadBare.globalMatch(command);
        while (jt.hasNext()) {
            const auto m = jt.next();
            if (commandTouchesDeniedPath(m.captured(1))) {
                return true;
            }
        }
    }

    // Parse command respecting quoted strings. Splits on whitespace and on
    // shell metacharacters including () `` so "$(cat ~/.ssh/id_rsa)" and
    // "`cat f`" surface their inner paths as tokens. "$" is deliberately NOT
    // a delimiter: splitting on it turned "$VAR/foo" into "VAR/foo" and let
    // unresolved variables bypass the deny scan. Keeping "$" in the token
    // lets checkCandidate() fail closed on it below.
    QStringList tokens;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    QString currentToken;

    for (int i = 0; i < command.size(); ++i) {
        QChar c = command.at(i);

        if (c == u'\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
            currentToken.append(c);
        } else if (c == u'"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
            currentToken.append(c);
        } else if ((c.isSpace() || c == u';' || c == u'|' || c == u'&' || c == u'<' || c == u'>'
                    || c == u'(' || c == u')' || c == u'`' || c == u'\n')
                   && !inSingleQuote && !inDoubleQuote) {
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

    auto checkCandidate = [this](QString candidate) -> bool {
        // Strip trailing glue the tokenizer leaves behind: "f)," in
        // "xargs -I{} … f)," or "id_rsa)," must still match.
        while (!candidate.isEmpty()) {
            const QChar last = candidate.at(candidate.size() - 1);
            if (last == u')' || last == u';' || last == u',' || last == u':' || last == u'"' || last == u'\'') {
                candidate.chop(1);
            } else {
                break;
            }
        }
        while (!candidate.isEmpty() && candidate.at(0) == u'(') {
            candidate.remove(0, 1);
        }
        candidate.remove(u'"');
        candidate.remove(u'\'');
        // Backtick remnants from command substitution.
        candidate.remove(u'`');
        if (candidate.isEmpty()) {
            return false;
        }
        if (candidate == u"~"_s) {
            candidate = QDir::homePath();
        } else if (candidate.startsWith(u"~/"_s)) {
            candidate = QDir::homePath() + candidate.mid(1);
        }
        // $HOME names the same directory as "~", so a denied path must not
        // slip past the check by spelling it with a variable.
        static const QRegularExpression homeVar(uR"(\$\{HOME\}|\$HOME(?![A-Za-z0-9_]))"_s);
        candidate.replace(homeVar, QDir::homePath());
        // Fail closed on unresolved $VAR/${VAR} *path operands*: such a token
        // could name anything, including a denied path, so checking it "as
        // written" lets it bypass the deny globs. Narrowed to real operands —
        // a "$" buried in prose ("line $i") or a "$(…)" substitution marker
        // (whose inner tokens are scanned separately) is not a path operand.
        if (candidate.contains(u'$')) {
            QString noSub = candidate;
            noSub.remove(u"$("_s);
            if (noSub.contains(u'$')) {
                // Special/positional params ($?, $#, $$, $1…) name no path.
                static const QRegularExpression varOperand(uR"(\$[A-Za-z_][A-Za-z0-9_]*|\$\{[A-Za-z_][A-Za-z0-9_]*\})"_s);
                const bool hasVar = varOperand.match(noSub).hasMatch();
                // Path-like (contains '/') or variable-led ("$F", "$F/x",
                // "${F}") operands fail closed; "$" inside a larger word
                // ("line $i", "a$b") is left to the checks below.
                if (hasVar && (noSub.contains(u'/') || noSub.trimmed().startsWith(u'$'))) {
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
        return false;
    };

    for (const QString &token : tokens) {
        QString candidate = token;
        // A flag is not a path, but its value can name one: "--config=.env"
        // and "-I/path/.env" would slip the deny scan entirely if every
        // '-'-prefixed token were skipped. The part after '=' (and after ':'
        // for include-style flags) is checked; the flag itself is not.
        if (candidate.startsWith(u'-')) {
            const int eq = candidate.indexOf(u'=');
            const int colon = candidate.indexOf(u':');
            int valueStart = -1;
            if (eq >= 0 && (colon < 0 || eq < colon)) {
                valueStart = eq + 1;
            } else if (colon >= 0) {
                valueStart = colon + 1;
            }
            if (valueStart >= 0 && valueStart < candidate.size()) {
                if (checkCandidate(candidate.mid(valueStart))) {
                    return true;
                }
            }
            // Glued include-style paths: "-I/path", "-L~/.ssh", "-I$HOME/x".
            // Only the path suffix is a candidate; the flag letter is not.
            static const QRegularExpression glued(uR"(^-[A-Za-z]+(/.+|~/.+|\$HOME.+|\$\{.+\}.+)$)"_s);
            const auto gm = glued.match(candidate);
            if (gm.hasMatch()) {
                if (checkCandidate(gm.captured(1))) {
                    return true;
                }
                continue;
            }
            static const QRegularExpression gluedAbs(uR"(^-[A-Za-z](/.+)$)"_s);
            const auto ga = gluedAbs.match(candidate);
            if (ga.hasMatch()) {
                if (checkCandidate(ga.captured(1))) {
                    return true;
                }
            }
            continue;
        }
        if (checkCandidate(candidate)) {
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
    // No invented fallback: callers report "no shell" with profile context
    // instead of exec'ing a path that is not there.
    return shell;
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

static QStringList unsandboxedArgv(const QString &command, QString *error = nullptr)
{
    const QString shell = findShellExecutable();
    if (shell.isEmpty()) {
        if (error) {
            *error = u"No shell executable was found, so the command was not run."_s;
        }
        return {};
    }
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

// The seatbelt profile embeds the workspace path inside "(subpath "…")".
// Parens, semicolons, quotes, backticks, "$", backslashes and newlines would
// break out of that string and change the profile syntax, so such a root is
// refused rather than escaped into a weaker sandbox.
static bool seatbeltPathSafe(const QString &path)
{
    for (const QChar c : path) {
        if (c == u'(' || c == u')' || c == u';' || c == u'"' || c == u'`' || c == u'$' || c == u'\\'
            || c == u'\n' || c == u'\r') {
            return false;
        }
    }
    return true;
}
#endif

#if defined(Q_OS_LINUX)
static QStringList linuxBubblewrapPrefix(const QString &workspaceRoot, SandboxProfile profile, QString *error)
{
    const QString bwrap = QStandardPaths::findExecutable(u"bwrap"_s);
    if (bwrap.isEmpty()) {
        if (error) {
            *error = u"bubblewrap (bwrap) is required for sandboxed commands and was not found."_s;
        }
        return {};
    }

    const bool readOnlyFs = profile == SandboxProfile::ReadOnly;

    // Never use the host absolute path as a bwrap destination. Intermediate
    // symlinks (common for /home) or special mounts make bwrap fail with
    // "Can't mkdir parents … Permission denied". Zed and other working
    // sandboxes always mount the project at a fixed shallow path instead.
    //
    // Strict              → /workspace under a clean tmpfs root
    // Workspace/ReadOnly  → /tmp/kateai-workspace (under a re-bound writable /tmp)
    const QString sandboxWorkspace =
        (profile == SandboxProfile::Strict) ? u"/workspace"_s : u"/tmp/kateai-workspace"_s;

    QStringList args;
    // NOTE (--clearenv): a minimal environment (PATH + TMPDIR only) would
    // shrink the secret surface (API keys in env leak into approved output via
    // printenv), but bwrap clears too much for typical builds (LANG, HOME,
    // build tool config) and risks breaking commands the user approved. Kept
    // as inherited env deliberately; revisit with an explicit allowlist if a
    // minimal-env Strict variant is wanted.
    args << bwrap << u"--die-with-parent"_s << u"--unshare-pid"_s << u"--unshare-ipc"_s << u"--unshare-uts"_s;

    if (profile == SandboxProfile::Strict) {
        args << u"--tmpfs"_s << u"/"_s;
        const QStringList runtimeDirectories = {
            u"/usr"_s, u"/lib"_s, u"/lib64"_s, u"/bin"_s, u"/sbin"_s,
        };
        for (const QString &directory : runtimeDirectories) {
            const QFileInfo info(directory);
            if (info.exists() && info.isDir() && !info.isSymLink()) {
                args << u"--ro-bind"_s << directory << directory;
            }
        }
        // On modern distributions /bin is a symlink to /usr/bin, so the loop
        // above skips it and "/bin/sh" would be missing inside the sandbox.
        // Bind the resolved shell binary explicitly so the exec path exists.
        {
            const QString shell = findShellExecutable();
            if (shell.isEmpty()) {
                if (error) {
                    *error = u"No shell executable was found for the strict profile, so the command was not run."_s;
                }
                return {};
            }
            const QFileInfo shellInfo(shell);
            if (shellInfo.exists() && !shellInfo.isDir()) {
                args << u"--ro-bind"_s << shell << shell;
                // Also cover the classic path when it is a dangling symlink
                // target inside the tmpfs root.
                if (shell != u"/bin/sh"_s && QFileInfo(u"/bin/sh"_s).isSymLink()) {
                    args << u"--ro-bind"_s << shell << u"/bin/sh"_s;
                }
            }
        }
        args << u"--bind"_s << workspaceRoot << sandboxWorkspace;
        // A private /tmp instead of the one of the host. The host's /tmp is
        // writable by everybody, so a command could leave files there, and in
        // the strict profile it would break the isolation of the tmpfs root.
        args << u"--tmpfs"_s << u"/tmp"_s;
        args << u"--proc"_s << u"/proc"_s << u"--dev"_s << u"/dev"_s;
        args << u"--chdir"_s << sandboxWorkspace;
        args << u"--unshare-net"_s;
    } else {
        args << u"--ro-bind"_s << u"/"_s << u"/"_s;
        args << u"--proc"_s << u"/proc"_s << u"--dev"_s << u"/dev"_s;
        // A private /tmp for Workspace too, not just ReadOnly: the host's
        // /tmp is world-writable and outlives the sandbox, so sharing it lets
        // a command leave files for other processes or read what they left.
        // Like Strict, both non-strict profiles get an isolated tmpfs.
        args << u"--tmpfs"_s << u"/tmp"_s;
        args << (readOnlyFs ? u"--ro-bind"_s : u"--bind"_s) << workspaceRoot << sandboxWorkspace;
        args << u"--chdir"_s << sandboxWorkspace;
        if (readOnlyFs) {
            args << u"--unshare-net"_s;
        }
    }
    return args;
}

static QStringList wrapLinuxBubblewrap(const QString &workspaceRoot, SandboxProfile profile, const QString &command, QString *error)
{
    QStringList args = linuxBubblewrapPrefix(workspaceRoot, profile, error);
    if (args.isEmpty()) {
        return {};
    }
    const QString shell = findShellExecutable();
    if (shell.isEmpty() || !QFileInfo(shell).isExecutable()) {
        if (error) {
            *error = u"No shell executable was found, so the command was not run."_s;
        }
        return {};
    }
    args << u"--"_s << shell << u"-lc"_s << command;
    return args;
}

static QStringList wrapLinuxBubblewrapArgv(const QString &workspaceRoot, SandboxProfile profile,
                                          const QString &program, const QStringList &argv, QString *error)
{
    QStringList args = linuxBubblewrapPrefix(workspaceRoot, profile, error);
    if (args.isEmpty()) {
        return {};
    }
    args << u"--"_s << program;
    args += argv;
    return args;
}
#endif

#if defined(Q_OS_MACOS)
static QStringList wrapMacSandboxExec(const QString &workspaceRoot, SandboxProfile profile, const QString &command, QString *error)
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
    if (!seatbeltPathSafe(workspaceRoot)) {
        if (error) {
            *error = u"The workspace path contains characters that cannot be embedded in a seatbelt profile, so the command was not run."_s;
        }
        return {};
    }
    const QString shell = findShellExecutable();
    if (shell.isEmpty() || !QFileInfo(shell).isExecutable()) {
        if (error) {
            *error = u"No shell executable was found, so the command was not run."_s;
        }
        return {};
    }

    const QString ws = seatbeltQuote(QDir::cleanPath(workspaceRoot));
    QString profileText = u"(version 1)\n(allow default)\n"_s;
    if (profile == SandboxProfile::ReadOnly) {
        profileText += u"(deny file-write*)\n"
                       "(allow file-write* (subpath \"/tmp\") (subpath \"/private/tmp\") "
                       "(subpath \"/private/var/folders\") (subpath \"/var/folders\") (subpath \"/dev\"))\n"
                       "(deny network*)\n"_s;
    } else if (profile == SandboxProfile::Strict) {
        profileText = u"(version 1)\n(deny default)\n"
                      "(allow process*)\n(allow signal)\n(allow sysctl-read)\n(allow mach-lookup)\n"
                      "(allow ipc-posix-shm)\n(allow file-read-metadata)\n"_s;
        profileText += u"(allow file-read* (subpath \"/usr\") (subpath \"/bin\") (subpath \"/sbin\") "
                       "(subpath \"/opt\") (subpath \"/Library\") (subpath \"/System\") "
                       "(subpath \"/private/tmp\") (subpath \"/tmp\") (subpath \"/dev\") (subpath \""_s
            + ws + u"\"))\n"_s;
        profileText += u"(allow file-write* (subpath \""_s + ws
            + u"\") (subpath \"/tmp\") (subpath \"/private/tmp\") "
              "(subpath \"/private/var/folders\") (subpath \"/var/folders\") (subpath \"/dev\"))\n"
              "(deny network*)\n"_s;
    } else {
        profileText += u"(deny file-write*)\n(allow file-write* (subpath \""_s + ws
            + u"\") (subpath \"/tmp\") (subpath \"/private/tmp\") "
              "(subpath \"/private/var/folders\") (subpath \"/var/folders\") (subpath \"/dev\"))\n"_s;
    }

    return {sandboxExec, u"-p"_s, profileText, shell, u"-lc"_s, command};
}

static QStringList wrapMacSandboxExecArgv(const QString &workspaceRoot, SandboxProfile profile,
                                         const QString &program, const QStringList &argv, QString *error)
{
    QStringList wrapped = wrapMacSandboxExec(workspaceRoot, profile, QString(), error);
    if (wrapped.isEmpty()) {
        return {};
    }
    // Replace the trailing `shell -lc command` with the raw argv so quotes in
    // arguments are never re-parsed by a nested bash -c.
    if (wrapped.size() >= 3) {
        wrapped.removeLast(); // command
        wrapped.removeLast(); // -lc
        wrapped.removeLast(); // shell
    }
    wrapped << program;
    wrapped += argv;
    return wrapped;
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

QStringList Sandbox::wrapCommand(const QString &command, QString *error) const
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

    if (m_profile == SandboxProfile::Off) {
        return unsandboxedArgv(command, error);
    }

#if defined(Q_OS_LINUX)
    return wrapLinuxBubblewrap(m_workspaceRoot, m_profile, command, error);
#elif defined(Q_OS_MACOS)
    return wrapMacSandboxExec(m_workspaceRoot, m_profile, command, error);
#else
    // There is no isolation mechanism on this platform. The strict profile
    // promises that the command cannot reach the rest of the system, and that
    // promise cannot be kept here. Running the command unsandboxed would
    // silently break it, so the command is refused instead (fail-closed).
    // Workspace/ReadOnly fall through to an unsandboxed shell, but only after
    // the always-denied and deny-glob scans above refused secret paths — there
    // is no containment here, just the policy guard, and callers must treat a
    // lack of isolationAvailable() as untrusted execution.
    if (m_profile == SandboxProfile::Strict) {
        if (error) {
            *error = u"The strict sandbox profile is not available on this platform, so the command was not run."_s;
        }
        return {};
    }
    return unsandboxedArgv(command, error);
#endif
}

QStringList Sandbox::wrapArgv(const QString &program, const QStringList &args, QString *error) const
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
    // A "sh -c 'payload'" argv hides the executed code in argv[2+], which the
    // shell-line scan above sees only quoted. Scan each payload element
    // directly too, plus the env/xargs/find indirection vectors: "env FOO=bar
    // cmd", "xargs rm" and "find … -exec rm" all run a program the argv[0]
    // alone does not name, so every element is scanned, not just argv[0].
    {
        const QString base = QFileInfo(program).fileName().toLower();
        const bool isShell = base == u"sh"_s || base == u"bash"_s || base == u"dash"_s || base == u"zsh"_s
            || base == u"fish"_s || base == u"powershell"_s || base == u"pwsh"_s || base == u"cmd"_s;
        if (isShell) {
            for (int i = 0; i < args.size(); ++i) {
                if (args.at(i) == u"-c"_s && i + 1 < args.size()) {
                    const QString payload = args.at(i + 1);
                    if (isAlwaysDeniedCommand(payload) || commandTouchesDeniedPath(payload)) {
                        if (error) {
                            *error = u"Command payload is blocked by the safety policy."_s;
                        }
                        return {};
                    }
                } else if (isAlwaysDeniedCommand(args.at(i)) || commandTouchesDeniedPath(args.at(i))) {
                    if (error) {
                        *error = u"Command touches a path blocked by the deny rules."_s;
                    }
                    return {};
                }
            }
        } else {
            // Non-shell argv: still scan every element for deny-listed paths
            // (covers "env VAR=…", "xargs …", "find … -exec …").
            for (const QString &element : args) {
                if (isAlwaysDeniedCommand(element) || commandTouchesDeniedPath(element)) {
                    if (error) {
                        *error = u"Command touches a path blocked by the deny rules."_s;
                    }
                    return {};
                }
            }
            if (isAlwaysDeniedCommand(program) || commandTouchesDeniedPath(program)) {
                if (error) {
                    *error = u"Command touches a path blocked by the deny rules."_s;
                }
                return {};
            }
        }
    }

    if (m_profile == SandboxProfile::Off) {
        QStringList out{program};
        out += args;
        return out;
    }

#if defined(Q_OS_LINUX)
    return wrapLinuxBubblewrapArgv(m_workspaceRoot, m_profile, program, args, error);
#elif defined(Q_OS_MACOS)
    return wrapMacSandboxExecArgv(m_workspaceRoot, m_profile, program, args, error);
#else
    // The same as in wrapCommand(): the strict profile cannot be honoured
    // without an isolation mechanism.
    if (m_profile == SandboxProfile::Strict) {
        if (error) {
            *error = u"The strict sandbox profile is not available on this platform, so the command was not run."_s;
        }
        return {};
    }
    QStringList out{program};
    out += args;
    return out;
#endif
}

} // namespace KateAi
