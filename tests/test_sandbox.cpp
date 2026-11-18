#include "sandbox.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

struct SandboxRunResult
{
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    QString output;
};

SandboxRunResult runSandboxed(const Sandbox &box, const QString &command, int timeoutMs = 20000)
{
    SandboxRunResult result;
    QString error;
    const QStringList wrapped = box.wrapCommand(command, &error);
    if (wrapped.isEmpty()) {
        result.output = error;
        return result;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(box.workspaceRoot());
    process.setProcessEnvironment(box.processEnvironment());
    process.start(wrapped.first(), wrapped.mid(1));
    if (!process.waitForStarted(5000)) {
        result.output = process.errorString();
        return result;
    }
    result.started = true;
    if (!process.waitForFinished(timeoutMs)) {
        result.timedOut = true;
        process.kill();
        process.waitForFinished(3000);
    }
    result.exitCode = process.exitCode();
    result.output = QString::fromUtf8(process.readAll());
    return result;
}

bool bubblewrapRuntimeWorks(SandboxProfile profile)
{
#if defined(Q_OS_LINUX)
    // Probe the wrapper the sandbox actually builds for this profile, not a
    // hand-written subset of its flags. A host can allow a user/PID namespace
    // but reject the network or cgroup namespace (Fedora aarch64 runners have
    // done both); the old probe passed anyway and the strict/read-only runtime
    // tests failed at the first command instead of skipping. Per-profile
    // results keep the workspace runtime test running on hosts where only the
    // network-namespaced profiles are unavailable.
    static QHash<int, bool> cache;
    const int key = static_cast<int>(profile);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd()) {
        return cached.value();
    }

    const bool works = [profile]() {
        QTemporaryDir workspace;
        if (!workspace.isValid()) {
            return false;
        }
        Sandbox box(workspace.path(), profile);
        QString error;
        const QStringList wrapped = box.wrapCommand(u"true"_s, &error);
        if (wrapped.isEmpty()) {
            return false;
        }
        QProcess probe;
        probe.setProcessChannelMode(QProcess::MergedChannels);
        probe.setWorkingDirectory(workspace.path());
        probe.setProcessEnvironment(box.processEnvironment());
        probe.start(wrapped.first(), wrapped.mid(1));
        if (!probe.waitForStarted(5000)) {
            return false;
        }
        if (!probe.waitForFinished(10000)) {
            probe.kill();
            probe.waitForFinished(2000);
            return false;
        }
        return probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    }();
    cache.insert(key, works);
    return works;
#else
    Q_UNUSED(profile);
    return false;
#endif
}

QStringList hostNetworkInterfaces()
{
    QStringList names;
#if defined(Q_OS_LINUX)
    QFile file(u"/proc/net/dev"_s);
    if (!file.open(QIODevice::ReadOnly)) {
        return names;
    }
    const QStringList lines = QString::fromLatin1(file.readAll()).split(u'\n');
    for (int i = 2; i < lines.size(); ++i) {
        const int colon = lines.at(i).indexOf(u':');
        if (colon > 0) {
            names.append(lines.at(i).left(colon).trimmed());
        }
    }
#endif
    return names;
}

QString shellQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(u'\'', u"'\\''"_s);
    return u"'"_s + escaped + u"'"_s;
}

} // namespace

