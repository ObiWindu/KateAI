#include "sandbox.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

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
        QVERIFY(!command.contains(u"/bin/sh"_s));
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
        for (SandboxProfile profile : {SandboxProfile::Strict, SandboxProfile::ReadOnly}) {
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
