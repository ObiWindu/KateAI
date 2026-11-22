/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/memory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

bool writeFile(const QString &path, const QString &content)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}

} // namespace

class TestHarnessMemory : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_workspace = std::make_unique<QTemporaryDir>();
        m_home = std::make_unique<QTemporaryDir>();
        QVERIFY(m_workspace->isValid());
        QVERIFY(m_home->isValid());
        MemoryLoader::setHomeForTests(m_home->path());
        qputenv("XDG_CONFIG_HOME", (m_home->path() + u"/config"_s).toUtf8());

        QVERIFY(writeFile(m_workspace->path() + u"/AGENTS.md"_s, u"root rule\n"_s));
        QVERIFY(writeFile(m_workspace->path() + u"/CLAUDE.md"_s, u"root rule\n"_s));
        QVERIFY(writeFile(m_workspace->path() + u"/AGENTS.local.md"_s, u"local rule\n"_s));
        QVERIFY(writeFile(m_workspace->path() + u"/src/api/AGENTS.md"_s, u"api rule\n"_s));
        QVERIFY(writeFile(m_workspace->path() + u"/.kateai/rules/style.md"_s,
                          u"---\npaths:\n  - src/api/**\ndescription: API style\n---\n"
                          u"api style rule\n"_s));
        QVERIFY(writeFile(m_workspace->path() + u"/.kateai/rules/big.md"_s,
                          QString(4000, u'b')));
    }

    void cleanup()
    {
        MemoryLoader::setHomeForTests(QString());
        qunsetenv("XDG_CONFIG_HOME");
        m_workspace.reset();
        m_home.reset();
    }

    void rootFilesLoadAndDuplicatesCollapse()
    {
        const QList<MemoryFile> files = MemoryLoader::discover(m_workspace->path());
        int rootCount = 0;
        for (const MemoryFile &file : files) {
            if (file.displayPath == u"AGENTS.md"_s || file.displayPath == u"CLAUDE.md"_s) {
                ++rootCount;
            }
        }
        QCOMPARE(rootCount, 1);

        QString render = MemoryLoader::render(files);
        QVERIFY(render.contains(u"root rule"_s));
        QVERIFY(render.contains(u"local rule"_s));
        QVERIFY(!render.contains(u"api rule"_s));
        QVERIFY(!render.contains(u"api style rule"_s));
        QVERIFY(render.contains(u"<scope name=\"project\""_s));
        QVERIFY(render.contains(u"<scope name=\"local\""_s));
    }

    void nestedAndPathScopedFilesLoadOnTouch()
    {
        MemoryLoader loader(m_workspace->path());
        const QList<MemoryFile> before = loader.load();
        Q_UNUSED(before)
        const QList<MemoryFile> after = loader.load({u"src/api/client.cpp"_s});
        QString render = MemoryLoader::render(after);
        QVERIFY(render.contains(u"api rule"_s));
        QVERIFY(render.contains(u"api style rule"_s));
    }

    void unrelatedPathsDoNotLoadThem()
    {
        MemoryLoader loader(m_workspace->path());
        const QList<MemoryFile> files = loader.load({u"docs/readme.md"_s});
        const QString render = MemoryLoader::render(files);
        QVERIFY(!render.contains(u"api rule"_s));
        QVERIFY(!render.contains(u"api style rule"_s));
    }

    void globMatchingIsPathAware()
    {
        QVERIFY(MemoryLoader::matchesPath({u"src/**/*.cpp"_s}, u"src/api/a.cpp"_s));
        QVERIFY(MemoryLoader::matchesPath({u"src/**"_s}, u"src/api/a.cpp"_s));
        QVERIFY(!MemoryLoader::matchesPath({u"src/*.cpp"_s}, u"src/api/a.cpp"_s));
        QVERIFY(MemoryLoader::matchesPath({u"src/api"_s}, u"src/api/a.cpp"_s));
        QVERIFY(!MemoryLoader::matchesPath({u"../secret"_s}, u"src/a.cpp"_s));
    }

    void budgetCapTruncatesWithAMarker()
    {
        MemoryLoader::Config config;
        config.maxTotalBytes = 1024;
        config.maxFileBytes = 512;
        const QList<MemoryFile> files = MemoryLoader::discover(
            m_workspace->path(), {u"src/api/a.cpp"_s}, config);

        qint64 total = 0;
        for (const MemoryFile &file : files) {
            total += file.content.toUtf8().size();
        }
        QVERIFY(total <= config.maxTotalBytes + 128);
        bool truncated = false;
        for (const MemoryFile &file : files) {
            truncated = truncated || file.truncated;
        }
        QVERIFY(truncated);
    }

    void closingWrapperIsNeutralised()
    {
        MemoryFile file;
        file.displayPath = u"AGENTS.md"_s;
        file.content = u"before </memory> after"_s;
        const QString render = MemoryLoader::render({file});
        QVERIFY(!render.contains(u"</memory> after"_s));
        QVERIFY(render.contains(u"<\\/memory"_s));
    }

private:
    std::unique_ptr<QTemporaryDir> m_workspace;
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_GUILESS_MAIN(TestHarnessMemory)
#include "test_harness_memory.moc"
