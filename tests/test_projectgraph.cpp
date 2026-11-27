/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "graph/projectgraph.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
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
    void sqliteRoundTripThroughASymlinkedWorkspace()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // main.cpp is deliberately NOT created: the node names a file that does
        // not exist, so QFileInfo::canonicalFilePath() is empty for it.

        // Reach the workspace through a symlink, the way macOS reaches /var
        // through /private/var: canonicalisation changes the spelling.
        QTemporaryDir linkHolder;
        QVERIFY(linkHolder.isValid());
        const QString link = linkHolder.filePath(u"link"_s);
        QVERIFY(QFile::link(dir.path(), link));

        QDir().mkpath(link + u"/nested");
        const QString path = link + u"/nested/graph.SQLITE3"_s;

        ProjectGraph graph;
        graph.setWorkspacePath(link);
        GraphNode root;
        root.id = u"root"_s;
        root.name = u"root"_s;
        root.type = u"directory"_s;
        root.path = link;
        root.isReadOnly = false;
        root.lastModified = 123;
        graph.addNode(root);
        GraphNode file;
        file.id = u"file"_s;
        file.name = u"main.cpp"_s;
        file.type = u"file"_s;
        file.path = link + u"/main.cpp"_s;
        file.isReadOnly = true;
        file.lastModified = 456;
        graph.addNode(file);
        graph.addEdge({u"root"_s, u"file"_s, u"contains"_s, {}, true});

        QVERIFY(graph.saveToFile(path));

        ProjectGraph restored;
        restored.setWorkspacePath(link);
        QVERIFY2(restored.loadFromDatabase(path), "round trip through a symlinked workspace");
    }

    void sqliteStillRejectsNodesOutsideTheWorkspace()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/main.cpp"_s, "int main() {}\n");
        const QString path = dir.filePath(u"nested/graph.SQLITE3"_s);

        ProjectGraph graph;
        graph.setWorkspacePath(dir.path());
        GraphNode root;
        root.id = u"root"_s;
        root.name = u"root"_s;
        root.type = u"directory"_s;
        root.path = dir.path();
        graph.addNode(root);
        GraphNode escaped;
        escaped.id = u"escaped"_s;
        escaped.name = u"passwd"_s;
        escaped.type = u"file"_s;
        escaped.path = u"/etc/passwd"_s;
        graph.addNode(escaped);

        QVERIFY(graph.saveToFile(path));

        ProjectGraph restored;
        restored.setWorkspacePath(dir.path());
        // A node outside the workspace must still fail the load.
        QVERIFY(!restored.loadFromDatabase(path));
    }

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

    void dependentsWalkTheReverseDirection()
    {
        ProjectGraph graph;
        GraphNode a; a.id = u"a"_s; a.type = u"file"_s;
        GraphNode b; b.id = u"b"_s; b.type = u"file"_s;
        GraphNode c; c.id = u"c"_s; c.type = u"file"_s;
        graph.addNode(a);
        graph.addNode(b);
        graph.addNode(c);
        graph.addEdge({u"a"_s, u"b"_s, u"imports"_s, {}, true});
        graph.addEdge({u"b"_s, u"c"_s, u"imports"_s, {}, true});
        const QSet<QString> dependents = graph.getTransitiveDependents(u"c"_s);
        QVERIFY(dependents.contains(u"b"_s));
        QVERIFY(dependents.contains(u"a"_s));
        QVERIFY(!dependents.contains(u"c"_s));
    }

    void removingOneRelationshipKeepsTheOther()
    {
        ProjectGraph graph;
        GraphNode a; a.id = u"a"_s; a.type = u"file"_s;
        GraphNode b; b.id = u"b"_s; b.type = u"file"_s;
        graph.addNode(a);
        graph.addNode(b);
        graph.addEdge({u"a"_s, u"b"_s, u"imports"_s, {}, true});
        graph.addEdge({u"a"_s, u"b"_s, u"references"_s, {}, true});
        graph.removeEdge(u"a"_s, u"b"_s);
        QCOMPARE(graph.getEdgesFrom(u"a"_s).size(), 1);
        graph.removeEdge(u"a"_s, u"b"_s);
        QCOMPARE(graph.getEdgesFrom(u"a"_s).size(), 0);
    }

    void importPathIsANodeIdSequence()
    {
        ProjectGraph graph;
        GraphNode a; a.id = u"a"_s; a.type = u"file"_s;
        GraphNode b; b.id = u"b"_s; b.type = u"file"_s;
        GraphNode c; c.id = u"c"_s; c.type = u"file"_s;
        graph.addNode(a);
        graph.addNode(b);
        graph.addNode(c);
        graph.addEdge({u"a"_s, u"b"_s, u"imports"_s, {}, true});
        graph.addEdge({u"b"_s, u"c"_s, u"imports"_s, {}, true});
        QCOMPARE(graph.findImportPaths(u"a"_s, u"c"_s), (QList<QString>{u"a"_s, u"b"_s, u"c"_s}));
    }

    void testSymbolsResolveThroughASymlinkedWorkspacePath()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/a.cpp"_s, "int compute() {\n    return 1;\n}\n");

        QTemporaryDir links;
        QVERIFY(links.isValid());
        const QString link = links.filePath(u"workspace"_s);
        if (!QFile::link(dir.path(), link) || !QFileInfo(link).isSymLink()) {
            QSKIP("symlinks are not available on this filesystem");
        }

        ProjectGraph graph;
        graph.generateGraph(dir.path());
        // macOS names the same temporary directory /var/... or /private/var/...;
        // lookups must not depend on which spelling the caller has.
        const QList<GraphSymbol> symbols = graph.symbolsInFile(link + u"/a.cpp"_s);
        QVERIFY2(!symbols.isEmpty(), "the graph did not find the file through the symlinked path");
        QCOMPARE(symbols.first().name, u"compute"_s);
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

    void testBinaryFilesAreSkippedAndLargeFilesAreIndexed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A file well over the old 64 KB cap, with real definitions in it.
        constexpr int kLines = 4000;
        QByteArray big;
        for (int i = 0; i < kLines; ++i) {
            big += "int function_" + QByteArray::number(i) + "() { return " + QByteArray::number(i) + "; }\n";
        }
        QVERIFY(big.size() > 64 * 1024);
        writeFile(dir.path() + u"/big.cpp"_s, big);
        // Known binary extension.
        writeFile(dir.path() + u"/pic.png"_s, QByteArrayLiteral("\x89PNG\r\n\x1a\n") + QByteArray(32, '\0'));
        // Text extension, but NUL bytes in the head.
        writeFile(dir.path() + u"/sneaky.dat"_s, QByteArrayLiteral("text\x00\x00\x00binary"));

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        // The big file is part of the project: dropping it left the graph
        // without the very files an agent asks about most.
        const GraphNode *node = graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/big.cpp"_s));
        QVERIFY2(node, "a file over 64 KB is missing from the graph");
        QCOMPARE(node->type, u"file"_s);
        // Its own metrics, not those of the first page of it.
        QCOMPARE(node->linesOfCode, kLines);
        QCOMPARE(node->complexity, kLines);
        // The binaries are still left out: the root and the one text file.
        QCOMPARE(graph.getNodeCount(), 2);
        QVERIFY(!graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/pic.png"_s)));
        QVERIFY(!graph.getNode(ProjectGraph::generateNodeId(dir.path() + u"/sneaky.dat"_s)));
    }

    void testTheGraphIsSerialisedTheSameWayEveryTime()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // Enough dependencies that the order of an unordered set cannot be the
        // sorted order by accident.
        QString source;
        for (int i = 0; i < 12; ++i) {
            const QString name = QStringLiteral("header_%1.h").arg(i);
            writeFile(dir.path() + u'/' + name, QByteArrayLiteral("int function();\n"));
            source += QStringLiteral("#include \"%1\"\n").arg(name);
        }
        writeFile(dir.path() + u"/a.cpp"_s, source.toUtf8());

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        // The cache is written into the project and compared and committed, so
        // the same graph has to serialise to the same bytes every time. The
        // dependency sets are unordered, and their order shifted with the hash
        // seed of the process.
        const QByteArray first = QJsonDocument(graph.toJson()).toJson();
        const QByteArray second = QJsonDocument(graph.toJson()).toJson();
        QCOMPARE(first, second);

        // And the lists themselves are ordered, not left to the hash seed.
        QJsonArray dependencies;
        const QJsonArray nodes = graph.toJson().value(u"nodes"_s).toArray();
        for (const QJsonValue &value : nodes) {
            const QJsonObject node = value.toObject();
            if (node.value(u"name"_s).toString() == QLatin1String("a.cpp")) {
                dependencies = node.value(u"dependencies"_s).toArray();
            }
        }
        QCOMPARE(dependencies.size(), 12);
        QStringList ids;
        for (const QJsonValue &value : dependencies) {
            ids.append(value.toString());
        }
        QStringList sorted = ids;
        sorted.sort();
        QCOMPARE(ids, sorted);
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

    void testNodeBudgetCapsAtOneThousand()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 0; i < 1050; ++i) {
            writeFile(dir.path() + u"/f%1.txt"_s.arg(i), "x\n");
        }

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        // The cap counts the root node too, so the graph stops at 1000.
        QCOMPARE(graph.getNodeCount(), 1000);
    }

    // The scan must analyse each file while its text is in hand and then cut
    // the text back to a bounded preview: holding every file until the
    // dependency pass made the graph's peak memory grow with the whole
    // workspace. A definition beyond that preview therefore has to survive in
    // the symbol index even though the node no longer keeps the full text.
    void testLargeFileSymbolsSurviveContentRelease()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QByteArray content;
        for (int i = 0; i < 3000; ++i) {
            content += "// filler line that makes the file larger than a preview\n";
        }
        QVERIFY(content.size() > 64 * 1024);
        content += "int lateFunction() { return 1; }\n";
        writeFile(dir.path() + u"/late.cpp"_s, content);

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        const QString path = dir.path() + u"/late.cpp"_s;
        const GraphNode *node = graph.findNodeByPath(path);
        QVERIFY(node);
        QVERIFY2(node->content.size() < content.size(),
                 "the scan retained the whole file text");
        QVERIFY2(node->content.size() <= 16 * 1024,
                 "the scan retained more than the content preview");
        const QList<GraphSymbol> symbols = graph.symbolsInFile(path);
        const auto found = std::find_if(symbols.cbegin(), symbols.cend(),
                                        [](const GraphSymbol &symbol) {
                                            return symbol.name == u"lateFunction"_s;
                                        });
        QVERIFY2(found != symbols.cend(), "a definition past the preview was not indexed");
        QVERIFY(found->line > 3000);
    }

    // The incremental path used to cut a new node's content to the preview
    // before resolving its imports, so an include past the first 16 KiB was
    // invisible until the next full scan.
    void testNewFileImportsBeyondThePreviewAreIndexed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/target.h"_s, "int target();\n");

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        QByteArray content;
        for (int i = 0; i < 1200; ++i) {
            content += "// filler line that pushes the include past the preview\n";
        }
        QVERIFY(content.size() > 16 * 1024);
        content += "#include \"target.h\"\nint created() { return 1; }\n";
        const QString newPath = dir.path() + u"/created.cpp"_s;
        graph.updateGraph(newPath, QString::fromUtf8(content));

        const GraphNode *created = graph.getNode(ProjectGraph::generateNodeId(newPath));
        QVERIFY(created);
        const QString targetId =
            ProjectGraph::generateNodeId(dir.path() + u"/target.h"_s, u"file"_s);
        QVERIFY2(created->dependencies.contains(targetId),
                 "an include past the content preview was not resolved incrementally");
        QVERIFY2(created->content.size() <= 16 * 1024,
                 "the incremental path kept more than the content preview");
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

    void testImportsResolveToFileNodeIds()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u"src"_s));
        writeFile(dir.path() + u"/src/a.cpp"_s, QByteArrayLiteral("#include \"b.h\"\nint a() { return 1; }\n"));
        writeFile(dir.path() + u"/src/b.h"_s, QByteArrayLiteral("int b();\n"));

        ProjectGraph graph;
        graph.generateGraph(dir.path());
        const QString aId = ProjectGraph::generateNodeId(dir.path() + u"/src/a.cpp"_s);
        const QString bId = ProjectGraph::generateNodeId(dir.path() + u"/src/b.h"_s);
        GraphNode *a = graph.getNode(aId);
        QVERIFY(a);
        QVERIFY2(a->dependencies.contains(bId), "the include did not resolve to the header node");
        QVERIFY2(graph.getNode(bId)->dependents.contains(aId), "the header does not know its dependent");
        const QString summary = graph.contextSummary(10, 10, 1);
        QVERIFY2(summary.contains(u"src/a.cpp -> src/b.h (imports)"_s), qPrintable(summary));
    }

    void testContextSummaryUsesReadablePathsAndSymbols()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u"src"_s));
        writeFile(dir.path() + u"/src/main.cpp"_s,
                  "class Widget { };\n\nint main() { return 0; }\n");

        ProjectGraph graph;
        graph.generateGraph(dir.path());

        const QString summary = graph.contextSummary(10, 10, 1);
        QVERIFY2(summary.contains(u"src/main.cpp"_s), qPrintable(summary));
        QVERIFY2(summary.contains(u"Widget"_s), qPrintable(summary));
        QVERIFY2(summary.contains(u"main"_s), qPrintable(summary));
        // A compact description must not leak the internal hash id when a
        // human-readable path is available.
        const QString id = ProjectGraph::generateNodeId(dir.path() + u"/src/main.cpp"_s);
        QVERIFY2(!summary.contains(id), qPrintable(summary));
        QVERIFY(graph.findNodeByPath(u"src/main.cpp"_s));
    }

    void sqliteRoundTripPreservesTheRelationalGraph()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProjectGraph graph;
        graph.setWorkspacePath(dir.path());
        GraphNode root;
        root.id = u"root"_s;
        root.name = u"root"_s;
        root.type = u"directory"_s;
        root.path = dir.path();
        root.isReadOnly = false;
        root.lastModified = 123;
        graph.addNode(root);
        GraphNode file;
        file.id = u"file"_s;
        file.name = u"main.cpp"_s;
        file.type = u"file"_s;
        file.path = dir.filePath(u"main.cpp"_s);
        file.content = u"int main() {}"_s;
        file.complexity = 2;
        file.linesOfCode = 3;
        file.isReadOnly = true;
        file.lastModified = 456;
        file.metadata.insert(u"language"_s, u"cpp"_s);
        graph.addNode(file);
        graph.addEdge({u"root"_s, u"file"_s, u"contains"_s, {}, true});
        graph.addEdge({u"file"_s, u"root"_s, u"references"_s, u"root"_s, false});
        const QJsonObject expected = graph.toJson();
        const QString path = dir.filePath(u"nested/graph.SQLITE3"_s);
        QVERIFY(graph.saveToFile(path));
        {
            const QString connection = u"graph-sqlite-inspect"_s;
            {
                QSqlDatabase database = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
                database.setDatabaseName(path);
                QVERIFY(database.open());
                QSqlQuery nodes(database);
                QVERIFY(nodes.exec(u"SELECT COUNT(*) FROM kateai_graph_nodes"_s));
                QVERIFY(nodes.next());
                QCOMPARE(nodes.value(0).toInt(), 2);
                QSqlQuery edges(database);
                QVERIFY(edges.exec(u"SELECT COUNT(*) FROM kateai_graph_edges"_s));
                QVERIFY(edges.next());
                QCOMPARE(edges.value(0).toInt(), 2);
                database.close();
            }
            QSqlDatabase::removeDatabase(connection);
        }
        ProjectGraph restored;
        restored.setWorkspacePath(dir.path());
        QVERIFY(restored.loadFromDatabase(path));
        QCOMPARE(restored.toJson(), expected);
        QVERIFY(restored.getNode(u"file"_s)->dependencies.contains(u"root"_s));
        QVERIFY(restored.getNode(u"root"_s)->dependents.contains(u"file"_s));
    }

    void sqliteRejectsIncompatibleCorruptAndLockedCaches()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProjectGraph graph;
        graph.setWorkspacePath(dir.path());
        graph.generateGraph(dir.path());
        const QString path = dir.filePath(u"graph.db"_s);
        QVERIFY(graph.saveToDb(path));
        const QJsonObject expected = graph.toJson();
        {
            const QString connection = u"graph-sqlite-version"_s;
            {
                QSqlDatabase database = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
                database.setDatabaseName(path);
                QVERIFY(database.open());
                QSqlQuery query(database);
                QVERIFY(query.exec(u"PRAGMA user_version = 99"_s));
                database.close();
            }
            QSqlDatabase::removeDatabase(connection);
        }
        QVERIFY(!graph.saveToDatabase(path));
        QCOMPARE(graph.toJson(), expected);
        ProjectGraph restored;
        restored.setWorkspacePath(dir.path());
        ProjectGraph::SqliteLoadError error = ProjectGraph::SqliteLoadError::None;
        QVERIFY(!restored.loadFromSqlite(path, &error));
        QCOMPARE(error, ProjectGraph::SqliteLoadError::InvalidSchema);
        QVERIFY(restored.getNodes().isEmpty());

        const QString corruptPath = dir.filePath(u"corrupt.sqlite"_s);
        const QByteArray corrupt = QByteArrayLiteral("not a sqlite database\n");
        writeFile(corruptPath, corrupt);
        QVERIFY(!graph.saveToSqlite(corruptPath));
        QFile corruptFile(corruptPath);
        QVERIFY(corruptFile.open(QIODevice::ReadOnly));
        QCOMPARE(corruptFile.readAll(), corrupt);
        QVERIFY(!restored.loadFromSqlite(corruptPath, &error));
        QCOMPARE(error, ProjectGraph::SqliteLoadError::Corrupt);

        const QString lockedPath = dir.filePath(u"locked.sqlite"_s);
        QVERIFY(graph.saveToSqlite(lockedPath));
        const QString connection = u"graph-sqlite-lock"_s;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
            database.setDatabaseName(lockedPath);
            QVERIFY(database.open());
            QSqlQuery lock(database);
            QVERIFY(lock.exec(u"BEGIN EXCLUSIVE"_s));
            QVERIFY(!restored.loadFromSqlite(lockedPath, &error));
            QCOMPARE(error, ProjectGraph::SqliteLoadError::Io);
            database.rollback();
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void jsonValidationIsExactAndDoesNotMutateOnFailure()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QTemporaryDir other;
        QVERIFY(other.isValid());
        ProjectGraph graph;
        graph.setWorkspacePath(dir.path());
        graph.generateGraph(dir.path());
        const QJsonObject expected = graph.toJson();
        QJsonObject newer = expected;
        newer.insert(u"schemaVersion"_s, 99);
        QVERIFY(!graph.fromJson(newer));
        QCOMPARE(graph.toJson(), expected);

        QJsonObject foreign = expected;
        foreign.insert(u"workspacePath"_s, other.path());
        QVERIFY(!graph.fromJson(foreign));
        QCOMPARE(graph.toJson(), expected);

        QJsonArray nodes = expected.value(u"nodes"_s).toArray();
        QJsonObject outside = nodes.first().toObject();
        outside.insert(u"id"_s, u"outside"_s);
        outside.insert(u"path"_s, other.filePath(u"secret.txt"_s));
        nodes.append(outside);
        QJsonObject escaped = expected;
        escaped.insert(u"nodes"_s, nodes);
        QVERIFY(!graph.fromJson(escaped));
        QCOMPARE(graph.toJson(), expected);

        QJsonObject dangling = expected;
        QJsonArray edges = expected.value(u"edges"_s).toArray();
        edges.append(QJsonObject{{u"sourceId"_s, u"missing"_s},
                                 {u"targetId"_s, u"missing"_s},
                                 {u"relationship"_s, u"imports"_s}});
        dangling.insert(u"edges"_s, edges);
        QVERIFY(!graph.fromJson(dangling));
        QCOMPARE(graph.toJson(), expected);
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

    void edgeIndexesStayConsistentAcrossMutationsAndSqliteReload()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/src/a.h"_s, "int a();\n");
        writeFile(dir.path() + u"/src/b.cpp"_s, "#include \"a.h\"\nint b() { return a(); }\n");

        ProjectGraph graph;
        graph.generateGraph(dir.path());
        const QString aId = ProjectGraph::generateNodeId(dir.path() + u"/src/a.h"_s);
        const QString bId = ProjectGraph::generateNodeId(dir.path() + u"/src/b.cpp"_s);

        QVERIFY(!graph.getEdgesFrom(bId).isEmpty());
        QVERIFY(!graph.getEdgesTo(aId).isEmpty());
        QCOMPARE(graph.getFilesInDirectory(dir.path() + u"/src"_s).size(), 2);

        const QString dbPath = dir.path() + u"/graph.sqlite"_s;
        QVERIFY(graph.saveToSqlite(dbPath));

        ProjectGraph reloaded;
        QVERIFY(reloaded.loadFromSqlite(dbPath));
        QCOMPARE(reloaded.getEdgesFrom(bId).size(), graph.getEdgesFrom(bId).size());
        QCOMPARE(reloaded.getEdgesTo(aId).size(), graph.getEdgesTo(aId).size());
        QCOMPARE(reloaded.getFilesInDirectory(dir.path() + u"/src"_s).size(), 2);

        reloaded.removeEdge(bId, aId);
        for (const GraphEdge *edge : reloaded.getEdgesFrom(bId)) {
            QVERIFY(edge->targetId != aId);
        }
        reloaded.removeNode(aId);
        QVERIFY(reloaded.getNode(aId) == nullptr);
        QVERIFY(reloaded.getEdgesTo(aId).isEmpty());
        QCOMPARE(reloaded.getFilesInDirectory(dir.path() + u"/src"_s).size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestProjectGraph)
#include "test_projectgraph.moc"
