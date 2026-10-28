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
            : QDir::cleanPath(QDir(workspaceRoot).absolutePath());
        // Canonicalize the workspace root to resolve symlinks (e.g., /var vs /private/var on macOS)
        if (!m_workspaceRoot.isEmpty()) {
            const QString canonicalRoot = QFileInfo(m_workspaceRoot).canonicalFilePath();
            if (!canonicalRoot.isEmpty()) {
                m_workspaceRoot = QDir::cleanPath(canonicalRoot);
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
    QRegularExpression re(globToRegex(pattern));
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
        QRegularExpression nested(globToRegex(u"**/"_s + pattern));
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
        it = cache.insert(regex, QRegularExpression(regex));
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
        // Also try: canonicalize root only, compare non-canonical path with canonical root's non-canonical form
        // Handle case where root is canonical but path is not (or vice versa)
        const QString rootCanon = QFileInfo(root).canonicalFilePath();
        if (!rootCanon.isEmpty()) {
            const QString rootCanonClean = QDir::cleanPath(rootCanon);
            // If path starts with root's original form's parent canonical...
            // Try comparing path against the canonical root by checking if
            // the path, when its existing ancestors are canonicalized, matches
            if (canonicalPath.isEmpty()) {
                // path doesn't exist and parent doesn't exist either; fall back
                // to lexical check with the canonical root's directory name
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

static QString windowsAbsoluteCandidate(const QString &workspaceRoot, QString candidate)
{
#ifdef Q_OS_WIN
    if (!workspaceRoot.isEmpty() && !candidate.isEmpty()
        && (candidate.startsWith(u'/') || candidate.startsWith(u'\\'))
        && !QDir::isAbsolutePath(candidate)) {
        QString driveRoot;
        if (workspaceRoot.size() >= 2 && workspaceRoot.at(1) == u':') {
            driveRoot = workspaceRoot.left(3);
        } else {
            driveRoot = QDir::rootPath();
        }
        if (!driveRoot.endsWith(u'/') && !driveRoot.endsWith(u'\\')) {
            driveRoot += u'/';
        }
        candidate = driveRoot + candidate.mid(1);
    }
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
    QString absolute = QDir::cleanPath(info.absoluteFilePath());

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
    QString canonical = fileInfo.canonicalFilePath();
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
        QString parentCanon = parentInfo.canonicalFilePath();
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
            QString absolute = QDir::cleanPath(info.absoluteFilePath());
            if (QFileInfo::exists(absolute)) {
                const QString canonical = QFileInfo(absolute).canonicalFilePath();
                if (!canonical.isEmpty()) {
                    absolute = canonical;
                }
            } else {
                // Try to canonicalize parent
                const QString parent = QFileInfo(absolute).absolutePath();
                const QString parentCanon = QFileInfo(parent).canonicalFilePath();
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
            QString absolute = QDir::cleanPath(info.absoluteFilePath());
            if (QFileInfo::exists(absolute)) {
                const QString canonical = QFileInfo(absolute).canonicalFilePath();
                if (!canonical.isEmpty()) {
                    absolute = canonical;
                }
            } else {
                // Try to canonicalize parent directory for files that don't exist yet
                const QString parent = QFileInfo(absolute).absolutePath();
                const QString parentCanon = QFileInfo(parent).canonicalFilePath();
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
        {u"sort"_s, {u"-o"_s, u"--output"_s}},
        // -s/--set writes the system clock; -f/--file only reads dates from a
        // file and stays read-only.
        {u"date"_s, {u"-s"_s, u"--set"_s}},
        {u"tail"_s, {u"-f"_s, u"-F"_s, u"--follow"_s}},
    };
    const QStringList flags = writeFlags.value(primary);
    if (flags.isEmpty()) {
        return false;
    }
    const QStringList tokens = command.split(QRegularExpression(uR"(\s+)"_s), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (flags.contains(token)) {
            return true;
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
    if (cmd.contains(u"&&"_s) || cmd.contains(u"||"_s) || cmd.contains(u';') || cmd.contains(u'|')) {
        const QStringList parts = cmd.split(QRegularExpression(uR"(\s*(?:&&|\|\||;|\|)\s*)"_s));
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
    // The rm branch covers a recursive or forced removal of the filesystem
    // root, of everything directly below it ("rm -rf /*"), or of the home
    // directory ("rm -rf ~", "rm -rf ~/…"), with the root written bare or
    // quoted ("rm -rf /", "rm -rf \"/\""). The flags - the short forms, the
    // long forms --force/--recursive, and --no-preserve-root - may appear in
    // any order and any combination. Deeper absolute paths are not matched:
    // they are a normal cleanup the permission prompt can judge.
    static const QRegularExpression denied(
        uR"((rm\s+((?:-[a-zA-Z]*[rf][a-zA-Z]*|--no-preserve-root|--force|--recursive)\s+)+(/(\s|$|\*+)|["']/["']?(\s|$|\*+)|~(/\S*)?(\s|$)))|(\bmkfs\b)|(\bdd\s+.*\bof=/dev/)|(:\(\)\s*\{\s*:\|:&\s*;\s*\})|(\b(curl|wget)\b.*\|\s*(?:sudo\s+)?(sh|bash|zsh|fish|dash|ksh|cmd|cmd\.exe|powershell|powershell\.exe|pwsh|python[0-9.]*|perl|ruby|node)\b)|(\bformat\s+[a-zA-Z]:)|(\bdiskpart\b)|(\brmdir\s+/s\s+/q\s+[a-zA-Z]:\\)|(\bRemove-Item\s+.*-Recurse.*[A-Z]:\\))"_s,
        QRegularExpression::CaseInsensitiveOption);
    return denied.match(cmd).hasMatch();
}

bool Sandbox::commandTouchesDeniedPath(const QString &command) const
{
    if (m_workspaceRoot.isEmpty()) {
        return false;
    }
    // This is not a shell parser. It scans tokens and asks the deny rules about
    // each one, which is enough to stop "cat ~/.ssh/id_rsa" and "cat .env" from
    // walking around the guard the file tools apply.
    //
    // Bare words are checked against the workspace root too, because that is the
    // working directory of every sandboxed command: "cat backup.pem" is a real
    // deny-glob hit. Over-blocking is the safe direction here — the agent has the
    // glob tool for pattern searches, and a blocked call reports why.
    
    // Parse command respecting quoted strings
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
            if (valueStart < 0 || valueStart >= candidate.size()) {
                continue;
            }
            candidate = candidate.mid(valueStart);
        }
        // Strip quote characters wherever they appear, not only as a matched
        // surrounding pair: "~/"x"" and 'a'"b" spellings must not defeat the
        // scan by mixing quotes. Over-blocking is the safe direction here.
        candidate.remove(u'"');
        candidate.remove(u'\'');
        if (candidate.isEmpty()) {
            continue;
        }
        if (candidate == u"~"_s) {
            candidate = QDir::homePath();
        } else if (candidate.startsWith(u"~/"_s)) {
            candidate = QDir::homePath() + candidate.mid(1);
        }
        // $HOME names the same directory as "~", so a denied path must not
        // slip past the check by spelling it with a variable. Other variables
        // cannot be resolved here; those tokens are checked as written.
        static const QRegularExpression homeVar(uR"(\$\{HOME\}|\$HOME(?![A-Za-z0-9_]))"_s);
        candidate.replace(homeVar, QDir::homePath());
        const QString adjustedCandidate = windowsAbsoluteCandidate(m_workspaceRoot, candidate);
        // Check the path as written, then the symlink-resolved form, so a denied
        // name is caught even when the file does not exist yet.
        const QString literal = QDir::cleanPath(QFileInfo(QDir(m_workspaceRoot), adjustedCandidate).absoluteFilePath());
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
        // The read-only profile may write to a scratch directory, but not to
        // the /tmp of the host: a file left there outlives the sandbox and is
        // visible to every other process of the machine.
        if (readOnlyFs) {
            args << u"--tmpfs"_s << u"/tmp"_s;
        } else {
            args << u"--bind"_s << u"/tmp"_s << u"/tmp"_s;
        }
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
    args << u"--"_s << findShellExecutable() << u"-lc"_s << command;
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

    return {sandboxExec, u"-p"_s, profileText, findShellExecutable(), u"-lc"_s, command};
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
        return unsandboxedArgv(command);
    }

#if defined(Q_OS_LINUX)
    return wrapLinuxBubblewrap(m_workspaceRoot, m_profile, command, error);
#elif defined(Q_OS_MACOS)
    return wrapMacSandboxExec(m_workspaceRoot, m_profile, command, error);
#else
    // There is no isolation mechanism on this platform. The strict profile
    // promises that the command cannot reach the rest of the system, and that
    // promise cannot be kept here. Running the command unsandboxed would
    // silently break it, so the command is refused instead.
    if (m_profile == SandboxProfile::Strict) {
        if (error) {
            *error = u"The strict sandbox profile is not available on this platform, so the command was not run."_s;
        }
        return {};
    }
    return unsandboxedArgv(command);
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
