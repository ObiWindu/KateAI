/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "documentbridge.h"
#include "graph/projectgraph.h"
#include "lsp.h"
#include "modes.h"
#include "permissions.h"
#include "sandbox.h"
#include "tools.h"
#include "types.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return;
    }
    f.write(data);
}

// A stand-in for KateLspBridge: the same LspProvider interface the core uses,
// with canned answers so the tool layer can be tested without Kate's LSP
// plugin.
class StubLsp : public LspProvider
{
public:
    bool available = true;
    QList<LspDiagnostic> diags;
    QStringList messages;
    QStringList actions;

    bool isAvailable() const override
    {
        return available;
    }
    QString serverState(const QString &) const override
    {
        return available ? u"loaded"_s : u"none"_s;
    }
    QList<LspDiagnostic> diagnostics(const QString &path) const override
    {
        if (path.isEmpty()) {
            return diags;
        }
        QList<LspDiagnostic> out;
        for (const LspDiagnostic &d : diags) {
            if (d.path == path) {
                out.append(d);
            }
        }
        return out;
    }
    QStringList recentMessages(int maxCount) const override
    {
        if (maxCount <= 0) {
            return messages;
        }
        return messages.mid(qMax(0, messages.size() - maxCount));
    }
    QStringList codeActions(const QString &, int, int) const override
    {
        return actions;
    }
};

ToolCall makeCall(const QString &name, const QJsonObject &args)
{
    ToolCall call;
    call.id = u"call-1"_s;
    call.name = name;
    call.arguments = args;
    return call;
}

} // namespace

class TestLsp : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSymbolExtractionCpp();
    void testSymbolExtractionPython();
    void testEnclosingSymbol();
    void testStaleFileCount();
    void testLspToolsReportUnavailableWithoutProvider();
    void testLspDiagnosticsWithProvider();
    void testLspDiagnosticsSeverityFilter();
    void testLspSymbolsViaGraph();
    void testLspReferencesTextFallback();
    void testLspCodeActions();
    void testToolRegistration();
};

void TestLsp::testSymbolExtractionCpp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + u"/a.cpp"_s;
    writeFile(path,
              "class Foo {\n"
              "public:\n"
              "    void bar() {\n"
              "        int x = 1;\n"
              "    }\n"
              "};\n"
              "\n"
              "int main() {\n"
              "    Foo f;\n"
              "    return 0;\n"
              "}\n");

    ProjectGraph graph;
    graph.generateGraph(dir.path());

    const QList<GraphSymbol> symbols = graph.symbolsInFile(path);
    QVERIFY(symbols.size() >= 3);
    bool foundClass = false;
    bool foundBar = false;
    bool foundMain = false;
    for (const GraphSymbol &s : symbols) {
        if (s.name == u"Foo"_s && s.kind == u"class"_s) {
            foundClass = true;
            QCOMPARE(s.line, 1);
            QVERIFY(s.endLine >= 6); // class body ends at the closing brace
        }
        if (s.name == u"bar"_s && s.kind == u"function"_s) {
            foundBar = true;
            QCOMPARE(s.line, 3);
            QCOMPARE(s.endLine, 5);
        }
        if (s.name == u"main"_s && s.kind == u"function"_s) {
            foundMain = true;
            QCOMPARE(s.line, 8);
        }
    }
    QVERIFY(foundClass);
    QVERIFY(foundBar);
    QVERIFY(foundMain);
}

void TestLsp::testSymbolExtractionPython()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + u"/a.py"_s;
    writeFile(path,
              "def alpha():\n"
              "    return 1\n"
              "\n"
              "def beta():\n"
              "    x = 2\n"
              "    return x\n"
              "\n"
              "class Gamma:\n"
              "    pass\n");

    ProjectGraph graph;
    graph.generateGraph(dir.path());

    const QList<GraphSymbol> symbols = graph.symbolsInFile(path);
    QCOMPARE(symbols.size(), 3);
    for (const GraphSymbol &s : symbols) {
        if (s.name == u"alpha"_s) {
            QCOMPARE(s.kind, u"function"_s);
            QCOMPARE(s.line, 1);
            QCOMPARE(s.endLine, 2);
        }
        if (s.name == u"beta"_s) {
            QCOMPARE(s.line, 4);
            QCOMPARE(s.endLine, 6);
        }
        if (s.name == u"Gamma"_s) {
            QCOMPARE(s.kind, u"class"_s);
            QCOMPARE(s.line, 8);
        }
    }
}

