/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Every tool card the OpenCode ACP agent can send must resolve to a proper
 * card: an icon, a human label, the family's colour, and the right expanded
 * surface (diff for edits, code for reads, terminal for commands, matches
 * for searches). OpenCode names its tools in lowercase without the _file
 * suffix (read, write, edit, list, patch, todowrite, webfetch, websearch,
 * task), so each mapping below pins one of those names to the same look
 * the native tool already has.
 */

#include "acpclient.h"
#include "chattheme.h"
#include "toolcallwidget.h"
#include "tools.h"
#include "types.h"

#include <QApplication>
#include <QLabel>
#include <QTest>
#include <QTextBrowser>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

QString cardIcon(const QString &tool)
{
    ToolCallWidget card(u"call-icon"_s);
    card.setToolInfo(tool, u"args"_s, ToolRisk::Read);
    QLabel *icon = card.findChild<QLabel *>(u"toolIcon"_s);
    Q_ASSERT(icon);
    return icon->text();
}

QString cardCaption(const QString &tool, const QString &finishedOutput = {})
{
    ToolCallWidget card(u"call-caption"_s);
    card.setToolInfo(tool, u"args"_s, ToolRisk::Read);
    if (!finishedOutput.isEmpty()) {
        ToolResult ok;
        ok.name = tool;
        ok.ok = true;
        ok.output = finishedOutput;
        card.setFinished(ok);
    }
    QLabel *caption = card.findChild<QLabel *>(u"toolViewCaption"_s);
    Q_ASSERT(caption);
    return caption->text();
}

} // namespace