class TestSandbox : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void pathEscapeIsBlocked()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::Workspace);
        QString error;
        QVERIFY(box.allowsWrite(u"src/main.cpp"_s, &error));
        QVERIFY(!box.allowsWrite(u"../outside.txt"_s, &error));
        QVERIFY(error.contains(u"outside"_s));
    }

    void writeOutsideWorkspaceIsBlockedEvenWhenTheDirectoryIsMissing()
    {
        // The containment check used to run only on the canonical path, which
        // does not exist when neither the file nor its parent directory does.
        // A write to <new-dir>/x.txt therefore succeeded in every profile,
        // including Strict. The escape target is a sibling of the workspace
        // under the system temp dir, so it is outside on every platform
        // (Windows included: no Unix-only "/tmp" literal).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = QDir::tempPath() + u"/kateai-sandbox-escape-%1-%2/outside.txt"_s
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(reinterpret_cast<quintptr>(&dir));
        for (SandboxProfile profile : {SandboxProfile::Workspace, SandboxProfile::Strict}) {
            Sandbox box(dir.path(), profile);
            QString error;
            QVERIFY2(!box.allowsWrite(target, &error), "the write was allowed");
            QVERIFY(error.contains(u"outside"_s));
        }
        // The same path inside the workspace is fine.
        Sandbox box(dir.path(), SandboxProfile::Workspace);
        QString error;
        QVERIFY(box.allowsWrite(u"sub/dir/file.txt"_s, &error));
    }

    void readsOutsideTheWorkspaceAreBlocked()
    {
        // The read tools are approved without a prompt, so a profile that
        // does not confine them lets the agent read any file of the host.
        // The "outside" file lives in a second temp dir: portable on every
        // platform (no Unix-only "/etc/passwd" literal, which maps to a
        // drive-relative path on Windows).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const QString outsideFile = outside.filePath(u"secret.txt"_s);
        Sandbox box(dir.path(), SandboxProfile::Workspace);
        QString error;
        QVERIFY(!box.allowsRead(outsideFile, &error));
        // Inside the workspace it still works.
        QFile file(dir.filePath(u"inside.txt"_s));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
        QVERIFY(box.allowsRead(u"inside.txt"_s, &error));
        // The opt-in lifts the confinement again.
        Sandbox open(dir.path(), SandboxProfile::Workspace, {}, true);
        QVERIFY(open.allowsRead(outsideFile, &error));
    }

    void readOnlyCommandRejectsSubstitution()
    {
        // "cat $(...)" has a read-only executable and still runs whatever the
        // substitution expands to. Such a command was auto-approved.
        Sandbox box(QStringLiteral("/tmp"), SandboxProfile::Workspace);
        QVERIFY(!box.isReadOnlyCommand(u"cat \"$(python3 -c 'import os')\""_s));
        QVERIFY(!box.isReadOnlyCommand(u"cat `id`"_s));
        QVERIFY(!box.isReadOnlyCommand(u"ls &"_s));
        // The ordinary read-only commands are unaffected.
        QVERIFY(box.isReadOnlyCommand(u"ls -la"_s));
        QVERIFY(box.isReadOnlyCommand(u"cat file.txt"_s));
        // A newline separates commands just like ';'. The second command used
        // to be auto-approved with the first.
        QVERIFY(!box.isReadOnlyCommand(u"ls\npython3 -c 'print(1)'"_s));
        QVERIFY(!box.isReadOnlyCommand(u"cat f\rpython3 -c 'print(1)'"_s));
        // Privilege escalation is never read-only.
        QVERIFY(!box.isReadOnlyCommand(u"sudo cat /etc/shadow"_s));
        // Attached/output forms of a write flag are not read-only.
        QVERIFY(!box.isReadOnlyCommand(u"sort -oout.txt list.txt"_s));
        QVERIFY(!box.isReadOnlyCommand(u"sort --output=out.txt list.txt"_s));
        QVERIFY(!box.isReadOnlyCommand(u"git branch -D main"_s));
        QVERIFY(!box.isReadOnlyCommand(u"git log --output=out.txt"_s));
    }

    void shellReadsAreConfinedToTheWorkspace()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::Workspace);
        QVERIFY(box.commandTouchesOutsideWorkspace(u"cat /etc/passwd"_s));
        QVERIFY(box.commandTouchesOutsideWorkspace(u"cat ~/.bash_history"_s));
        QVERIFY(!box.commandTouchesOutsideWorkspace(u"cat notes.txt"_s));
        QVERIFY(!box.commandTouchesOutsideWorkspace(u"grep -rn foo src"_s));
        // The opt-in lifts the restriction, matching allowsRead().
        Sandbox open(dir.path(), SandboxProfile::Workspace, {}, true);
        QVERIFY(!open.commandTouchesOutsideWorkspace(u"cat /etc/passwd"_s));
    }

    void envWrapperIsNotReadOnly()
    {
        // "env" with no command only prints the environment, but
        // "env bash -c …" executes an arbitrary program: a read-only
        // allowlist entry would auto-approve code execution in Ask mode.
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QVERIFY(!box.isReadOnlyCommand(u"env bash -c 'echo hi'"_s));
        QVERIFY(!box.isReadOnlyCommand(u"env python3 -c 'print(1)'"_s));
        QVERIFY(!box.isReadOnlyCommand(u"env FOO=1 ls -la"_s));
        // "printenv" only ever prints.
        QVERIFY(box.isReadOnlyCommand(u"printenv"_s));
        QVERIFY(box.isReadOnlyCommand(u"printenv HOME"_s));
    }

    void processSubstitutionIsNotReadOnly()
    {
        // "<(…)" runs a program inline, exactly like "$(…)": the executable
        // in front of it is read-only but the substitution is not.
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QVERIFY(!box.isReadOnlyCommand(u"diff <(python3 -c 'print(1)') b.txt"_s));
        QVERIFY(!box.isReadOnlyCommand(u"sort <(curl http://example.invalid) | head"_s));
        QVERIFY(!box.isReadOnlyCommand(u"cat >(gzip > out.gz)"_s));
        // A plain pipe of read-only commands stays read-only.
        QVERIFY(box.isReadOnlyCommand(u"cat a.txt | sort | head"_s));
    }

    void deniedDirectoryMatchesItself()
    {
        // "**/.ssh/**" did not match the directory itself, so a command that
        // named the directory was not caught.
        QVERIFY(Sandbox::globMatch(u"**/.ssh/**"_s, u"/home/user/.ssh"_s));
        QVERIFY(Sandbox::globMatch(u"**/.ssh/**"_s, u"/home/user/.ssh/id_rsa"_s));
    }

    void envFileIsDenied()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile env(dir.filePath(u".env"_s));
        QVERIFY(env.open(QIODevice::WriteOnly));
        env.write("SECRET=1\n");
        env.close();

        Sandbox box(dir.path(), SandboxProfile::Off);
        QString error;
        QVERIFY(!box.allowsRead(u".env"_s, &error));
        QVERIFY(box.isDenied(dir.filePath(u".env"_s)));
        QVERIFY(Sandbox::globMatch(u"**/.env"_s, dir.filePath(u".env"_s)));
        QVERIFY(Sandbox::globMatch(u"**/*.pem"_s, u"/tmp/cert.pem"_s));
        // The shell removes the backslash; the deny scan must too.
        QVERIFY(box.commandTouchesDeniedPath(u"cat .\\env"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"(cat .env)"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"sh -c 'cat .env'"_s));
    }

    void readOnlyBlocksWrites()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::ReadOnly);
        QString error;
        QVERIFY(box.allowsRead(u"README.md"_s, &error));
        QVERIFY(!box.allowsWrite(u"README.md"_s, &error));
    }

    void strictBlocksOutsideReads()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        Sandbox box(dir.path(), SandboxProfile::Strict);
        QString error;
        QVERIFY(!box.allowsRead(outside.filePath(u"secret.txt"_s), &error));
        QVERIFY(box.allowsRead(u"src/foo.cpp"_s, &error) || error.isEmpty() || !error.contains(u"deny"_s));
        const QString resolved = QDir::fromNativeSeparators(box.resolve(u"src/foo.cpp"_s, &error, true));
        const QString root = QDir::fromNativeSeparators(QDir(dir.path()).canonicalPath().isEmpty()
                                                            ? QDir(dir.path()).absolutePath()
                                                            : QDir(dir.path()).canonicalPath());
#if defined(Q_OS_WIN)
        // Windows filesystems are case-insensitive; the temp dir may be
        // addressed through an 8.3 short name vs. the long canonical form.
        QVERIFY(resolved.startsWith(root, Qt::CaseInsensitive));
