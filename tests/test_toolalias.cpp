/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

// How ACP tool calls are read: the names agents give them, the files and diffs
// they carry, and how the modes treat them. These are pure functions of the
// JSON, so no agent process is needed.

#include "acpclient.h"
#include "modes.h"
#include "types.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestToolAlias : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void kindsAndAgentNamesCanonicalize()
    {
        // The ACP tool kinds stand for the Kate tools they name.
        QCOMPARE(canonicalToolName(u"read"_s), u"read_file"_s);
        QCOMPARE(canonicalToolName(u"search"_s), u"grep"_s);
        QCOMPARE(canonicalToolName(u"fetch"_s), u"web_fetch"_s);
        QCOMPARE(canonicalToolName(u"execute"_s), u"bash"_s);
        QCOMPARE(canonicalToolName(u"edit"_s), u"edit_file"_s);
        // Agents spell the same tools in other cases.
        QCOMPARE(canonicalToolName(u"Grep"_s), u"grep"_s);
        QCOMPARE(canonicalToolName(u"BASH"_s), u"bash"_s);
        QCOMPARE(canonicalToolName(u"Task"_s), subtaskToolName());
        QCOMPARE(canonicalToolName(u"WebFetch"_s), u"web_fetch"_s);
        // Names with no Kate equivalent keep their spelling, and MCP names are
        // never rewritten.
        QCOMPARE(canonicalToolName(u"move"_s), u"move"_s);
        QCOMPARE(canonicalToolName(u"mcp__server__thing"_s), u"mcp__server__thing"_s);
    }

    void kindOnlyCallKeepsItsIdentity()
    {
        AcpClient client;
        client.setWorkspace(u"/ws"_s);
        const QJsonObject call{
            {u"toolCallId"_s, u"c1"_s},
            {u"kind"_s, u"read"_s},
            {u"title"_s, u"Read a.cpp"_s},
            {u"locations"_s, QJsonArray{QJsonObject{{u"path"_s, u"/ws/a.cpp"_s}}}},
        };
        const PermissionRequest request = client.permissionFromToolCall(call);
        QCOMPARE(request.toolName, u"read_file"_s);
        QCOMPARE(request.originalToolName, u"read"_s);
        QCOMPARE(request.risk, ToolRisk::Read);
        QVERIFY(!request.mutatesFiles);
        QCOMPARE(request.path, u"/ws/a.cpp"_s);
    }

    void moveKindMutatesFiles()
    {
        AcpClient client;
        const QJsonObject call{
            {u"toolCallId"_s, u"c2"_s},
            {u"kind"_s, u"move"_s},
            {u"locations"_s, QJsonArray{QJsonObject{{u"path"_s, u"/ws/old.cpp"_s}}}},
        };
        const PermissionRequest request = client.permissionFromToolCall(call);
        QVERIFY(request.mutatesFiles);
        QCOMPARE(request.risk, ToolRisk::Write);
    }

    void diffsStayWithTheFileTheyEdit()
    {
        // The location names a.cpp, but the diffs edit b.cpp and then a.cpp. The
        // a.cpp diff is the primary one; b.cpp's text must not be shown as a.cpp's.
        AcpClient client;
        client.setWorkspace(u"/ws"_s);
        const QJsonObject call{
            {u"toolCallId"_s, u"c3"_s},
            {u"kind"_s, u"edit"_s},
            {u"locations"_s, QJsonArray{QJsonObject{{u"path"_s, u"/ws/a.cpp"_s}}}},
            {u"content"_s, QJsonArray{
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/ws/b.cpp"_s}, {u"oldText"_s, u"alpha"_s}, {u"newText"_s, u"beta"_s}},
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/ws/a.cpp"_s}, {u"oldText"_s, u"p"_s}, {u"newText"_s, u"q"_s}},
            }},
        };
        const PermissionRequest request = client.permissionFromToolCall(call);
        QCOMPARE(request.path, u"/ws/a.cpp"_s);
        QVERIFY(request.describeDiff.contains(u"+q"_s));
        QVERIFY(!request.describeDiff.contains(u"beta"_s));
        QCOMPARE(request.additionalPaths, QStringList{u"/ws/b.cpp"_s});
        QCOMPARE(request.additionalDiffs.size(), 1);
        QVERIFY(request.additionalDiffs.at(0).contains(u"+beta"_s));
    }

    void diffWithoutALocationBecomesThePrimaryFile()
    {
        AcpClient client;
        const QJsonObject call{
            {u"toolCallId"_s, u"c4"_s},
            {u"kind"_s, u"edit"_s},
            {u"content"_s, QJsonArray{
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/ws/only.cpp"_s}, {u"oldText"_s, u"x"_s}, {u"newText"_s, u"y"_s}},
            }},
        };
        const PermissionRequest request = client.permissionFromToolCall(call);
        QCOMPARE(request.path, u"/ws/only.cpp"_s);
        QVERIFY(request.describeDiff.contains(u"+y"_s));
        QVERIFY(request.additionalPaths.isEmpty());
    }

    void contentOnlyDiffIsRecordedInTheArguments()
    {
        // The arguments are what a restored transcript rebuilds the card from.
        // A diff that came only as a content block must be recorded there too,
        // in the shape the edit tools use.
        AcpClient client;
        const QJsonObject edit{
            {u"toolCallId"_s, u"c5"_s},
            {u"kind"_s, u"edit"_s},
            {u"content"_s, QJsonArray{
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/ws/a.cpp"_s}, {u"oldText"_s, u"old"_s}, {u"newText"_s, u"new"_s}},
            }},
        };
        const QJsonObject editArgs = QJsonDocument::fromJson(client.permissionFromToolCall(edit).details.toUtf8()).object();
        QCOMPARE(editArgs.value(u"old_string"_s).toString(), u"old"_s);
        QCOMPARE(editArgs.value(u"new_string"_s).toString(), u"new"_s);
        QCOMPARE(editArgs.value(u"path"_s).toString(), u"/ws/a.cpp"_s);

        const QJsonObject created{
            {u"toolCallId"_s, u"c6"_s},
            {u"kind"_s, u"edit"_s},
            {u"content"_s, QJsonArray{
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/ws/new.cpp"_s}, {u"newText"_s, u"fresh"_s}},
            }},
        };
        const QJsonObject createdArgs = QJsonDocument::fromJson(client.permissionFromToolCall(created).details.toUtf8()).object();
        QCOMPARE(createdArgs.value(u"content"_s).toString(), u"fresh"_s);
        QVERIFY(!createdArgs.contains(u"old_string"_s));
    }

    void mergeKeepsTheAnnouncedIdentity()
    {
        AcpClient client;
        const PermissionRequest announced = client.permissionFromToolCall(QJsonObject{
            {u"toolCallId"_s, u"c7"_s},
            {u"kind"_s, u"edit"_s},
            {u"content"_s, QJsonArray{
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/ws/a.cpp"_s}, {u"oldText"_s, u"o"_s}, {u"newText"_s, u"n"_s}},
            }},
        });
        // A permission request that repeats only the id is the common shape.
        const PermissionRequest bare = client.permissionFromToolCall(QJsonObject{{u"toolCallId"_s, u"c7"_s}});
        QCOMPARE(bare.toolName, u"tool"_s);

        const PermissionRequest merged = AcpClient::mergeToolCallDetails(announced, bare);
        QCOMPARE(merged.toolName, u"edit_file"_s);
        QCOMPARE(merged.originalToolName, u"edit"_s);
        QCOMPARE(merged.describeDiff, announced.describeDiff);
        QCOMPARE(merged.path, u"/ws/a.cpp"_s);

        // The other way round, the identity comes from the message that has it.
        const PermissionRequest reversed = AcpClient::mergeToolCallDetails(bare, announced);
        QCOMPARE(reversed.toolName, u"edit_file"_s);
        QCOMPARE(reversed.describeDiff, announced.describeDiff);
    }

    void planModeAllowsReadsAndRefusesEdits()
    {
        // The read-only tools a plan-mode turn may use, whatever name the agent
        // gives them. An agent that reports a read by its kind must not be refused.
        const ToolAccess plan = ModeRegistry::toolAccessFor(ModeDefinition{}, true);
        QVERIFY(plan.allows(canonicalToolName(u"read"_s)));
        QVERIFY(plan.allows(canonicalToolName(u"search"_s)));
        QVERIFY(plan.allows(canonicalToolName(u"Grep"_s)));
        QVERIFY(!plan.allows(canonicalToolName(u"edit"_s)));
        QVERIFY(!plan.allows(canonicalToolName(u"execute"_s)));
        QVERIFY(!plan.allows(canonicalToolName(u"delete"_s)));
    }

    void modesDecideAcpToolsByWhatTheyDo()
    {
        const ToolAccess plan = ModeRegistry::toolAccessFor(ModeDefinition{}, true);
        ModeDefinition codeMode;
        codeMode.groups = {ToolGroup::read(), ToolGroup::edit(), ToolGroup::command()};
        const ToolAccess code = ModeRegistry::toolAccessFor(codeMode, false);

        auto request = [](const QString &name, ToolRisk risk, bool mutates) {
            PermissionRequest r;
            r.toolName = canonicalToolName(name);
            r.risk = risk;
            r.mutatesFiles = mutates;
            return r;
        };

        // A Kate tool is judged by its name.
        QVERIFY(!acpToolPermitted(plan, request(u"edit"_s, ToolRisk::Write, true)));
        QVERIFY(acpToolPermitted(code, request(u"edit"_s, ToolRisk::Write, true)));
        QVERIFY(!acpToolPermitted(plan, request(u"execute"_s, ToolRisk::Execute, false)));
        // Reasoning is allowed everywhere.
        QVERIFY(acpToolPermitted(plan, request(u"think"_s, ToolRisk::Read, false)));
        // An agent's own tool with no Kate equivalent is judged by what it does,
        // so a todo tool keeps working in plan mode.
        QVERIFY(acpToolPermitted(plan, request(u"TodoWrite"_s, ToolRisk::Write, false)));
        QVERIFY(!acpToolPermitted(plan, request(u"apply_diff"_s, ToolRisk::Write, true)));
        QVERIFY(acpToolPermitted(code, request(u"apply_diff"_s, ToolRisk::Write, true)));
        // Unrestricted access allows everything.
        QVERIFY(acpToolPermitted(ToolAccess::unrestricted(), request(u"execute"_s, ToolRisk::Execute, false)));
    }

    void registryAgentToolNamesCanonicalize()
    {
        // Names registry agents actually send for ask-user, todos and patches.
        QCOMPARE(canonicalToolName(u"ask_user"_s), u"question"_s);
        QCOMPARE(canonicalToolName(u"ask_user_question"_s), u"question"_s);
        QCOMPARE(canonicalToolName(u"request_user_input"_s), u"question"_s);
        QCOMPARE(canonicalToolName(u"TodoWrite"_s), u"todo_write"_s);
        QCOMPARE(canonicalToolName(u"todo_read"_s), u"todo_read"_s);
        QCOMPARE(canonicalToolName(u"exit_plan_mode"_s), u"exit_plan_mode"_s);
        QCOMPARE(canonicalToolName(u"ExitPlanMode"_s), u"exit_plan_mode"_s);
        // apply_patch is its own tool now, not a single-file edit.
        QCOMPARE(canonicalToolName(u"apply_patch"_s), u"apply_patch"_s);
        QCOMPARE(canonicalToolName(u"patch"_s), u"apply_patch"_s);
        QCOMPARE(canonicalToolName(u"Shell"_s), u"bash"_s);
        QCOMPARE(canonicalToolName(u"RunCommand"_s), u"bash"_s);
        QCOMPARE(canonicalToolName(u"WebFetch"_s), u"web_fetch"_s);
        QCOMPARE(canonicalToolName(u"View"_s), u"read_file"_s);
        QCOMPARE(canonicalToolName(u"find_files"_s), u"glob"_s);
        QCOMPARE(canonicalToolName(u"multi_replace_file_content"_s), u"multi_edit_file"_s);
    }

    void applyPatchToolCallParsesPerFileDiffs()
    {
        AcpClient client;
        client.setWorkspace(u"/ws"_s);
        const QString patch = u"--- a/src/a.cpp\n+++ b/src/a.cpp\n@@ -1,2 +1,2 @@\n-old\n+new\n"
                              u"--- a/src/b.cpp\n+++ b/src/b.cpp\n@@ -5,1 +5,1 @@\n-x\n+y\n"_s;
        const QJsonObject call{
            {u"toolCallId"_s, u"c9"_s},
            {u"name"_s, u"apply_patch"_s},
            {u"kind"_s, u"edit"_s},
            {u"title"_s, u"Apply patch"_s},
            {u"content"_s, QJsonArray{QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, patch}}}},
        };
        const PermissionRequest request = client.permissionFromToolCall(call);
        QCOMPARE(request.toolName, u"apply_patch"_s);
        QVERIFY(request.mutatesFiles);
        QCOMPARE(request.path, u"src/a.cpp"_s);
        QVERIFY(request.describeDiff.contains(u"+++ b/src/a.cpp"_s));
        QCOMPARE(request.additionalPaths, QStringList{u"src/b.cpp"_s});
        QCOMPARE(request.additionalDiffs.size(), 1);
        QVERIFY(request.additionalDiffs.first().contains(u"+++ b/src/b.cpp"_s));
    }
};

QTEST_GUILESS_MAIN(TestToolAlias)
#include "test_toolalias.moc"
