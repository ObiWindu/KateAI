#include "harness/worktree.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestWorktree : public QObject
{
    Q_OBJECT

private:
    static bool git(const QString &cwd, const QStringList &args)
    {
        QProcess process;
        process.setWorkingDirectory(cwd);
        process.start(u"git"_s, args);
        if (!process.waitForStarted(5000) || !process.waitForFinished(20000)) {
            return false;
        }
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    }

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

    bool makeRepository(const QString &path)
    {
        if (!git(path, {u"init"_s, u"--quiet"_s})) {
            return false;
        }
        git(path, {u"config"_s, u"user.email"_s, u"kateai@example.invalid"_s});
        git(path, {u"config"_s, u"user.name"_s, u"KateAI Test"_s});
        writeFile(path + u"/README.md"_s, u"initial\n"_s);
        return git(path, {u"add"_s, u"README.md"_s})
            && git(path, {u"commit"_s, u"-m"_s, u"initial"_s, u"--quiet"_s});
    }

private Q_SLOTS:
    void init()
    {
        m_repo = std::make_unique<QTemporaryDir>();
        QVERIFY(m_repo->isValid());
        m_cache = std::make_unique<QTemporaryDir>();
        QVERIFY(m_cache->isValid());
        if (!makeRepository(m_repo->path())) {
            QSKIP("git is unavailable; worktree isolation cannot be exercised");
        }
        SubagentWorktree::setCacheRootForTests(m_cache->path());
    }

    void cleanup()
    {
        SubagentWorktree::setCacheRootForTests(QString());
        m_repo.reset();
        m_cache.reset();
    }

    void nonRepositoryIsRejected()
    {
        QTemporaryDir plain;
        QVERIFY(plain.isValid());
        QString reason;
        QVERIFY(!SubagentWorktree::isAvailable(plain.path(), &reason));
        QVERIFY(!reason.isEmpty());
    }

    void isolatedChangesNeverTouchTheParentAndProduceAPatch()
    {
        const WorktreeResult worktree = SubagentWorktree::create(m_repo->path(), u"reviewer"_s);
        QVERIFY2(worktree.ok, qPrintable(worktree.error));
        QVERIFY(QFileInfo(worktree.path).isDir());
        QVERIFY(!worktree.baseCommit.isEmpty());

        writeFile(worktree.path + u"/README.md"_s, u"child change\n"_s);
        writeFile(worktree.path + u"/new.txt"_s, u"new file\n"_s);

        QCOMPARE(readFile(m_repo->path() + u"/README.md"_s), u"initial\n"_s);
        QVERIFY(!QFileInfo::exists(m_repo->path() + u"/new.txt"_s));

        QString error;
        const QString diff = SubagentWorktree::diff(worktree.path, worktree.baseCommit, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(diff.contains(u"child change"_s));
        QVERIFY(diff.contains(u"new.txt"_s));
        const QStringList files = SubagentWorktree::changedFiles(worktree.path,
                                                                 worktree.baseCommit, &error);
        QCOMPARE(files, QStringList({u"README.md"_s, u"new.txt"_s}));

        QVERIFY2(SubagentWorktree::apply(m_repo->path(), diff, &error), qPrintable(error));
        QCOMPARE(readFile(m_repo->path() + u"/README.md"_s), u"child change\n"_s);
        QCOMPARE(readFile(m_repo->path() + u"/new.txt"_s), u"new file\n"_s);

        QVERIFY2(SubagentWorktree::remove(m_repo->path(), worktree.path, &error),
                 qPrintable(error));
        QVERIFY(!QFileInfo::exists(worktree.path));
    }

    void aConflictingPatchIsRejectedBeforeWriting()
    {
        const WorktreeResult worktree = SubagentWorktree::create(m_repo->path(), u"conflict"_s);
        QVERIFY2(worktree.ok, qPrintable(worktree.error));
        writeFile(worktree.path + u"/README.md"_s, u"from the child\n"_s);
        QString error;
        const QString diff = SubagentWorktree::diff(worktree.path, worktree.baseCommit, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        writeFile(m_repo->path() + u"/README.md"_s, u"the user changed this meanwhile\n"_s);
        QVERIFY(!SubagentWorktree::apply(m_repo->path(), diff, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readFile(m_repo->path() + u"/README.md"_s),
                 u"the user changed this meanwhile\n"_s);
    }

    void cacheRootRefusesToDeleteArbitraryPaths()
    {
        QString error;
        QVERIFY(!SubagentWorktree::remove(m_repo->path(), m_repo->path() + u"/README.md"_s, &error));
        QVERIFY(QFileInfo::exists(m_repo->path() + u"/README.md"_s));
    }

private:
    std::unique_ptr<QTemporaryDir> m_repo;
    std::unique_ptr<QTemporaryDir> m_cache;
};

QTEST_GUILESS_MAIN(TestWorktree)
#include "test_worktree.moc"