#else
        QVERIFY(resolved.startsWith(root));
#endif
    }

    void dangerousCommands()
    {
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf /"_s));
        QVERIFY(box.isDangerousCommand(u"sudo make install"_s));
        QVERIFY(box.isReadOnlyCommand(u"ls -la"_s));
        QVERIFY(box.isReadOnlyCommand(u"git status"_s));
        QVERIFY(!box.isReadOnlyCommand(u"ls && rm -rf src"_s));
    }

    void alwaysDeniedCoversRootHomeAndStarRemoval()
    {
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf /"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf /*"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf //"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf /./*"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf ~"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf ~/Documents"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -r /"_s));
        // The flags and --no-preserve-root may appear in any order.
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf --no-preserve-root /"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm --no-preserve-root -rf /"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"sudo rm -rf ~"_s));
        // Deeper paths are a normal cleanup the permission prompt can judge.
        QVERIFY(!box.isAlwaysDeniedCommand(u"rm -rf /etc"_s));
        QVERIFY(!box.isAlwaysDeniedCommand(u"rm -rf /tmp/build"_s));
        QVERIFY(!box.isAlwaysDeniedCommand(u"rm -rf src"_s));
        QVERIFY(!box.isAlwaysDeniedCommand(u"rm -rf ./build"_s));
    }

    void globPatternCannotWalkAroundDenyGlobs()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile envFile(dir.filePath(u".env"_s));
        QVERIFY(envFile.open(QIODevice::WriteOnly));
        envFile.write("SECRET=1\n");
        envFile.close();
        QVERIFY(QDir().mkpath(dir.filePath(u".kateai"_s)));
        QFile mcpFile(dir.filePath(u".kateai/mcp.json"_s));
        QVERIFY(mcpFile.open(QIODevice::WriteOnly));
        mcpFile.write("{}");
        mcpFile.close();

        Sandbox box(dir.path(), SandboxProfile::Workspace);
        // The pattern text does not match the deny glob, but the shell expands
        // it to the denied file, so the check has to look at the expansion.
        QVERIFY(box.commandTouchesDeniedPath(u"cat .e*"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"cat .*nv"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"cat .kateai/mcp.jso?"_s));
        // A quoted word must not swallow the rest of the command: the token
        // after the closing quote is a path of its own.
        QVERIFY(box.commandTouchesDeniedPath(u"cat \"a b.txt\" .env"_s));
        // A pattern that expands to nothing denied keeps running.
        QVERIFY(!box.commandTouchesDeniedPath(u"cat *.cpp"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"cat nonexistent/*.txt"_s));
    }

    void alwaysDeniedCoversSeparatorAndHomeVariable()
    {
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        // "--" separates the flags from the target, and the home directory can
        // be spelled with a variable. Both used to slip past the rule.
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf -- /"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf $HOME"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf ${HOME}"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm --force --recursive /"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf --no-preserve-root -- /"_s));
        // The program itself may be quoted or spelled with a path.
        QVERIFY(box.isAlwaysDeniedCommand(u"\"/bin/rm\" -rf /"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"/bin/rm -rf /"_s));
        // A command that is not blocked must not be called dangerous either.
        QVERIFY(box.isDangerousCommand(u"rm -rf -- /"_s));
        QVERIFY(!box.isAlwaysDeniedCommand(u"rm -rf -- /etc/keep"_s));
    }

    void emptyWorkspaceRootStaysEmpty()
    {
        // QDir("").absolutePath() is the process working directory: rooting
        // the sandbox there would silently confine to a directory the caller
        // never chose, and every guard that checks for an empty root would
        // stop firing.
        Sandbox box(QString(), SandboxProfile::Workspace);
        QVERIFY(box.workspaceRoot().isEmpty());
    }

    void globWorkspaceMatch()
    {
        QVERIFY(Sandbox::globMatch(u"**/*.cpp"_s, u"src/plugin.cpp"_s));
        QVERIFY(Sandbox::globMatch(u"*.json"_s, u"kateai.json"_s));
        QVERIFY(Sandbox::globMatch(u"**/.ssh/**"_s, u"/home/me/.ssh/id_rsa"_s));
    }

    void globCharacterClasses()
    {
        // A deny glob with a character class used to have its brackets
        // escaped as literals and silently match nothing.
        QVERIFY(Sandbox::globMatch(u"**/*.key[0-9]"_s, u"/home/me/backup.key5"_s));
        QVERIFY(Sandbox::globMatch(u"**/*.key[0-9]"_s, u"backup.key5"_s));
        QVERIFY(!Sandbox::globMatch(u"**/*.key[0-9]"_s, u"/home/me/backup.keyx"_s));
        // Negation, ranges, and an unmatched bracket. A negated class still
        // requires a character, so "confi" alone cannot match "confi[!g]".
        QVERIFY(Sandbox::globMatch(u"**/confi[!g]"_s, u"/home/me/confiv"_s));
        QVERIFY(!Sandbox::globMatch(u"**/confi[!g]"_s, u"/home/me/config"_s));
        QVERIFY(!Sandbox::globMatch(u"**/confi[!g]"_s, u"/home/me/confi"_s));
        QVERIFY(Sandbox::globMatch(u"**/key[a-c]x"_s, u"/home/me/keybx"_s));
        // A ']' right after '[' is a literal member.
        QVERIFY(Sandbox::globMatch(u"**/x[]y]z"_s, u"/home/me/x]z"_s));
        // No closing bracket: a literal '['.
        QVERIFY(Sandbox::globMatch(u"**/a[b"_s, u"/home/me/a[b"_s));
    }

    void symlinkEscapeIsBlocked()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());

        // A directory link that points out of the workspace. The classic
        // escape: the path as written stays inside, the target does not.
        QVERIFY(QDir().mkpath(dir.path() + u"/inside"_s));
        if (!QFile::link(outside.path(), dir.path() + u"/inside/escape"_s)) {
            QSKIP("Symlinks unavailable on this filesystem");
        }
        QFile outsideFile(outside.filePath(u"secret.txt"_s));
        QVERIFY(outsideFile.open(QIODevice::WriteOnly));
        outsideFile.write("secret\n");
        outsideFile.close();

        Sandbox box(dir.path(), SandboxProfile::Workspace);
        QString error;
        // Writing through the link is refused, and says where it resolved to.
        QVERIFY(!box.allowsWrite(u"inside/escape/secret.txt"_s, &error));
        QVERIFY(error.contains(u"outside the workspace"_s));
        QVERIFY(box.resolve(u"inside/escape/secret.txt"_s, &error, true).isEmpty());
        error.clear();
        // So is a brand new file that would land outside via the link.
        QVERIFY(box.resolve(u"inside/escape/new.txt"_s, &error, true).isEmpty());
        QVERIFY(error.contains(u"outside the workspace"_s));
        // The real file inside the workspace is untouched by all of this.
        QVERIFY(box.allowsWrite(u"inside/keep.txt"_s, &error));
    }

    void symlinkEscapeBlockedForReadInStrict()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        QFile outsideFile(outside.filePath(u"secret.txt"_s));
        QVERIFY(outsideFile.open(QIODevice::WriteOnly));
        outsideFile.write("secret\n");
        outsideFile.close();

        QVERIFY(QDir().mkpath(dir.path() + u"/inside"_s));
        if (!QFile::link(outside.path(), dir.path() + u"/inside/escape"_s)) {
            QSKIP("Symlinks unavailable on this filesystem");
        }

        Sandbox box(dir.path(), SandboxProfile::Strict);
        QString error;
        QVERIFY(!box.allowsRead(u"inside/escape/secret.txt"_s, &error));
        QVERIFY(error.contains(u"outside the workspace"_s));
        QVERIFY(box.resolve(u"inside/escape/secret.txt"_s, &error).isEmpty());
    }

    void symlinkInsideWorkspaceStillWorks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.path() + u"/real"_s));
        if (!QFile::link(dir.path() + u"/real"_s, dir.path() + u"/link"_s)) {
            QSKIP("Symlinks unavailable on this filesystem");
        }
        {
            QFile f(dir.path() + u"/real/existing.txt"_s);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }

        Sandbox box(dir.path(), SandboxProfile::Workspace);
        QString error;
        // A link that stays inside the workspace must resolve and keep working.
        // Normalize separators first: canonicalFilePath() may return native
        // backslashes on Windows while the test compares with "/".
        const QString resolved = QDir::fromNativeSeparators(box.resolve(u"link/existing.txt"_s, &error, true));
        QVERIFY2(!resolved.isEmpty(), qPrintable(error));
        const QString root = QDir::fromNativeSeparators(QDir(dir.path()).canonicalPath());
