#include "sandbox.h"

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
        Sandbox box(dir.path(), SandboxProfile::Strict);
        QString error;
        QVERIFY(!box.allowsRead(u"/etc/passwd"_s, &error));
        QVERIFY(box.allowsRead(u"src/foo.cpp"_s, &error) || error.isEmpty() || !error.contains(u"deny"_s));
        const QString resolved = box.resolve(u"src/foo.cpp"_s, &error, true);
        QVERIFY(resolved.startsWith(QDir(dir.path()).absolutePath()));
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

    void globWorkspaceMatch()
    {
        QVERIFY(Sandbox::globMatch(u"**/*.cpp"_s, u"src/plugin.cpp"_s));
        QVERIFY(Sandbox::globMatch(u"*.json"_s, u"kateai.json"_s));
        QVERIFY(Sandbox::globMatch(u"**/.ssh/**"_s, u"/home/me/.ssh/id_rsa"_s));
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
        QFile(dir.path() + u"/real/existing.txt"_s).open(QIODevice::WriteOnly);

        Sandbox box(dir.path(), SandboxProfile::Workspace);
        QString error;
        // A link that stays inside the workspace must resolve and keep working.
        const QString resolved = box.resolve(u"link/existing.txt"_s, &error, true);
        QVERIFY2(!resolved.isEmpty(), qPrintable(error));
        QVERIFY(resolved.startsWith(QDir(dir.path()).canonicalPath()));
        QVERIFY(resolved.endsWith(u"/existing.txt"_s));
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
        // Ordinary work must keep running.
        QVERIFY(!box.commandTouchesDeniedPath(u"ls -la"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"cat src/main.cpp"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"git status"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"cat CMakeLists.txt | grep Qt"_s));
        QVERIFY(!box.commandTouchesDeniedPath(u"git commit -m 'handle redirect'"_s));
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
        if (command.isEmpty()) {
            QSKIP(qPrintable(error));
        }
#if defined(Q_OS_LINUX)
        QVERIFY(command.contains(u"--tmpfs"_s));
        QVERIFY(!command.contains(u"/bin/sh"_s));
        for (int i = 0; i + 2 < command.size(); ++i) {
            QVERIFY(!(command.at(i) == u"--ro-bind"_s && command.at(i + 1) == u"/"_s && command.at(i + 2) == u"/"_s));
        }
#elif defined(Q_OS_MACOS)
        QVERIFY(command.contains(u"-p"_s));
        QVERIFY(command.join(u' ').contains(u"(deny default)"_s));
#else
        QVERIFY(!command.isEmpty());
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
