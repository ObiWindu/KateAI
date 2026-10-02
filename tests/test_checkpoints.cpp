#include "checkpoint.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestCheckpoints : public QObject
{
    Q_OBJECT

private:
    static void writeFile(const QString &path, const QString &contents)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        file.write(contents.toUtf8());
    }

    static QString readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }

private Q_SLOTS:
    void initTestCase()
    {
        // Checkpoints need git; skip rather than fail when it is missing.
        QProcess probe;
        probe.start(u"git"_s, QStringList{u"--version"_s});
        m_hasGit = probe.waitForFinished(5000) && probe.exitCode() == 0;
    }

    void init()
    {
        m_workspace = std::make_unique<QTemporaryDir>();
        QVERIFY(m_workspace->isValid());
    }

    void cleanup()
    {
        m_workspace.reset();
    }

    void disabledManagerDoesNothing()
    {
        CheckpointManager manager;
        manager.setWorkspace(m_workspace->path());
        manager.setEnabled(false);
        QVERIFY(!manager.isAvailable());
        QString error;
        QVERIFY(manager.createCheckpoint(u"nope"_s, &error).isEmpty());
        QVERIFY(manager.checkpoints().isEmpty());
    }

    void createAndListSnapshots()
    {
        if (!m_hasGit) {
            QSKIP("git is not available");
        }
        writeFile(m_workspace->path() + u"/a.txt"_s, u"original"_s);

        CheckpointManager manager;
        manager.setWorkspace(m_workspace->path());
        manager.setRetention(10);
        QVERIFY(manager.isAvailable());

        QString error;
        const QString first = manager.createCheckpoint(u"first"_s, &error);
        QVERIFY2(!first.isEmpty(), qPrintable(error));
        QCOMPARE(manager.checkpoints().size(), 1);
        QCOMPARE(manager.checkpoints().first().label, u"first"_s);

        writeFile(m_workspace->path() + u"/a.txt"_s, u"changed"_s);
        const QString second = manager.createCheckpoint(u"second"_s, &error);
        QVERIFY2(!second.isEmpty(), qPrintable(error));
        QVERIFY(second != first);

        const QList<CheckpointInfo> checkpoints = manager.checkpoints();
        QCOMPARE(checkpoints.size(), 2);
        // Newest first.
        QCOMPARE(checkpoints.at(0).label, u"second"_s);
        QCOMPARE(checkpoints.at(1).label, u"first"_s);
        QCOMPARE(checkpoints.at(0).shortId.size(), 8);
    }

    void emptyCheckpointIsStillARestorePoint()
    {
        if (!m_hasGit) {
            QSKIP("git is not available");
        }
        writeFile(m_workspace->path() + u"/a.txt"_s, u"same"_s);

        CheckpointManager manager;
        manager.setWorkspace(m_workspace->path());
        QString error;
        const QString before = manager.createCheckpoint(u"before"_s, &error);
        QVERIFY2(!before.isEmpty(), qPrintable(error));
        // Nothing changed, but the user asked for a restore point.
        const QString again = manager.createCheckpoint(u"again"_s, &error);
        QVERIFY2(!again.isEmpty(), qPrintable(error));
        QCOMPARE(manager.checkpoints().size(), 2);
    }

    void restoreRevertsEditsAndRemovesAddedFiles()
    {
        if (!m_hasGit) {
            QSKIP("git is not available");
        }
        writeFile(m_workspace->path() + u"/a.txt"_s, u"original"_s);
        writeFile(m_workspace->path() + u"/nested/b.txt"_s, u"keep me"_s);

        CheckpointManager manager;
        manager.setWorkspace(m_workspace->path());
        QString error;
        const QString checkpoint = manager.createCheckpoint(u"baseline"_s, &error);
        QVERIFY2(!checkpoint.isEmpty(), qPrintable(error));

        // Mutate: modify an existing file, add a new one, delete another.
        writeFile(m_workspace->path() + u"/a.txt"_s, u"broken"_s);
        writeFile(m_workspace->path() + u"/added.txt"_s, u"should not survive"_s);
        QVERIFY(QFile::remove(m_workspace->path() + u"/nested/b.txt"_s));

        QCOMPARE(manager.changedFileCount(checkpoint), 3);

        QVERIFY2(manager.restoreCheckpoint(checkpoint, &error), qPrintable(error));
        QCOMPARE(readFile(m_workspace->path() + u"/a.txt"_s), u"original"_s);
        QCOMPARE(readFile(m_workspace->path() + u"/nested/b.txt"_s), u"keep me"_s);
        QVERIFY(!QFileInfo::exists(m_workspace->path() + u"/added.txt"_s));
        QCOMPARE(manager.changedFileCount(checkpoint), 0);
    }

    void diffReportsChangesAgainstTheSnapshot()
    {
        if (!m_hasGit) {
            QSKIP("git is not available");
        }
        writeFile(m_workspace->path() + u"/a.txt"_s, u"line one\nline two\n"_s);

        CheckpointManager manager;
        manager.setWorkspace(m_workspace->path());
        QString error;
        const QString checkpoint = manager.createCheckpoint(u"baseline"_s, &error);
        QVERIFY2(!checkpoint.isEmpty(), qPrintable(error));
        QVERIFY(manager.diffAgainstCheckpoint(checkpoint).isEmpty());

        writeFile(m_workspace->path() + u"/a.txt"_s, u"line one\nline changed\n"_s);
        const QString diff = manager.diffAgainstCheckpoint(checkpoint);
        QVERIFY(diff.contains(u"-line two"_s));
        QVERIFY(diff.contains(u"+line changed"_s));

        // Long diffs are truncated rather than dumped into the transcript.
        QString huge;
        for (int i = 0; i < 5000; ++i) {
            huge += QStringLiteral("line %1\n").arg(i);
        }
        writeFile(m_workspace->path() + u"/big.txt"_s, huge);
        const QString truncated = manager.diffAgainstCheckpoint(checkpoint, 100);
        QVERIFY(truncated.contains(u"truncated"_s));
    }

    void retentionKeepsTheNewestSnapshots()
    {
        if (!m_hasGit) {
            QSKIP("git is not available");
        }
        CheckpointManager manager;
        manager.setWorkspace(m_workspace->path());
        manager.setRetention(3);

        QString error;
        for (int i = 0; i < 6; ++i) {
            writeFile(m_workspace->path() + u"/f.txt"_s, QStringLiteral("rev %1").arg(i));
            QVERIFY2(!manager.createCheckpoint(QStringLiteral("rev %1").arg(i), &error).isEmpty(), qPrintable(error));
        }

        const QList<CheckpointInfo> checkpoints = manager.checkpoints();
        QVERIFY(checkpoints.size() <= 4);
        QVERIFY(checkpoints.size() >= 2);
        // The newest snapshot is still reachable after pruning.
        QCOMPARE(checkpoints.first().label, u"rev 5"_s);
    }

    void restoringAnUnknownCheckpointFails()
    {
        if (!m_hasGit) {
            QSKIP("git is not available");
        }
        writeFile(m_workspace->path() + u"/a.txt"_s, u"x"_s);
        CheckpointManager manager;
        manager.setWorkspace(m_workspace->path());
        QString error;
        QVERIFY(manager.createCheckpoint(u"one"_s, &error).isEmpty() == false);
        QVERIFY(!manager.restoreCheckpoint(u"deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"_s, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!manager.restoreCheckpoint(QString(), &error));
        // The workspace must be untouched by the failed restore.
        QCOMPARE(readFile(m_workspace->path() + u"/a.txt"_s), u"x"_s);
    }

    void theProjectsOwnGitHistoryIsNeverTouched()
    {
        if (!m_hasGit) {
            QSKIP("git is not available");
        }
        QProcess init;
        init.setWorkingDirectory(m_workspace->path());
        init.start(u"git"_s, QStringList{u"init"_s, u"--quiet"_s});
        QVERIFY(init.waitForFinished(10000));

        writeFile(m_workspace->path() + u"/tracked.txt"_s, u"real content"_s);
        QProcess add;
        add.setWorkingDirectory(m_workspace->path());
        add.start(u"git"_s, QStringList{u"add"_s, u"-A"_s});
        QVERIFY(add.waitForFinished(10000));
        QProcess commit;
        commit.setWorkingDirectory(m_workspace->path());
        commit.start(u"git"_s, QStringList{u"-c"_s,
                                          u"user.name=t"_s,
                                          u"-c"_s,
                                          u"user.email=t@t"_s,
                                          u"commit"_s,
                                          u"--quiet"_s,
                                          u"-m"_s,
                                          u"real commit"_s});
        QVERIFY(commit.waitForFinished(10000));

        CheckpointManager manager;
        manager.setWorkspace(m_workspace->path());
        QString error;
        QVERIFY2(!manager.createCheckpoint(u"shadow"_s, &error).isEmpty(), qPrintable(error));

        // The snapshot repository lives outside the workspace, and the
        // project's own log still shows exactly the one real commit.
        QVERIFY(!QFileInfo::exists(m_workspace->path() + u"/.kateai/checkpoints"_s));
        QProcess log;
        log.setWorkingDirectory(m_workspace->path());
        log.start(u"git"_s, QStringList{u"log"_s, u"--oneline"_s});
        QVERIFY(log.waitForFinished(10000));
        const QString history = QString::fromUtf8(log.readAllStandardOutput());
        QCOMPARE(history.count(u"real commit"_s), 1);
    }

private:
    std::unique_ptr<QTemporaryDir> m_workspace;
    bool m_hasGit = false;
};

QTEST_GUILESS_MAIN(TestCheckpoints)
#include "test_checkpoints.moc"