#if defined(Q_OS_WIN)
        QVERIFY(resolved.startsWith(root, Qt::CaseInsensitive));
        QVERIFY(resolved.endsWith(u"existing.txt"_s));
        QVERIFY(resolved.contains(QChar(u'/')));
#else
        QVERIFY(resolved.startsWith(root));
        QVERIFY(resolved.endsWith(u"/existing.txt"_s));
#endif
        QVERIFY(box.allowsWrite(u"link/new.txt"_s, &error));
        // A link that points at the workspace root itself resolves inside too.
        if (QFile::link(dir.path(), dir.path() + u"/rootlink"_s)) {
            QVERIFY(box.allowsRead(u"rootlink/existing.txt"_s, &error));
        }
    }

    void redirectionIsNotReadOnly()
    {
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        // A read-only executable still writes when its output is redirected.
        QVERIFY(!box.isReadOnlyCommand(u"cat notes.txt > out.txt"_s));
        QVERIFY(!box.isReadOnlyCommand(u"echo hi >> ~/.bashrc"_s));
        QVERIFY(!box.isReadOnlyCommand(u"git status > report.md"_s));
        QVERIFY(!box.isReadOnlyCommand(u"ls &> all.txt"_s));
        QVERIFY(!box.isReadOnlyCommand(u"cat a > b && cat c"_s));
        // Descriptor duplication writes to a stream, not a file.
        QVERIFY(box.isReadOnlyCommand(u"ls 2>&1"_s));
        QVERIFY(box.isReadOnlyCommand(u"ls -la /etc 2>&1"_s));
        QVERIFY(box.isReadOnlyCommand(u"cat notes.txt"_s));
    }

    void writeFlagsAreNotReadOnly()
    {
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        // These read the tree but delete or rewrite files, so Ask mode must
        // not auto-approve them just because the executable looks harmless.
        QVERIFY(!box.isReadOnlyCommand(u"find . -name '*.o' -delete"_s));
        QVERIFY(!box.isReadOnlyCommand(u"find . -name '*.c' -fprintf out.h 'x'"_s));
        QVERIFY(!box.isReadOnlyCommand(u"sort -o sorted.txt list.txt"_s));
        QVERIFY(!box.isReadOnlyCommand(u"date -s '2020-01-01' -f stamp.txt"_s));
        // A plain traversal stays read-only.
        QVERIFY(box.isReadOnlyCommand(u"find . -name '*.cpp' -print"_s));
    }

    void shellCannotReachDeniedPaths()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::Workspace);

        // Deny globs are enforced for file tools; the shell must not route around them.
        QVERIFY(box.commandTouchesDeniedPath(u"cat ~/.ssh/id_rsa"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"cat .env"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"cat backup.pem"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"head -n 5 server.key"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"grep -rn token /home/me/.netrc"_s));
        // A flag's value names a denied path too: skipping every '-'
        // prefixed token let --flag=/path/.env walk around the globs.
        QVERIFY(box.commandTouchesDeniedPath(u"tool --config=/home/me/.env"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"tool --input=x/.ssh/id_rsa"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"tool --config='~/.ssh/id_rsa'"_s));
        // Ordinary work must keep running.
        QVERIFY(!box.commandTouchesDeniedPath(u"ls -la"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"cat src/main.cpp"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"git status"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"cat CMakeLists.txt | grep Qt"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"git commit -m 'handle redirect'"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"tool --config=src/main.cpp"_s));
    }

    void denyGlobsMatchWholeSegmentsOnly()
    {
        // "**/.env" used to compile so that every "*.env" file matched: the
        // agent could not read "config/prod.env" although the rule only means
        // the dotfile. The glob now demands a whole path segment.
        Sandbox box(QDir::tempPath(), SandboxProfile::Off);
        QString error;
        QVERIFY(box.allowsRead(u"foo.env"_s, &error));
        QVERIFY(box.allowsRead(u"config/prod.env"_s, &error));
        QVERIFY(!box.allowsRead(u".env"_s, &error));
        QVERIFY(!box.allowsRead(u"config/.env"_s, &error));
        QVERIFY(box.allowsRead(u"foo.ssh"_s, &error));
        QVERIFY(!box.allowsRead(u".ssh/config"_s, &error));
        QVERIFY(!Sandbox::globMatch(u"**/.env"_s, u"/home/user/foo.env"_s));
        QVERIFY(Sandbox::globMatch(u"**/.env"_s, u"/home/user/.env"_s));
    }

    void rootRemovalWithLongFlagsAndQuotesIsDenied()
    {
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        // Long flags and a quoted root used to slip past the always-deny
        // regex, which only knew the short flag forms and a bare "/".
        QVERIFY(box.isAlwaysDeniedCommand(u"rm --force /"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm --recursive --force /"_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf \"/\""_s));
        QVERIFY(box.isAlwaysDeniedCommand(u"rm -rf '/*'"_s));
        // Deeper paths still belong to the permission prompt.
        QVERIFY(!box.isAlwaysDeniedCommand(u"rm --verbose notes.txt"_s));
        QVERIFY(!box.isAlwaysDeniedCommand(u"rm -rf /etc"_s));
    }

    void dateSetAndTailFollowAreNotReadOnly()
    {
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        // date -s writes the system clock; tail -f never terminates. Neither
        // may be auto-approved. date -f only reads dates from a file and
        // stays read-only.
        QVERIFY(box.isReadOnlyCommand(u"date"_s));
        QVERIFY(box.isReadOnlyCommand(u"date -f stamps.txt"_s));
        QVERIFY(!box.isReadOnlyCommand(u"date -s 12:00"_s));
        QVERIFY(!box.isReadOnlyCommand(u"date --set 12:00"_s));
        QVERIFY(!box.isReadOnlyCommand(u"tail -f debug.log"_s));
        QVERIFY(box.isReadOnlyCommand(u"tail -n 20 debug.log"_s));
    }

    void redirectionTargetsCannotReachDeniedPaths()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::Workspace);
        // ">>" used to glue onto the following path, so a redirection aimed
        // at a denied file escaped the check; $HOME did the same.
        QVERIFY(box.commandTouchesDeniedPath(u"echo x >>~/.ssh/authorized_keys"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"echo x > ~/.ssh/authorized_keys"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"cat $HOME/.ssh/id_rsa"_s));
        QVERIFY(box.commandTouchesDeniedPath(u"cat ${HOME}/.netrc"_s));
        // Ordinary redirection targets keep running.
        QVERIFY(!box.commandTouchesDeniedPath(u"echo x >> notes.txt"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"echo done > build.log"_s));
    }

    void wrapCommandRefusesDeniedPaths()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::Workspace);
        QString error;
        QVERIFY(box.wrapCommand(u"echo x"_s, &error).size() > 1); // sanity: a normal command wraps
        error.clear();
        QVERIFY(box.wrapCommand(u"cat .env"_s, &error).isEmpty());
        QVERIFY(error.contains(u"deny"_s));
    }

    void strictCommandsDoNotMountTheHostRoot()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::Strict);
        QString error;
        const QStringList command = box.wrapCommand(u"pwd"_s, &error);
