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
                regex += u".*"_s;
                ++i;
                if (i + 1 < normalized.size() && normalized.at(i + 1) == u'/') {
                    regex += u"/?"_s;
                    ++i;
                }
            } else if (c == u'*') {
                regex += u"[^/]*"_s;
            } else if (c == u'?') {
                regex += u"[^/]"_s;
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
    : m_workspaceRoot(QDir(workspaceRoot).absolutePath())
    , m_profile(profile)
    , m_extraDenyGlobs(std::move(extraDenyGlobs))
    , m_allowOutsideReads(allowOutsideReads)
    {
        m_workspaceRoot = QDir::cleanPath(m_workspaceRoot);
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

    bool Sandbox::isDenied(const QString &path) const
    {
        const QString unix = QDir::fromNativeSeparators(path);
        for (const QString &glob : denyGlobs()) {
            if (globMatch(glob, unix)) {
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
                    if (globMatch(glob, relative) || globMatch(glob, u"/"_s + relative)) {
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
        const QString path = QDir::cleanPath(absolutePath);
        if (path == root) {
            return true;
        }
        return path.startsWith(root + u'/');
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

    // Resolve symlinks safely - use canonicalFilePath which resolves all symlinks
    // To mitigate TOCTOU, we verify the path is still within workspace after resolution.
    QFileInfo fileInfo(absolute);
    QString canonical = fileInfo.canonicalFilePath();
    if (!canonical.isEmpty()) {
        absolute = canonical;
        // Re-check workspace containment AFTER symlink resolution
        if (mustStayInWorkspace && !pathInsideWorkspace(absolute)) {
            if (error) {
                *error = u"Path resolves outside the workspace: %1"_s.arg(absolute);
            }
            return {};
        }
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
            const QFileInfo info(QDir(m_workspaceRoot), path);
            const QString absolute = QDir::cleanPath(info.absoluteFilePath());
            if (isDenied(absolute)) {
                if (error) {
                    *error = u"Access to this path is blocked by a deny rule: %1"_s.arg(absolute);
                }
                return false;
            }
            return true;
        }

        if (m_profile == SandboxProfile::Workspace || m_profile == SandboxProfile::ReadOnly) {
            const QFileInfo info(QDir(m_workspaceRoot), path);
            QString absolute = QDir::cleanPath(info.absoluteFilePath());
            if (QFileInfo::exists(absolute)) {
                const QString canonical = QFileInfo(absolute).canonicalFilePath();
                if (!canonical.isEmpty()) {
                    absolute = canonical;
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

// Flags that make an otherwise read-only command write to disk. Only commands
// already on the read-only list are checked against this, so a flag can never
// pull a new command onto the allowlist - it can only take one off it.
static bool hasWriteFlag(const QString &primary, const QString &command)
{
    static const QHash<QString, QStringList> writeFlags = {
        {u"find"_s,
         {u"-delete"_s, u"-exec"_s, u"-execdir"_s, u"-ok"_s, u"-okdir"_s,
          u"-fls"_s, u"-fprint"_s, u"-fprint0"_s, u"-fprintf"_s}},
        {u"sort"_s, {u"-o"_s, u"--output"_s}},
        {u"date"_s, {u"-f"_s, u"--file"_s}},
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
        u"type"_s,   u"env"_s,     u"printenv"_s, u"dir"_s,      u"where"_s,  u"where.exe"_s,
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
    static const QRegularExpression denied(
        uR"((rm\s+(-[a-zA-Z]*f[a-zA-Z]*\s+)?(--no-preserve-root\s+)?/(\s|$))|(\bmkfs\b)|(\bdd\s+.*\bof=/dev/)|(:\(\)\s*\{\s*:\|:&\s*;\s*\})|(\b(curl|wget)\b.*\|\s*(?:sudo\s+)?(sh|bash|zsh|fish|dash|ksh|cmd|cmd\.exe|powershell|powershell\.exe|pwsh|python[0-9.]*|perl|ruby|node)\b)|(\bformat\s+[a-zA-Z]:)|(\bdiskpart\b)|(\brmdir\s+/s\s+/q\s+[a-zA-Z]:\\)|(\bRemove-Item\s+.*-Recurse.*[A-Z]:\\))"_s,
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
        } else if ((c.isSpace() || c == u';' || c == u'|' || c == u'&' || c == u'<') && !inSingleQuote && !inDoubleQuote) {
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
            }
        } else {
            currentToken.append(c);
        }
    }
    if (!currentToken.isEmpty()) {
        tokens.append(currentToken);
    }
    
    for (const QString &token : tokens) {
        if (token.startsWith(u'-')) {
            continue; // a flag is not a path
        }
        QString candidate = token;
        // Strip surrounding quotes
        if ((candidate.startsWith(u'\'') && candidate.endsWith(u'\'')) ||
            (candidate.startsWith(u'"') && candidate.endsWith(u'"'))) {
            candidate = candidate.mid(1, candidate.size() - 2);
        }
        if (candidate == u"~"_s) {
            candidate = QDir::homePath();
        } else if (candidate.startsWith(u"~/"_s)) {
            candidate = QDir::homePath() + candidate.mid(1);
        }
        // Check the path as written, then the symlink-resolved form, so a denied
        // name is caught even when the file does not exist yet.
        const QString literal = QDir::cleanPath(QFileInfo(QDir(m_workspaceRoot), candidate).absoluteFilePath());
        if (isDenied(literal)) {
            return true;
        }
        // Check both read and write resolution - shell commands can write via
        // redirections (>, >>, &>) even if the command itself is read-only.
        const QString resolvedRead = normalizePath(candidate, nullptr, false);
        if (!resolvedRead.isEmpty() && resolvedRead != literal && isDenied(resolvedRead)) {
            return true;
        }
        const QString resolvedWrite = normalizePath(candidate, nullptr, true);
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
    quoted.replace(u'\\', u"\\\\"_s);
    quoted.replace(u'"', u"\\\""_s);
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
