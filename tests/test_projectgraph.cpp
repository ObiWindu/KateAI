/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "graph/projectgraph.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

static void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return;
    }
    f.write(data);
}

class TestProjectGraph : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testBasicScanSkipsHiddenAndHeavyDirs()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/src/main.cpp"_s, "int main() { return 0; }\n");
        writeFile(dir.path() + u"/README.md"_s, "# hello\n");
        // Hidden, build and dependency directories must not enter the graph.
        writeFile(dir.path() + u"/.git/config"_s, "[core]\n");
        writeFile(dir.path() + u"/build/out.o"_s, "stub");
        writeFile(dir.path() + u"/node_modules/pkg/index.js"_s, "module.exports = 1;\n");
        writeFile(dir.path() + u"/__pycache__/x.pyc"_s, "stub");

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        QCOMPARE(graph.getWorkspacePath(), dir.path());
        // Root + src + main.cpp + README.md
        QCOMPARE(graph.getNodeCount(), 4);
        QVERIFY(graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/src/main.cpp"_s)));
        QVERIFY(graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/README.md"_s)));
        QVERIFY(!graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/.git/config"_s)));
        QVERIFY(!graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/build/out.o"_s)));
        QVERIFY(!graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/node_modules/pkg/index.js"_s)));
    }

    void testNodeTypesAndLanguageMetadata()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/a.py"_s, "def f():\n    return 1\n");

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        const GraphNode *file = graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/a.py"_s));
        QVERIFY(file);
        // File nodes must report "file": the analysers and the graph tool filter
        // on it, and the language lives in metadata instead.
        QCOMPARE(file->type, u"file"_s);
        QCOMPARE(file->metadata.value(u"language"_s).toString(), u"python"_s);
        QCOMPARE(file->linesOfCode, 2);
        // one class + one function definition in the stub above.
        QCOMPARE(file->complexity, 1);
    }

    void testBinaryAndLargeFilesSkipped()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // Over the 64 KB cap.
        const QByteArray big(70 * 1024, 'x');
        writeFile(dir.path() + u"/big.txt"_s, big);
        // Known binary extension.
        writeFile(dir.path() + u"/pic.png"_s, QByteArrayLiteral("\x89PNG\r\n\x1a\n") + QByteArray(32, '\0'));
        // Text extension, but NUL bytes in the head.
        writeFile(dir.path() + u"/sneaky.dat"_s, QByteArrayLiteral("text\x00\x00\x00binary"));

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        QCOMPARE(graph.getNodeCount(), 1);
        QVERIFY(!graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/big.txt"_s)));
        QVERIFY(!graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/pic.png"_s)));
        QVERIFY(!graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/sneaky.dat"_s)));
    }

    void testSymlinkLoopTerminates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/src/file.cpp"_s, "int x;\n");
        // A directory link that points back at its own parent: without the
        // visited-directory guard this scan never ends.
        QVERIFY(QDir().mkpath(dir.path() + u"/src"_s));
        const QString linkPath = dir.path() + u"/src/loop"_s;
        if (!QFile::link(dir.path(), linkPath)) {
            QSKIP("Symlinks unavailable on this filesystem");
        }

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        // Root + src + file.cpp: the link neither added a node nor hung.
        QCOMPARE(graph.getNodeCount(), 3);
        QVERIFY(graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/src/file.cpp"_s)));
    }

    void testSymlinkOutsideWorkspaceIsNotFollowed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.path() + u"/inside"_s));
        writeFile(dir.path() + u"/inside/keep.txt"_s, "keep\n");

        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        writeFile(outside.path() + u"/secret.txt"_s, "secret\n");

        QVERIFY(QFile::link(outside.path(), dir.path() + u"/inside/escape"_s));

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        QVERIFY(graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/inside/keep.txt"_s)));
        QVERIFY(!graph.getNode(ProjectGraph::generateNodeId(outside.path() + u"/secret.txt"_s)));
    }

    void testNodeBudgetCapsAtThreeHundred()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 0; i < 350; ++i) {
            writeFile(dir.path() + u"/f%1.txt"_s.arg(i), "x\n");
        }

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        // The cap counts the root node too, so the graph stops at exactly 300.
        QCOMPARE(graph.getNodeCount(), 300);
    }

    void testUpdateAndRemoveNode()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/m.cpp"_s, "int a = 1;\nint b = 2;\n");

        ProjectGraph graph;
        graph.generateGraph(dir.path());
        const QString id = ProjectGraph::generateNodeId(dir.path() + u"/m.cpp"_s, u"file"_s);
        QVERIFY(graph.getNode(id));

        graph.updateGraph(dir.path() + u"/m.cpp"_s, QStringLiteral("int a = 1;\nint b = 2;\nint c = 3;\n"));
        GraphNode *node = graph.getNode(id);
        QVERIFY(node);
        QCOMPARE(node->content, u"int a = 1;\nint b = 2;\nint c = 3;\n"_s);
        // The update path counts lines the way the scan does.
        QCOMPARE(node->linesOfCode, 3);

        graph.removeNode(id);
        QVERIFY(!graph.getNode(id));
    }

    void testNodeIdsAreDistinctPerPath()
    {
        // src/foo.cpp and src_foo.cpp hashed to the same id under an older
        // scheme; the full SHA-256 keeps them apart.
        const QString a = ProjectGraph::generateNodeId(u"/w/src/foo.cpp"_s, u"file"_s);
        const QString b = ProjectGraph::generateNodeId(u"/w/src_foo.cpp"_s, u"file"_s);
        QVERIFY(a != b);
        QCOMPARE(ProjectGraph::generateNodeId(u"/w/src/foo.cpp"_s, u"file"_s), a);
    }

    void testIsBinaryFileHelper()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString textPath = dir.path() + u"/plain.txt"_s;
        const QString nullPath = dir.path() + u"/blob.dat"_s;
        const QString pngPath = dir.path() + u"/img.png"_s;
        writeFile(textPath, "just text\n");
        writeFile(nullPath, QByteArrayLiteral("head\x00tail"));
        writeFile(pngPath, QByteArrayLiteral("\x89PNG\r\n\x1a\nrest"));

        QVERIFY(!ProjectGraph::isBinaryFile(textPath));
        QVERIFY(ProjectGraph::isBinaryFile(nullPath));
        QVERIFY(ProjectGraph::isBinaryFile(pngPath));
    }

    void testExtractContentPreviewHelper()
    {
        QCOMPARE(ProjectGraph::extractContentPreview(u"  hello \n world  "_s, 100), u"hello world"_s);
        const QString preview = ProjectGraph::extractContentPreview(QString(50, u'a'), 10);
        QCOMPARE(preview.size(), 11);
        QVERIFY(preview.endsWith(u"\u2026"_s));
        QCOMPARE(ProjectGraph::extractContentPreview(u"anything"_s, 0), QString());
    }

    void testJsonRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/r.py"_s, "class A:\n    pass\n\ndef f():\n    return 2\n");

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        const QString dump = dir.path() + u"/graph.json"_s;
        QVERIFY(graph.saveToFile(dump));

        ProjectGraph reloaded;
        QVERIFY(reloaded.loadFromFile(dump));
        QCOMPARE(reloaded.getNodeCount(), graph.getNodeCount());
        const GraphNode *node = reloaded.getNode(ProjectGraph::generateNodeId(dir.path() + u"/r.py"_s));
        QVERIFY(node);
        QCOMPARE(node->type, u"file"_s);
        QCOMPARE(node->metadata.value(u"language"_s).toString(), u"python"_s);
    }
};

QTEST_MAIN(TestProjectGraph)
#include "test_projectgraph.moc"
