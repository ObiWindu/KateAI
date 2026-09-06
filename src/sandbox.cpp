#include "sandbox.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

static QString globToRegex(const QString &pattern)
{
    QString regex = u"^"_s;
    const QString normalized = QDir::fromNativeSeparators(pattern);
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
    regex += u'$';
    return regex;
}

Sandbox::Sandbox(QString workspaceRoot, SandboxProfile profile, QStringList extraDenyGlobs)
    : m_workspaceRoot(QDir(workspaceRoot).absolutePath())
    , m_profile(profile)
    , m_extraDenyGlobs(std::move(extraDenyGlobs))
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
    const QString name = QFileInfo(haystack).fileName();
    if (re.match(name).hasMatch()) {
        return true;
    }
    if (!pattern.startsWith(u'/') && !pattern.startsWith(u"**"_s)) {
        QRegularExpression nested(globToRegex(u"**/"_s + pattern));
        return nested.match(haystack).hasMatch();
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
            if (relative.startsWith(m_workspaceRoot)) {
                relative = relative.mid(m_workspaceRoot.size());
                if (relative.startsWith(u'/')) {
                    relative = relative.mid(1);
                }
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

    if (QFileInfo::exists(absolute)) {
        const QString canonical = QFileInfo(absolute).canonicalFilePath();
        if (!canonical.isEmpty()) {
            absolute = canonical;
        }
    } else {
        const QString parent = QFileInfo(absolute).absolutePath();
        if (QFileInfo::exists(parent)) {
            const QString parentCanon = QFileInfo(parent).canonicalFilePath();
            if (!parentCanon.isEmpty()) {
                absolute = QDir::cleanPath(parentCanon + u'/' + QFileInfo(absolute).fileName());
            }
        }
    }

    const bool mustStayInWorkspace = m_profile == SandboxProfile::Strict || (m_profile != SandboxProfile::Off && forWrite);
    if (mustStayInWorkspace && !pathInsideWorkspace(absolute)) {
        if (error) {
            *error = u"Path is outside the workspace: %1"_s.arg(absolute);
        }
        return {};
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
    const int slash = token.lastIndexOf(u'/');
    if (slash >= 0) {
        token = token.mid(slash + 1);
    }
    return token;
}

bool Sandbox::isReadOnlyCommand(const QString &command) const
{
    static const QStringList readOnly = {
        u"ls"_s,     u"cat"_s,     u"pwd"_s,     u"date"_s,      u"whoami"_s, u"hostname"_s, u"uptime"_s, u"ps"_s,
        u"head"_s,   u"tail"_s,    u"wc"_s,      u"sort"_s,      u"uniq"_s,   u"tr"_s,       u"cut"_s,    u"grep"_s,
        u"rg"_s,     u"find"_s,    u"file"_s,    u"stat"_s,      u"diff"_s,   u"echo"_s,     u"printf"_s, u"which"_s,
        u"type"_s,   u"env"_s,     u"printenv"_s,
    };

    const QString cmd = command.trimmed();
    static const QRegularExpression writers(uR"(\b(?:tee|rm|mv|cp|chmod|chown|mkdir|touch|dd)\b)"_s);
    if (writers.match(cmd).hasMatch()) {
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
    return readOnly.contains(primary);
}

bool Sandbox::isDangerousCommand(const QString &command) const
{
    const QString cmd = command.trimmed();
    static const QRegularExpression dangerous(
        uR"(\b(?:sudo|su|chmod\s+-R|chown\s+-R|mkfs|shutdown|reboot|systemctl|useradd|userdel|passwd)\b)"_s);
    return dangerous.match(cmd).hasMatch() || isAlwaysDeniedCommand(cmd);
}

bool Sandbox::isAlwaysDeniedCommand(const QString &command) const
{
    const QString cmd = command.trimmed();
    static const QRegularExpression denied(
        uR"((rm\s+(-[a-zA-Z]*f[a-zA-Z]*\s+)?(--no-preserve-root\s+)?/(\s|$))|(\bmkfs\b)|(\bdd\s+.*\bof=/dev/)|(:\(\)\s*\{\s*:\|:&\s*;\s*\})|(\b(curl|wget)\b.*\|\s*(sh|bash|zsh)))"_s);
    return denied.match(cmd).hasMatch();
}

bool Sandbox::bubblewrapAvailable() const
{
    return !QStandardPaths::findExecutable(u"bwrap"_s).isEmpty();
}

QStringList Sandbox::wrapCommand(const QString &command, QString *error) const
{
    if (isAlwaysDeniedCommand(command)) {
        if (error) {
            *error = u"Command is blocked by the safety policy."_s;
        }
        return {};
    }

    if (m_profile == SandboxProfile::Off) {
        return {u"/bin/sh"_s, u"-lc"_s, command};
    }

    const QString bwrap = QStandardPaths::findExecutable(u"bwrap"_s);
    if (bwrap.isEmpty()) {
        if (error) {
            *error = u"bubblewrap (bwrap) is required for sandboxed commands and was not found."_s;
        }
        return {};
    }

    const bool readOnlyFs = m_profile == SandboxProfile::ReadOnly;
    QStringList args;
    args << bwrap << u"--die-with-parent"_s << u"--unshare-pid"_s << u"--unshare-ipc"_s << u"--unshare-uts"_s;
    if (m_profile == SandboxProfile::Strict) {
        // Do not inherit a read-only host root: strict mode must not expose
        // arbitrary host files to shell commands. /usr plus runtime library
        // locations provide the shell and shared libraries on supported Linux
        // systems; project access is mounted explicitly below.
        args << u"--tmpfs"_s << u"/"_s;
        const QStringList runtimeDirectories = {u"/usr"_s, u"/lib"_s, u"/lib64"_s};
        for (const QString &directory : runtimeDirectories) {
            if (QFileInfo::exists(directory)) {
                args << u"--ro-bind"_s << directory << directory;
            }
        }
    } else {
        args << u"--ro-bind"_s << u"/"_s << u"/"_s;
    }
    args << u"--proc"_s << u"/proc"_s << u"--dev"_s << u"/dev"_s;
    args << (readOnlyFs ? u"--ro-bind"_s : u"--bind"_s) << m_workspaceRoot << m_workspaceRoot;
    args << u"--bind"_s << u"/tmp"_s << u"/tmp"_s << u"--chdir"_s << m_workspaceRoot;

    if (m_profile == SandboxProfile::ReadOnly || m_profile == SandboxProfile::Strict) {
        args << u"--unshare-net"_s;
    }

    const QString shell = m_profile == SandboxProfile::Strict ? u"/usr/bin/sh"_s : u"/bin/sh"_s;
    args << u"--"_s << shell << u"-lc"_s << command;
    return args;
}

} // namespace KateAi