#if defined(Q_OS_LINUX)
        if (command.isEmpty()) {
            QSKIP(qPrintable(error));
        }
        QVERIFY(command.contains(u"--tmpfs"_s));
        // The shell path itself is host-dependent (`/bin/sh` on Fedora, other
        // paths elsewhere); what matters is that the host root is not mounted.
        for (int i = 0; i + 2 < command.size(); ++i) {
            QVERIFY(!(command.at(i) == u"--ro-bind"_s && command.at(i + 1) == u"/"_s && command.at(i + 2) == u"/"_s));
        }
#elif defined(Q_OS_MACOS)
        if (command.isEmpty()) {
            QSKIP(qPrintable(error));
        }
        QVERIFY(command.contains(u"-p"_s));
        QVERIFY(command.join(u' ').contains(u"(deny default)"_s));
#else
        // No isolation mechanism here: the strict profile must be refused
        // rather than run unsandboxed. No SKIP: an empty command is the
        // expected result on this platform.
        QVERIFY(command.isEmpty());
        QVERIFY(error.contains(u"not available"_s));
#endif
    }

    void theHostTmpIsNeverWritableInsideTheSandbox()
    {
        // The /tmp of the host is writable by everybody, so a command could
        // leave files there that outlive the sandbox.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (SandboxProfile profile : {SandboxProfile::Strict, SandboxProfile::ReadOnly, SandboxProfile::Workspace}) {
            Sandbox box(dir.path(), profile);
            QString error;
            const QStringList command = box.wrapCommand(u"pwd"_s, &error);
            if (command.isEmpty()) {
#if defined(Q_OS_LINUX)
                QSKIP(qPrintable(error));
#else
                // Outside Linux there is no bubblewrap: Strict is refused
                // (empty), other profiles run unsandboxed. Neither writes to
                // the host tmp through a sandbox mount, so there is nothing
                // to check.
                continue;
#endif
            }
#if defined(Q_OS_LINUX)
            // A private tmpfs, not the /tmp of the host.
            bool hasPrivateTmp = false;
            for (int i = 0; i + 1 < command.size(); ++i) {
                if (command.at(i) == u"--tmpfs"_s && command.at(i + 1) == u"/tmp"_s) {
                    hasPrivateTmp = true;
                }
                QVERIFY(!(command.at(i) == u"--bind"_s && command.at(i + 1) == u"/tmp"_s));
            }
            QVERIFY(hasPrivateTmp);
#endif
        }
    }

    void theStrictProfileIsRefusedWithoutAnIsolationMechanism()
    {
        // On a platform without bubblewrap or sandbox-exec the strict profile
        // cannot be honoured. The command is refused instead of running
        // unsandboxed.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::Strict);
        QString error;
        const QStringList command = box.wrapCommand(u"pwd"_s, &error);
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
        QVERIFY2(!command.isEmpty() || !error.isEmpty(), "the strict profile must either run or explain itself");
#else
        QVERIFY(command.isEmpty());
        QVERIFY(error.contains(u"not available"_s));
#endif
    }

    void linuxSandboxCommandCarriesNamespaceFlags()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("bubblewrap command construction is Linux-only");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        auto hasPair = [](const QStringList &args, const QString &first, const QString &second) {
            for (int i = 0; i + 1 < args.size(); ++i) {
                if (args.at(i) == first && args.at(i + 1) == second) {
                    return true;
                }
            }
            return false;
        };

        Sandbox strictBox(dir.path(), SandboxProfile::Strict);
        QString error;
        const QStringList strict = strictBox.wrapCommand(u"true"_s, &error);
        if (strict.isEmpty()) {
            QSKIP(qPrintable(error));
        }
        // Namespace isolation: PID, IPC, UTS, cgroup and network have their own
        // view, and capabilities are dropped before the command starts.
        QVERIFY(strict.contains(u"--unshare-pid"_s));
        QVERIFY(strict.contains(u"--unshare-ipc"_s));
        QVERIFY(strict.contains(u"--unshare-uts"_s));
        QVERIFY(strict.contains(u"--unshare-cgroup-try"_s));
        QVERIFY(strict.contains(u"--unshare-net"_s));
        QVERIFY(strict.contains(u"--new-session"_s));
        QVERIFY(hasPair(strict, u"--cap-drop"_s, u"ALL"_s));
        QVERIFY(hasPair(strict, u"--tmpfs"_s, u"/"_s));
        QVERIFY(hasPair(strict, u"--tmpfs"_s, u"/tmp"_s));
        QVERIFY(hasPair(strict, u"--ro-bind"_s, u"/etc"_s));
        // The host root must never be bound read-only as a whole.
        QVERIFY(!hasPair(strict, u"--ro-bind"_s, u"/"_s));

        Sandbox workspaceBox(dir.path(), SandboxProfile::Workspace);
        const QStringList workspace = workspaceBox.wrapCommand(u"true"_s, &error);
        QVERIFY(!workspace.isEmpty());
        QVERIFY(!workspace.contains(u"--unshare-net"_s));
        // The host /tmp is replaced by a private tmpfs, never bound through.
        QVERIFY(hasPair(workspace, u"--tmpfs"_s, u"/tmp"_s));
        QVERIFY(!hasPair(workspace, u"--bind"_s, u"/tmp"_s));
        QVERIFY(hasPair(workspace, u"--ro-bind"_s, u"/"_s));
        // Sandboxed commands do not source the login profile, which could
        // re-export the variables the environment policy removed.
        QVERIFY(workspace.contains(u"-c"_s));
        QVERIFY(!workspace.contains(u"-lc"_s));
        // A credential directory that exists on the host is masked with an
        // empty tmpfs inside the sandbox, not merely blocked by the scanner.
        const QString sshDirectory = QDir::homePath() + u"/.ssh"_s;
        if (QFileInfo(sshDirectory).isDir()) {
            QVERIFY2(hasPair(workspace, u"--tmpfs"_s, sshDirectory),
                     "the SSH directory was not masked in the bubblewrap command");
        }

        Sandbox readOnlyBox(dir.path(), SandboxProfile::ReadOnly);
        const QStringList readOnly = readOnlyBox.wrapCommand(u"true"_s, &error);
        QVERIFY(!readOnly.isEmpty());
        QVERIFY(readOnly.contains(u"--unshare-net"_s));
        QVERIFY(hasPair(readOnly, u"--tmpfs"_s, u"/tmp"_s));

        // A strict sandbox rooted at "/" would bind the host root writable over
        // the tmpfs root, so it is refused instead of pretending to isolate.
        Sandbox rootBox(QDir::rootPath(), SandboxProfile::Strict);
        error.clear();
        QVERIFY(rootBox.wrapCommand(u"true"_s, &error).isEmpty());
        QVERIFY(error.contains(u"filesystem root"_s));