void TestLsp::testEnclosingSymbol()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + u"/a.cpp"_s;
    writeFile(path,
              "void outer() {\n"
              "    int a = 1;\n"
              "    if (a) {\n"
              "        int b = 2;\n"
              "    }\n"
              "}\n"
              "\n"
              "void other() {\n"
              "}\n");

    ProjectGraph graph;
    graph.generateGraph(dir.path());

    // Line 4 is inside the if-block, which belongs to outer().
    const GraphSymbol sym = graph.enclosingSymbol(path, 4);
    QCOMPARE(sym.name, u"outer"_s);
    // Line 8 is inside other().
    QCOMPARE(graph.enclosingSymbol(path, 8).name, u"other"_s);
    // Line 7 is between the two functions: no range covers it, the nearest
    // preceding symbol is the fallback.
    QCOMPARE(graph.enclosingSymbol(path, 7).name, u"outer"_s);
}

void TestLsp::testStaleFileCount()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + u"/a.cpp"_s;
    writeFile(path, "int main() { return 0; }\n");

    ProjectGraph graph;
    graph.generateGraph(dir.path());
    QCOMPARE(graph.staleFileCount(), 0);

    // A file that vanished since the scan is stale.
    QVERIFY(QFile::remove(path));
    QVERIFY(graph.staleFileCount() >= 1);
}

void TestLsp::testLspToolsReportUnavailableWithoutProvider()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFile(dir.path() + u"/a.cpp"_s, "int main() { return 0; }\n");

    Sandbox sandbox(dir.path(), SandboxProfile::Workspace);
    DiskDocumentBridge bridge;
    ToolRunner runner(sandbox, &bridge);
    // No setLspProvider call: headless, like the core tests.

    ToolResult res = runner.run(makeCall(u"lsp_diagnostics"_s, QJsonObject{}));
    QVERIFY(!res.ok);
    QVERIFY(res.output.contains(u"not loaded"_s) || res.output.contains(u"No language server"_s));

    res = runner.run(makeCall(u"lsp_code_actions"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}));
    QVERIFY(!res.ok);
    QVERIFY(res.output.contains(u"No code actions"_s));
}

void TestLsp::testLspDiagnosticsWithProvider()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + u"/a.cpp"_s;
    writeFile(path, "int main() { return x; }\n");

    StubLsp lsp;
    LspDiagnostic err;
    err.path = path;
    err.line = 1;
    err.column = 24;
    err.severity = 1;
    err.message = u"'x' undeclared"_s;
    err.source = u"clangd"_s;
    LspDiagnostic warn;
    warn.path = path;
    warn.line = 1;
    warn.column = 5;
    warn.severity = 2;
    warn.message = u"unused variable"_s;
    lsp.diags = {err, warn};
    lsp.messages = {u"[LSP] clangd started"_s};

    Sandbox sandbox(dir.path(), SandboxProfile::Workspace);
    DiskDocumentBridge bridge;
    ToolRunner runner(sandbox, &bridge);
    runner.setLspProvider(&lsp);

    ToolResult res = runner.run(makeCall(u"lsp_diagnostics"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}));
    QVERIFY2(res.ok, qPrintable(res.output));
    QVERIFY(res.output.contains(u"1 error(s), 1 warning(s)"_s));
    QVERIFY(res.output.contains(u"a.cpp:1:24: error: 'x' undeclared [clangd]"_s));

    // Workspace-wide: summary line plus the recent server messages.
    res = runner.run(makeCall(u"lsp_diagnostics"_s, QJsonObject{}));
    QVERIFY2(res.ok, qPrintable(res.output));
    QVERIFY(res.output.contains(u"in the workspace"_s));
    QVERIFY(res.output.contains(u"clangd started"_s));
}

void TestLsp::testLspDiagnosticsSeverityFilter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + u"/a.cpp"_s;
    writeFile(path, "int main() { return 0; }\n");

    StubLsp lsp;
    LspDiagnostic err;
    err.path = path;
    err.line = 1;
    err.severity = 1;
    err.message = u"boom"_s;
    LspDiagnostic warn;
    warn.path = path;
    warn.line = 1;
    warn.severity = 2;
    warn.message = u"meh"_s;
    lsp.diags = {err, warn};

    Sandbox sandbox(dir.path(), SandboxProfile::Workspace);
    DiskDocumentBridge bridge;
    ToolRunner runner(sandbox, &bridge);
    runner.setLspProvider(&lsp);

    ToolResult res = runner.run(makeCall(u"lsp_diagnostics"_s,
                                         QJsonObject{{u"path"_s, u"a.cpp"_s}, {u"severity"_s, u"error"_s}}));
    QVERIFY2(res.ok, qPrintable(res.output));
    QVERIFY(res.output.contains(u"1 error(s), 0 warning(s)"_s));
    QVERIFY(res.output.contains(u"boom"_s));
    QVERIFY(!res.output.contains(u"meh"_s));
}

