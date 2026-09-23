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
        QVERIFY(command.contains(u"--tmpfs"_s));
        QVERIFY(!command.contains(u"/bin/sh"_s));
        for (int i = 0; i + 2 < command.size(); ++i) {
            QVERIFY(!(command.at(i) == u"--ro-bind"_s && command.at(i + 1) == u"/"_s && command.at(i + 2) == u"/"_s));
        }
    }
};

QTEST_GUILESS_MAIN(TestSandbox)
#include "test_sandbox.moc"