#endif
    }

    void sandboxEnvironmentRemovesSecretsAndSessionChannels()
    {
#if !defined(Q_OS_UNIX)
        QSKIP("Unix environment policy test");
#else
        const QByteArray oldSecret = qgetenv("KATEAI_TEST_API_KEY");
        const QByteArray oldVisible = qgetenv("KATEAI_TEST_VISIBLE");
        const QByteArray oldSock = qgetenv("SSH_AUTH_SOCK");
        const QByteArray oldDisplay = qgetenv("DISPLAY");

        qputenv("KATEAI_TEST_API_KEY", "do-not-leak");
        qputenv("KATEAI_TEST_VISIBLE", "visible-value");
        qputenv("SSH_AUTH_SOCK", "/tmp/fake-agent.sock");
        qputenv("DISPLAY", ":99");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox workspaceBox(dir.path(), SandboxProfile::Workspace);
        const QProcessEnvironment env = workspaceBox.processEnvironment();
        QVERIFY(!env.contains(u"KATEAI_TEST_API_KEY"_s));
        QCOMPARE(env.value(u"KATEAI_TEST_VISIBLE"_s), u"visible-value"_s);
        QVERIFY(!env.contains(u"SSH_AUTH_SOCK"_s));
        QVERIFY(!env.contains(u"DISPLAY"_s));
        QCOMPARE(env.value(u"TMPDIR"_s), u"/tmp"_s);
        QVERIFY(!env.value(u"XDG_RUNTIME_DIR"_s).isEmpty());

        Sandbox strictBox(dir.path(), SandboxProfile::Strict);
        const QProcessEnvironment strictEnv = strictBox.processEnvironment();
#if defined(Q_OS_LINUX)
        QCOMPARE(strictEnv.value(u"HOME"_s), u"/tmp/kateai-home"_s);
        QCOMPARE(strictEnv.value(u"XDG_RUNTIME_DIR"_s), u"/tmp/kateai-home/run"_s);
        QCOMPARE(strictEnv.value(u"TMPDIR"_s), u"/tmp"_s);
        QCOMPARE(strictEnv.value(u"PATH"_s), u"/usr/local/bin:/usr/bin:/bin"_s);
#endif

        // Off is explicitly unsandboxed and keeps the ambient environment.
        Sandbox offBox(dir.path(), SandboxProfile::Off);
        QCOMPARE(offBox.processEnvironment().value(u"KATEAI_TEST_VISIBLE"_s), u"visible-value"_s);

        auto restore = [](const char *name, const QByteArray &value) {
            if (value.isNull()) {
                qunsetenv(name);
            } else {
                qputenv(name, value);
            }
        };
        restore("KATEAI_TEST_API_KEY", oldSecret);
        restore("KATEAI_TEST_VISIBLE", oldVisible);
        restore("SSH_AUTH_SOCK", oldSock);
        restore("DISPLAY", oldDisplay);
#endif
    }

    void strictRuntimeConfinesFilesystemNetworkPidAndEnvironment()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("bubblewrap runtime isolation is Linux-only");
