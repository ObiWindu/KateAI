/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "debug.h"
#include "documentbridge.h"
#include "ecosystem.h"
#include "katedebugbridge.h"
#include "permissions.h"
#include "sandbox.h"
#include "tools.h"
#include "types.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

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

ToolCall makeCall(const QString &name, const QJsonObject &args = {})
{
    ToolCall c;
    c.id = u"dbg-1"_s;
    c.name = name;
    c.arguments = args;
    return c;
}

class StubDebugProvider : public DebugProvider
{
public:
    bool available = true;
    DebugSessionState state = DebugSessionState::Paused;
    QString target = u"app-debug"_s;
    DebugStopInfo stop;
    QList<DebugThread> threadList;
    QList<DebugStackFrame> frameList;
    QList<DebugVariable> localList;
    QList<DebugBreakpoint> bpList;
    QList<DebugTarget> targetList;
    QString consoleOut = u"Program stopped at src/main.cpp:42\n"_s;
    QString lastAction;
    QString lastEvalExpr;
    QString lastBpPath;
    int lastBpLine = 0;

    bool isAvailable() const override { return available; }
    DebugSessionState sessionState() const override { return state; }
    QString activeTarget() const override { return target; }
    QList<DebugTarget> targets() const override { return targetList; }
    DebugStopInfo currentStop() const override { return stop; }
    QList<DebugThread> threads() const override { return threadList; }
    QList<DebugStackFrame> stackFrames(int) const override { return frameList; }
    QList<DebugVariable> locals(int) const override { return localList; }
    QList<DebugBreakpoint> breakpoints() const override { return bpList; }
    QString recentOutput(int maxChars) const override { return consoleOut.right(maxChars); }

    DebugActionResult selectFrame(int level) override
    {
        lastAction = u"select_frame:%1"_s.arg(level);
        return {level >= 0 && level < frameList.size(), u"Selected frame #%1"_s.arg(level), stop};
    }
    DebugActionResult selectThread(int tid) override
    {
        lastAction = u"select_thread:%1"_s.arg(tid);
        return {tid > 0, u"Selected thread #%1"_s.arg(tid), stop};
    }
    DebugActionResult setBreakpoint(const QString &file, int line, const QString &cond,
                                    const QString &, const QString &, bool remove) override
    {
        lastBpPath = file;
        lastBpLine = line;
        lastAction = remove ? u"rm_bp"_s : u"set_bp"_s;
        return {true, u"%1 breakpoint at %2:%3%4"_s.arg(remove ? u"Removed"_s : u"Set"_s, file).arg(line).arg(cond.isEmpty() ? QString() : u" if "_s + cond), stop};
    }
    DebugActionResult clearAllBreakpoints() override
    {
        bpList.clear();
        return {true, u"Cleared all breakpoints."_s, stop};
    }
    DebugActionResult step(const QString &action, const QString &file, int line, int) override
    {
        lastAction = u"step:%1:%2:%3"_s.arg(action, file).arg(line);
        return {true, u"Stepped (%1)."_s.arg(action), stop};
    }
    DebugActionResult evaluate(const QString &expr, int) override
    {
        lastEvalExpr = expr;
        return {true, u"%1 = 42"_s.arg(expr), stop};
    }
    DebugActionResult controlSession(const QString &action, const QString &targetName, int) override
    {
        lastAction = u"session:%1:%2"_s.arg(action, targetName);
        return {true, u"Session %1 OK"_s.arg(action), stop};
    }
    DebugActionResult sendConsoleCommand(const QString &cmd) override
    {
        lastAction = u"console:%1"_s.arg(cmd);
        return {true, u"Console output for %1"_s.arg(cmd), stop};
    }
    bool reloadLaunchJsonTargets() override { return true; }
};

class StubEcosystemProvider : public EcosystemProvider
{
public:
    bool buildAvail = true;
    bool termAvail = true;
    QList<BuildTargetInfo> targetInfos;
    QString lastBuiltTarget;
    QString lastTermCmd;
    QString lastTermDir;