void TestLsp::testLspSymbolsViaGraph()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFile(dir.path() + u"/a.cpp"_s,
              "class Thing {\n"
              "};\n"
              "int compute() {\n"
              "    return 42;\n"
              "}\n");

    ProjectGraph graph;
    graph.generateGraph(dir.path());

    Sandbox sandbox(dir.path(), SandboxProfile::Workspace);
    DiskDocumentBridge bridge;
    ToolRunner runner(sandbox, &bridge);
    runner.setProjectGraph(&graph);
    // No LSP provider: the graph fallback must answer.
    StubLsp lsp;
    lsp.available = false;
    runner.setLspProvider(&lsp);

    ToolResult res = runner.run(makeCall(u"lsp_symbols"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}));
    QVERIFY2(res.ok, qPrintable(res.output));
    QVERIFY(res.output.contains(u"class Thing"_s));
    QVERIFY(res.output.contains(u"function compute"_s));
    QVERIFY2(res.output.contains(u"3:5: function compute"_s), qPrintable(res.output));

    // Hover inside compute() names the enclosing symbol and shows its code.
    res = runner.run(makeCall(u"lsp_hover"_s,
                             QJsonObject{{u"path"_s, u"a.cpp"_s}, {u"line"_s, 5}}));
    QVERIFY2(res.ok, qPrintable(res.output));
    QVERIFY(res.output.contains(u"function compute"_s));
    QVERIFY(res.output.contains(u"return 42;"_s));
}

void TestLsp::testLspReferencesTextFallback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFile(dir.path() + u"/def.cpp"_s, "void helper() {\n}\n");
    writeFile(dir.path() + u"/use.cpp"_s, "void caller() {\n    helper();\n}\n");

    ProjectGraph graph;
    graph.generateGraph(dir.path());

    Sandbox sandbox(dir.path(), SandboxProfile::Workspace);
    DiskDocumentBridge bridge;
    ToolRunner runner(sandbox, &bridge);
    runner.setProjectGraph(&graph);

    ToolResult res = runner.run(makeCall(u"lsp_references"_s,
                                         QJsonObject{{u"path"_s, u"use.cpp"_s}, {u"line"_s, 2}, {u"symbol"_s, u"helper"_s}}));
    QVERIFY2(res.ok, qPrintable(res.output));
    QVERIFY(res.output.contains(u"helper"_s));
    QVERIFY(res.output.contains(u"def.cpp"_s));
    QVERIFY(res.output.contains(u"use.cpp:2"_s));

    // Definition finds the declaration line via the graph plus text matches.
    res = runner.run(makeCall(u"lsp_definition"_s,
                             QJsonObject{{u"path"_s, u"use.cpp"_s}, {u"line"_s, 2}, {u"symbol"_s, u"helper"_s}}));
    QVERIFY2(res.ok, qPrintable(res.output));
    QVERIFY(res.output.contains(u"def.cpp:1"_s));
}

void TestLsp::testLspCodeActions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFile(dir.path() + u"/a.cpp"_s, "int main() { return x; }\n");

    StubLsp lsp;
    lsp.actions = {u"Declare 'x'"_s, u"Remove unused statement"_s};

    Sandbox sandbox(dir.path(), SandboxProfile::Workspace);
    DiskDocumentBridge bridge;
    ToolRunner runner(sandbox, &bridge);
    runner.setLspProvider(&lsp);

    ToolResult res = runner.run(makeCall(u"lsp_code_actions"_s,
                                         QJsonObject{{u"path"_s, u"a.cpp"_s}, {u"line"_s, 1}}));
    QVERIFY2(res.ok, qPrintable(res.output));
    QVERIFY(res.output.contains(u"Declare 'x'"_s));
}

void TestLsp::testToolRegistration()
{
    // The LSP tools are advertised to the model...
    const QJsonArray defs = toolDefinitions(ToolAccess::unrestricted());
    QStringList names;
    for (const QJsonValue &v : defs) {
        names.append(v.toObject().value(u"function"_s).toObject().value(u"name"_s).toString());
    }
    for (const QString &name : {u"lsp_diagnostics"_s, u"lsp_symbols"_s, u"lsp_hover"_s,
                                u"lsp_definition"_s, u"lsp_references"_s, u"lsp_code_actions"_s}) {
        QVERIFY2(names.contains(name), qPrintable(name));
        QVERIFY2(allBuiltInToolNames().contains(name), qPrintable(name));
    }

    // ...classified read-only (no prompt, allowed in read-only modes)...
    PermissionPolicy policy;
    for (const QString &name : {u"lsp_diagnostics"_s, u"lsp_symbols"_s, u"lsp_hover"_s,
                                u"lsp_definition"_s, u"lsp_references"_s, u"lsp_code_actions"_s}) {
        QVERIFY2(policy.isReadTool(name), qPrintable(name));
        QCOMPARE(policy.riskFor(name), ToolRisk::Read);
        QVERIFY2(builtInToolsForGroup(ToolGroup::read()).contains(name), qPrintable(name));
    }
}

// QTEST_MAIN, not GUILESS: the DiskDocumentBridge asks KTextEditor for open
// documents, and the editor singleton needs a QGuiApplication.
QTEST_MAIN(TestLsp)
#include "test_lsp.moc"