#else
        if (!bubblewrapRuntimeWorks(SandboxProfile::Strict)) {
            QSKIP("bubblewrap cannot create the required strict namespaces in this environment");
        }

        QTemporaryDir workspace;
        QTemporaryDir outside;
        QVERIFY(workspace.isValid());
        QVERIFY(outside.isValid());
        const QString secretPath = outside.filePath(u"secret.txt"_s);
        {
            QFile secret(secretPath);
            QVERIFY(secret.open(QIODevice::WriteOnly));
            secret.write("outside-secret\n");
        }
        {
            QFile inside(workspace.filePath(u"inside.txt"_s));
            QVERIFY(inside.open(QIODevice::WriteOnly));
            inside.write("inside\n");
        }

        Sandbox box(workspace.path(), SandboxProfile::Strict);

        // A path outside the workspace is not mounted at all.
        SandboxRunResult result = runSandboxed(box, u"cat "_s + shellQuote(secretPath) + u" 2>&1"_s);
        if (!result.started) {
            QSKIP(qPrintable(result.output));
        }
        QVERIFY2(result.exitCode != 0, qPrintable(result.output));
        QVERIFY(!result.output.contains(u"outside-secret"_s));

        // The workspace is writable through the relative path...
        result = runSandboxed(box, u"printf 'workspace-write' > relative.txt && cat relative.txt"_s);
        QVERIFY2(result.exitCode == 0, qPrintable(result.output));
        QVERIFY(result.output.contains(u"workspace-write"_s));
        QVERIFY(QFileInfo::exists(workspace.filePath(u"relative.txt"_s)));

        // ...and through its absolute host path.
        result = runSandboxed(box, u"printf 'absolute-write' > "_s
                                       + shellQuote(workspace.filePath(u"absolute.txt"_s)));
        QVERIFY2(result.exitCode == 0, qPrintable(result.output));
        QVERIFY(QFileInfo::exists(workspace.filePath(u"absolute.txt"_s)));

        // Writing to a sibling of the workspace does not reach the host.
        const QString escapePath = outside.filePath(u"escaped.txt"_s);
        result = runSandboxed(box, u"echo escaped > "_s + shellQuote(escapePath));
        QVERIFY2(result.exitCode != 0, qPrintable(result.output));
        QVERIFY(!QFileInfo::exists(escapePath));

        // A file in the host /tmp is not visible: /tmp is a private tmpfs.
        QTemporaryFile hostTmp(QDir::tempPath() + u"/kateai-host-tmp-XXXXXX"_s);
        QVERIFY(hostTmp.open());
        hostTmp.write("host-tmp-secret\n");
        hostTmp.flush();
        result = runSandboxed(box, u"cat "_s + shellQuote(hostTmp.fileName()) + u" 2>&1"_s);
        QVERIFY(result.exitCode != 0);
        QVERIFY(!result.output.contains(u"host-tmp-secret"_s));

        // The host PID is not visible in the new PID namespace.
        result = runSandboxed(box, u"test -e /proc/"_s
                                       + QString::number(QCoreApplication::applicationPid())
                                       + u" && echo visible || echo hidden"_s);
        QVERIFY2(result.exitCode == 0, qPrintable(result.output));
        QVERIFY(result.output.contains(u"hidden"_s));
        QVERIFY(!result.output.contains(u"visible"_s));

        // The strict profile gets a private HOME, not the real one.
        result = runSandboxed(box, u"printf %s \"$HOME\""_s);
        QVERIFY2(result.exitCode == 0, qPrintable(result.output));
        QCOMPARE(result.output.trimmed(), u"/tmp/kateai-home"_s);

        // The network namespace has only loopback; the host interfaces are gone.
        const QStringList hostInterfaces = hostNetworkInterfaces();
        result = runSandboxed(box, u"cat /proc/net/dev"_s);
        QVERIFY2(result.exitCode == 0, qPrintable(result.output));
        QVERIFY(result.output.contains(u"lo:"_s));
        for (const QString &interface : hostInterfaces) {
            if (interface == u"lo"_s || interface.isEmpty()) {
                continue;
            }
            QVERIFY2(!result.output.contains(interface + u':'),
                     qPrintable(u"host interface %1 leaked into the sandbox"_s.arg(interface)));
        }

        // Ambient credentials are not present in the command's environment.
        qputenv("KATEAI_TEST_API_KEY", "do-not-leak");
        result = runSandboxed(box, u"printenv KATEAI_TEST_API_KEY || true"_s);
        qunsetenv("KATEAI_TEST_API_KEY");
        QVERIFY2(result.exitCode == 0, qPrintable(result.output));
        QVERIFY(!result.output.contains(u"do-not-leak"_s));