    bool isBuildPluginAvailable() const override { return buildAvail; }
    QList<BuildTargetInfo> buildTargets(const QString &ws) const override
    {
        return targetInfos.isEmpty() ? discoverWorkspaceBuildTargets(ws) : targetInfos;
    }
    EcosystemActionResult triggerBuild(const QString &targetName) override
    {
        lastBuiltTarget = targetName;
        return {true, u"Triggered build: %1"_s.arg(targetName.isEmpty() ? u"default"_s : targetName)};
    }
    bool isTerminalAvailable() const override { return termAvail; }
    EcosystemActionResult runInTerminal(const QString &command, const QString &workingDir) override
    {
        lastTermCmd = command;
        lastTermDir = workingDir;
        return {true, u"Sent to Konsole: %1"_s.arg(command)};
    }
};

} // namespace

class TestDebug : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCrashReasonDetection();
    void testContextSummaryAndSnapshots();
    void testLaunchJsonGenerator();
    void testBridgeModelAndOutputParsers();
    void testDebugToolsAndRiskGating();
    void testEcosystemToolsAndSandbox();
};

void TestDebug::testCrashReasonDetection()
{
    QVERIFY(DebugProvider::isCrashStopReason(u"exception"_s));
    QVERIFY(DebugProvider::isCrashStopReason(u"signal"_s, u"SIGSEGV segmentation fault"_s));
    QVERIFY(DebugProvider::isCrashStopReason(u"panic"_s));
    QVERIFY(!DebugProvider::isCrashStopReason(u"breakpoint"_s));
    QVERIFY(!DebugProvider::isCrashStopReason(u"step"_s));
}

void TestDebug::testContextSummaryAndSnapshots()
{
    StubDebugProvider stub;
    stub.stop.reason = u"signal"_s;
    stub.stop.description = u"SIGSEGV"_s;
    stub.stop.file = u"/work/src/main.cpp"_s;
    stub.stop.line = 42;
    stub.stop.isCrashOrException = true;
    stub.frameList = {
        {0, 1, u"crashHere"_s, u"/work/src/main.cpp"_s, 42, 0, {}, true},
        {1, 2, u"main"_s, u"/work/src/main.cpp"_s, 100, 0, {}, false},
    };
    stub.localList = {
        {u"ptr"_s, u"int *"_s, u"0x0"_s, u"Locals"_s, 0, {}},
        {u"count"_s, u"int"_s, u"3"_s, u"Locals"_s, 0, {}},
    };

    const QString ctx = stub.contextSummary();
    QVERIFY(ctx.contains(u"Debugger: paused"_s));
    QVERIFY(ctx.contains(u"crashHere"_s));
    QVERIFY(ctx.contains(u"ptr"_s));
    QVERIFY(ctx.contains(u"0x0"_s));

    const DebugSnapshot s1 = stub.captureSnapshot(u"good"_s);
    QCOMPARE(s1.label, u"good"_s);

    stub.stop.line = 45;
    stub.localList[1].value = u"99"_s;
    stub.localList.append({u"extra"_s, u"bool"_s, u"true"_s, u"Locals"_s, 0, {}});
    const DebugSnapshot s2 = stub.captureSnapshot(u"bad"_s);

    const QString diff = DebugProvider::diffSnapshots(s1, s2);
    QVERIFY(diff.contains(u"good"_s));
    QVERIFY(diff.contains(u"bad"_s));
    QVERIFY(diff.contains(u"count"_s));
    QVERIFY(diff.contains(u"3"_s));
    QVERIFY(diff.contains(u"99"_s));
    QVERIFY(diff.contains(u"extra"_s));
}

void TestDebug::testLaunchJsonGenerator()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFile(dir.path() + u"/CMakeLists.txt"_s, "cmake_minimum_required(VERSION 3.20)\nproject(Demo)\nadd_executable(demo_app main.cpp)\n");
    writeFile(dir.path() + u"/Cargo.toml"_s, "[package]\nname = \"demo_rs\"\nversion = \"0.1.0\"\n");
    writeFile(dir.path() + u"/main.py"_s, "print('hello')\n");

    QString summary;
    const QJsonDocument doc = DebugProvider::generateLaunchConfig(dir.path(), &summary);
    QVERIFY(!doc.isNull());
    const QJsonObject root = doc.object();
    QCOMPARE(root.value(u"version"_s).toString(), u"0.2.0"_s);
    const QJsonArray configs = root.value(u"configurations"_s).toArray();
    QVERIFY(configs.size() >= 3);
    QVERIFY(!summary.isEmpty());
}