class TestOpenCodeCards : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void labelsMatchNativeFamilies()
    {
        QCOMPARE(ChatTheme::toolLabel(u"read"_s), ChatTheme::toolLabel(u"read_file"_s));
        QCOMPARE(ChatTheme::toolLabel(u"write"_s), ChatTheme::toolLabel(u"write_file"_s));
        QCOMPARE(ChatTheme::toolLabel(u"edit"_s), ChatTheme::toolLabel(u"edit_file"_s));
        QCOMPARE(ChatTheme::toolLabel(u"patch"_s), ChatTheme::toolLabel(u"edit_file"_s));
        QCOMPARE(ChatTheme::toolLabel(u"list"_s), ChatTheme::toolLabel(u"list_dir"_s));
        QCOMPARE(ChatTheme::toolLabel(u"task"_s), ChatTheme::toolLabel(u"new_task"_s));
        QCOMPARE(ChatTheme::toolLabel(u"webfetch"_s), QStringLiteral("Fetch page"));
        QCOMPARE(ChatTheme::toolLabel(u"websearch"_s), ChatTheme::toolLabel(u"web_search"_s));
        QCOMPARE(ChatTheme::toolLabel(u"todowrite"_s), QStringLiteral("Update todo list"));
        QCOMPARE(ChatTheme::toolLabel(u"todoread"_s), QStringLiteral("Read todo list"));
    }

    void coloursMatchNativeFamilies()
    {
        QCOMPARE(ChatTheme::toolColor(u"read"_s), ChatTheme::toolColor(u"read_file"_s));
        QCOMPARE(ChatTheme::toolColor(u"write"_s), ChatTheme::toolColor(u"write_file"_s));
        QCOMPARE(ChatTheme::toolColor(u"edit"_s), ChatTheme::toolColor(u"edit_file"_s));
        QCOMPARE(ChatTheme::toolColor(u"patch"_s), ChatTheme::toolColor(u"edit_file"_s));
        QCOMPARE(ChatTheme::toolColor(u"list"_s), ChatTheme::toolColor(u"list_dir"_s));
        QCOMPARE(ChatTheme::toolColor(u"task"_s), ChatTheme::toolColor(u"new_task"_s));
        QCOMPARE(ChatTheme::toolColor(u"webfetch"_s), ChatTheme::toolColor(u"web_fetch"_s));
        QCOMPARE(ChatTheme::toolColor(u"websearch"_s), ChatTheme::toolColor(u"web_search"_s));
        QCOMPARE(ChatTheme::toolColor(u"todowrite"_s), ChatTheme::toolColor(u"write_plan_file"_s));
        QCOMPARE(ChatTheme::toolColor(u"todoread"_s), ChatTheme::toolColor(u"write_plan_file"_s));
        // Edit detection drives the diff surface, so the aliases must agree.
        QVERIFY(ChatTheme::isEditToolName(u"patch"_s));
        QVERIFY(ChatTheme::isEditToolName(u"write"_s));
        QVERIFY(ChatTheme::isEditToolName(u"edit"_s));
        QVERIFY(!ChatTheme::isEditToolName(u"read"_s));
        QVERIFY(!ChatTheme::isEditToolName(u"list"_s));
        QVERIFY(!ChatTheme::isEditToolName(u"task"_s));
        QVERIFY(!ChatTheme::isEditToolName(u"webfetch"_s));
    }

    void iconsMatchNativeFamilies()
    {
        QCOMPARE(cardIcon(u"read"_s), QStringLiteral(u"\U0001F4C4"));
        QCOMPARE(cardIcon(u"write"_s), QStringLiteral(u"\U0001F4DD"));
        QCOMPARE(cardIcon(u"edit"_s), QStringLiteral(u"\u270F"));
        QCOMPARE(cardIcon(u"patch"_s), QStringLiteral(u"\u270F"));
        QCOMPARE(cardIcon(u"list"_s), QStringLiteral(u"\U0001F4C1"));
        QCOMPARE(cardIcon(u"grep"_s), QStringLiteral(u"\U0001F50D"));
        QCOMPARE(cardIcon(u"glob"_s), QStringLiteral(u"\U0001F50E"));
        QCOMPARE(cardIcon(u"bash"_s), QStringLiteral(u"\u26A1"));
        QCOMPARE(cardIcon(u"websearch"_s), QStringLiteral(u"\U0001F310"));
        QCOMPARE(cardIcon(u"webfetch"_s), QStringLiteral(u"\U0001F4D6"));
        QCOMPARE(cardIcon(u"task"_s), QStringLiteral(u"\U0001F9E9"));
        QCOMPARE(cardIcon(u"todowrite"_s), QStringLiteral(u"\U0001F4CB"));
        QCOMPARE(cardIcon(u"todoread"_s), QStringLiteral(u"\U0001F4CB"));
    }

    void detailViewsMatchNativeFamilies()
    {
        // Reads land on the code surface, edits on the diff surface, commands
        // on the terminal, searches on matches.
        QCOMPARE(cardCaption(u"read"_s, u"contents"_s), QStringLiteral("File contents"));
        QCOMPARE(cardCaption(u"webfetch"_s, u"contents"_s), QStringLiteral("File contents"));
        QCOMPARE(cardCaption(u"websearch"_s, u"result"_s), QStringLiteral("Matches"));
        QCOMPARE(cardCaption(u"bash"_s, u"out"_s), QStringLiteral("Output"));
        // An edit whose result carries a unified diff opens on Changes.
        const QString diff = QStringLiteral("--- a/f\n+++ b/f\n@@ -1 +1 @@\n-old\n+new\n");
        QCOMPARE(cardCaption(u"edit"_s, diff), QStringLiteral("Changes"));
        QCOMPARE(cardCaption(u"patch"_s, diff), QStringLiteral("Changes"));
        QCOMPARE(cardCaption(u"write"_s, diff), QStringLiteral("Changes"));
    }

    void acpPermissionParsesOpenCodeArgs()
    {
        AcpClient client;
        // OpenCode spells the path camelCase and the edit pair oldText/newText.
        QJsonObject toolCall;
        toolCall.insert(u"toolCallId"_s, u"call-edit"_s);
        toolCall.insert(u"name"_s, u"edit"_s);
        toolCall.insert(u"title"_s, u"Edit main.cpp"_s);
        toolCall.insert(u"kind"_s, u"edit"_s);
        QJsonObject rawInput;
        rawInput.insert(u"filePath"_s, u"/tmp/main.cpp"_s);
        rawInput.insert(u"oldText"_s, u"int old = 1;"_s);
        rawInput.insert(u"newText"_s, u"int old = 2;"_s);
        toolCall.insert(u"rawInput"_s, rawInput);
        const PermissionRequest request = client.permissionFromToolCall(toolCall);
        QCOMPARE(request.path, u"/tmp/main.cpp"_s);
        QVERIFY(request.mutatesFiles);
        QVERIFY(request.describeDiff.contains(u"-int old = 1;"_s));
        QVERIFY(request.describeDiff.contains(u"+int old = 2;"_s));

        // A write whose content arrives under contents still gets a diff.
        QJsonObject writeCall;
        writeCall.insert(u"toolCallId"_s, u"call-write"_s);
        writeCall.insert(u"name"_s, u"write"_s);
        writeCall.insert(u"title"_s, u"Write notes"_s);
        writeCall.insert(u"kind"_s, u"write"_s);
        QJsonObject writeInput;
        writeInput.insert(u"filePath"_s, u"/tmp/notes.md"_s);
        writeInput.insert(u"contents"_s, u"hello"_s);
        writeCall.insert(u"rawInput"_s, writeInput);
        const PermissionRequest writeRequest = client.permissionFromToolCall(writeCall);
        QCOMPARE(writeRequest.path, u"/tmp/notes.md"_s);
        QVERIFY(writeRequest.mutatesFiles);
        QVERIFY(writeRequest.describeDiff.contains(u"+hello"_s));
    }

    void shellSummariesCoverOpenCodeAliases()
    {
        QJsonObject readArgs;
        readArgs.insert(u"filePath"_s, u"src/main.cpp"_s);
        QVERIFY(shellCommandFor(u"read"_s, readArgs).contains(u"src/main.cpp"_s));
        QJsonObject writeArgs;
        writeArgs.insert(u"filePath"_s, u"out.txt"_s);
        QVERIFY(shellCommandFor(u"write"_s, writeArgs).contains(u"out.txt"_s));
        QJsonObject editArgs;
        editArgs.insert(u"filePath"_s, u"a.cpp"_s);
        QVERIFY(shellCommandFor(u"edit"_s, editArgs).contains(u"a.cpp"_s));
        QVERIFY(shellCommandFor(u"patch"_s, editArgs).contains(u"a.cpp"_s));
        QJsonObject listArgs;
        listArgs.insert(u"path"_s, u"src"_s);
        QCOMPARE(shellCommandFor(u"list"_s, listArgs), u"ls src"_s);
        QJsonObject taskArgs;
        taskArgs.insert(u"description"_s, u"do it"_s);
        QVERIFY(shellCommandFor(u"task"_s, taskArgs).contains(u"do it"_s));
    }
};

QTEST_MAIN(TestOpenCodeCards)
#include "test_opencodecards.moc"