#endif
    }

    void workspaceRuntimeCannotWriteOutsideTheWorkspace()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("bubblewrap runtime isolation is Linux-only");
#else
        if (!bubblewrapRuntimeWorks(SandboxProfile::Workspace)) {
            QSKIP("bubblewrap cannot create the required workspace sandbox in this environment");
        }

        QTemporaryDir workspace;
        QTemporaryDir outside;
        QVERIFY(workspace.isValid());
        QVERIFY(outside.isValid());

        Sandbox box(workspace.path(), SandboxProfile::Workspace);

        SandboxRunResult result = runSandboxed(box, u"echo inside > inside.txt"_s);
        if (!result.started) {
            QSKIP(qPrintable(result.output));
        }
        QVERIFY2(result.exitCode == 0, qPrintable(result.output));
        QVERIFY(QFileInfo::exists(workspace.filePath(u"inside.txt"_s)));

        const QString escapePath = outside.filePath(u"escaped.txt"_s);
        result = runSandboxed(box, u"echo escaped > "_s + shellQuote(escapePath));
        QVERIFY2(result.exitCode != 0, qPrintable(result.output));
        QVERIFY(!QFileInfo::exists(escapePath));

        // The workspace is writable even when named by its absolute path.
        result = runSandboxed(box, u"echo absolute > "_s
                                       + shellQuote(workspace.filePath(u"absolute.txt"_s)));
        QVERIFY2(result.exitCode == 0, qPrintable(result.output));
        QVERIFY(QFileInfo::exists(workspace.filePath(u"absolute.txt"_s)));
#endif
    }

    void readOnlyRuntimeBlocksWritesAndNetwork()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("bubblewrap runtime isolation is Linux-only");
#else
        if (!bubblewrapRuntimeWorks(SandboxProfile::ReadOnly)) {
            QSKIP("bubblewrap cannot create the required read-only namespaces in this environment");
        }

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Sandbox box(workspace.path(), SandboxProfile::ReadOnly);

        SandboxRunResult result = runSandboxed(box, u"echo nope > blocked.txt"_s);
        if (!result.started) {
            QSKIP(qPrintable(result.output));
        }
        QVERIFY2(result.exitCode != 0, qPrintable(result.output));
        QVERIFY(!QFileInfo::exists(workspace.filePath(u"blocked.txt"_s)));

        const QStringList hostInterfaces = hostNetworkInterfaces();
        result = runSandboxed(box, u"cat /proc/net/dev"_s);
        QVERIFY2(result.exitCode == 0, qPrintable(result.output));
        QVERIFY(result.output.contains(u"lo:"_s));
        for (const QString &interface : hostInterfaces) {
            if (interface == u"lo"_s || interface.isEmpty()) {
                continue;
            }
            QVERIFY2(!result.output.contains(interface + u':'),
                     qPrintable(u"host interface %1 leaked into the sandbox"_s.arg(interface)));
        }
#endif
    }

    void sandboxedWorkingDirectoryIsConfinedToTheWorkspace()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("bubblewrap working-directory handling is Linux-only");
#else
        QTemporaryDir workspace;
        QTemporaryDir outside;
        QVERIFY(workspace.isValid());
        QVERIFY(outside.isValid());
        QVERIFY(QDir().mkpath(workspace.filePath(u"sub/dir"_s)));

        Sandbox box(workspace.path(), SandboxProfile::Workspace);
        QString error;
        const QStringList inside = box.wrapCommand(u"pwd"_s, &error, workspace.filePath(u"sub/dir"_s));
        QVERIFY2(!inside.isEmpty(), qPrintable(error));
        bool changed = false;
        for (int i = 0; i + 1 < inside.size(); ++i) {
            if (inside.at(i) == u"--chdir"_s && inside.at(i + 1) == workspace.filePath(u"sub/dir"_s)) {
                changed = true;
            }
        }
        QVERIFY(changed);

        error.clear();
        const QStringList escaped = box.wrapCommand(u"pwd"_s, &error, outside.path());
        QVERIFY(escaped.isEmpty());
        QVERIFY(error.contains(u"workspace"_s));
#endif
    }

    void wrapArgvDoesNotRequoteThroughBashDashC()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::Off);
        QString error;
        const QString script = u"printf %s 'quoted ok'"_s;
        const QStringList wrapped = box.wrapArgv(u"/bin/bash"_s, {u"-c"_s, script}, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(wrapped, (QStringList{u"/bin/bash"_s, u"-c"_s, script}));
    }

    void wrapCommandPassesShellLineAsOneArgument()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Sandbox box(dir.path(), SandboxProfile::Off);
        QString error;
        const QString line = u"/bin/bash -c 'printf %s quoted-ok'"_s;
        const QStringList wrapped = box.wrapCommand(line, &error);
        QVERIFY(error.isEmpty());
        QVERIFY(wrapped.size() >= 3);
        QCOMPARE(wrapped.last(), line);
        QCOMPARE(wrapped.at(wrapped.size() - 2), u"-lc"_s);
    }
};

QTEST_GUILESS_MAIN(TestSandbox)
#include "test_sandbox.moc"