void TestDebug::testBridgeModelAndOutputParsers()
{
    // Stack model matching kategdbplugin StackView: columns [Level, Function, File:Line]
    QStandardItemModel stackModel;
    stackModel.appendRow({
        new QStandardItem(u"0"_s),
        new QStandardItem(u"compute()"_s),
        new QStandardItem(u"/proj/src/calc.cpp:17"_s),
    });
    stackModel.appendRow({
        new QStandardItem(u"1"_s),
        new QStandardItem(u"main"_s),
        new QStandardItem(u"/proj/src/main.cpp:8"_s),
    });
    const QList<DebugStackFrame> frames = KateDebugBridge::readStackFramesFromModel(&stackModel, 0);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0).function, u"compute()"_s);
    QCOMPARE(frames.at(0).file, u"/proj/src/calc.cpp"_s);
    QCOMPARE(frames.at(0).line, 17);
    QVERIFY(frames.at(0).active);

    // Locals tree matching kategdbplugin LocalsView: columns [Symbol, Value]
    QTreeWidget localsTree;
    localsTree.setColumnCount(2);
    auto *item = new QTreeWidgetItem(&localsTree, {u"total"_s, u"128"_s});
    item->setToolTip(1, u"int"_s);
    auto *child = new QTreeWidgetItem(item, {u"size"_s, u"4"_s});
    Q_UNUSED(child);
    const QList<DebugVariable> vars = KateDebugBridge::readLocalsFromTree(&localsTree);
    QCOMPARE(vars.size(), 1);
    QCOMPARE(vars.at(0).name, u"total"_s);
    QCOMPARE(vars.at(0).value, u"128"_s);
    QCOMPARE(vars.at(0).children.size(), 1);
    QCOMPARE(vars.at(0).children.at(0).name, u"size"_s);

    // Output parsers
    DebugStopInfo stop;
    QVERIFY(KateDebugBridge::parseStopEventFromOutput(u"Thread 1 stopped (exception: division by zero)"_s, &stop));
    QVERIFY(stop.isCrashOrException);

    QString evalVal;
    QVERIFY(KateDebugBridge::parseEvalResultFromOutput(u"$1 = 42\n"_s, u"x + 1"_s, &evalVal));
    QCOMPARE(evalVal, u"42"_s);
}

void TestDebug::testDebugToolsAndRiskGating()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString srcFile = dir.path() + u"/main.cpp"_s;
    writeFile(srcFile, "int main() {\n    return 0;\n}\n");

    DiskDocumentBridge bridge;
    Sandbox sandbox(dir.path(), SandboxProfile::Workspace);
    ToolRunner runner(sandbox, &bridge);

    // Without provider, debug_status reports unavailable cleanly.
    const ToolResult unavail = runner.run(makeCall(u"debug_status"_s));
    QVERIFY(!unavail.ok);
    QVERIFY(unavail.output.contains(u"kategdbplugin"_s));

    StubDebugProvider stub;
    stub.stop.reason = u"breakpoint"_s;
    stub.stop.file = srcFile;
    stub.stop.line = 2;
    stub.frameList = {{0, 1, u"main"_s, srcFile, 2, 0, {}, true}};
    stub.localList = {{u"x"_s, u"int"_s, u"7"_s, u"Locals"_s, 0, {}}};
    runner.setDebugProvider(&stub);

    // Risk classification invariants
    QCOMPARE(runner.describe(makeCall(u"debug_status"_s)).risk, ToolRisk::Read);
    QCOMPARE(runner.describe(makeCall(u"debug_stack"_s)).risk, ToolRisk::Read);
    QCOMPARE(runner.describe(makeCall(u"debug_locals"_s)).risk, ToolRisk::Read);
    QCOMPARE(runner.describe(makeCall(u"debug_breakpoints"_s)).risk, ToolRisk::Read);
    QCOMPARE(runner.describe(makeCall(u"debug_select_frame"_s, {{u"frame_level"_s, 0}})).risk, ToolRisk::Read);
    QCOMPARE(runner.describe(makeCall(u"debug_snapshot"_s)).risk, ToolRisk::Read);
    QCOMPARE(runner.describe(makeCall(u"debug_set_breakpoint"_s, {{u"path"_s, u"main.cpp"_s}, {u"line"_s, 2}})).risk, ToolRisk::Write);
    QCOMPARE(runner.describe(makeCall(u"debug_step"_s, {{u"action"_s, u"step_over"_s}})).risk, ToolRisk::Execute);
    QCOMPARE(runner.describe(makeCall(u"debug_eval"_s, {{u"expression"_s, u"x"_s}})).risk, ToolRisk::Execute);
    QCOMPARE(runner.describe(makeCall(u"debug_session"_s, {{u"action"_s, u"start"_s}})).risk, ToolRisk::Execute);
    QCOMPARE(runner.describe(makeCall(u"debug_console"_s, {{u"command"_s, u"whereami"_s}})).risk, ToolRisk::Execute);

    // Execution checks
    QVERIFY(runner.run(makeCall(u"debug_status"_s)).ok);
    const ToolResult stackRes = runner.run(makeCall(u"debug_stack"_s));
    QVERIFY(stackRes.ok);
    QVERIFY(stackRes.output.contains(u"main"_s));

    const ToolResult localsRes = runner.run(makeCall(u"debug_locals"_s));
    QVERIFY(localsRes.ok);
    QVERIFY(localsRes.output.contains(u"x : int = 7"_s));

    // Breakpoint inside workspace works; outside workspace is blocked by sandbox.
    const ToolResult bpOk = runner.run(makeCall(u"debug_set_breakpoint"_s, {{u"path"_s, u"main.cpp"_s}, {u"line"_s, 2}}));
    QVERIFY(bpOk.ok);
    QCOMPARE(stub.lastBpLine, 2);

    const ToolResult bpDenied = runner.run(makeCall(u"debug_set_breakpoint"_s, {{u"path"_s, u"/etc/passwd"_s}, {u"line"_s, 1}}));
    QVERIFY(!bpDenied.ok);

    // Eval & step
    const ToolResult evalRes = runner.run(makeCall(u"debug_eval"_s, {{u"expression"_s, u"x + 35"_s}}));
    QVERIFY(evalRes.ok);
    QVERIFY(evalRes.output.contains(u"42"_s));

    const ToolResult stepRes = runner.run(makeCall(u"debug_step"_s, {{u"action"_s, u"step_over"_s}}));
    QVERIFY(stepRes.ok);
    QVERIFY(stub.lastAction.startsWith(u"step:step_over"_s));
}

void TestDebug::testEcosystemToolsAndSandbox()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFile(dir.path() + u"/CMakeLists.txt"_s, "project(T)\nadd_executable(mybin main.cpp)\n");
    writeFile(dir.path() + u"/build/compile_commands.json"_s, "[]\n");

    DiskDocumentBridge bridge;
    Sandbox sandbox(dir.path(), SandboxProfile::Workspace);
    ToolRunner runner(sandbox, &bridge);
    StubEcosystemProvider eco;
    runner.setEcosystemProvider(&eco);

    QCOMPARE(runner.describe(makeCall(u"build_targets"_s)).risk, ToolRisk::Read);
    QCOMPARE(runner.describe(makeCall(u"build_run"_s)).risk, ToolRisk::Execute);
    QCOMPARE(runner.describe(makeCall(u"terminal_run"_s, {{u"command"_s, u"echo hi"_s}})).risk, ToolRisk::Execute);

    const ToolResult targets = runner.run(makeCall(u"build_targets"_s));
    QVERIFY(targets.ok);
    QVERIFY(targets.output.contains(u"mybin"_s));
    QVERIFY(targets.output.contains(u"compile_commands.json"_s));

    const ToolResult buildRes = runner.run(makeCall(u"build_run"_s, {{u"target"_s, u"mybin"_s}}));
    QVERIFY(buildRes.ok);
    QCOMPARE(eco.lastBuiltTarget, u"mybin"_s);

    const ToolResult termOk = runner.run(makeCall(u"terminal_run"_s, {{u"command"_s, u"cmake --build build"_s}}));
    QVERIFY(termOk.ok);
    QCOMPARE(eco.lastTermCmd, u"cmake --build build"_s);

    // Always-denied destructive command must be blocked even via terminal_run.
    const ToolResult termDenied = runner.run(makeCall(u"terminal_run"_s, {{u"command"_s, u"rm -rf /"_s}}));
    QVERIFY(!termDenied.ok);
}

QTEST_MAIN(TestDebug)
#include "test_debug.moc"
