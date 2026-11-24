/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Guards the transcript rebuild that runs when switching to a conversation
 * from history: the tool cards must come back the way the live turn showed
 * them -- right icon, right title, right rail colour, and the result box
 * filled with the tool's output instead of an empty card.
 */

#include "chatwidget.h"
#include "chattheme.h"
#include "sessionstore.h"
#include "sessionstoreisolation.h"
#include "subtaskwidget.h"
#include "todowidget.h"
#include "toolcallwidget.h"
#include "types.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

QJsonObject toolCall(const QString &id, const QString &name, const QJsonObject &args)
{
    QJsonObject function;
    function.insert(u"name"_s, name);
    function.insert(u"arguments"_s, QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)));
    QJsonObject call;
    call.insert(u"id"_s, id);
    call.insert(u"type"_s, u"function"_s);
    call.insert(u"function"_s, function);
    return call;
}

ChatMessage toolResultMessage(const QString &toolCallId, const QString &name, const QString &content)
{
    ChatMessage msg;
    msg.role = ChatMessage::Role::Tool;
    msg.toolCallId = toolCallId;
    msg.name = name;
    msg.content = content;
    return msg;
}

QWidget *cardForToolCallId(ChatWidget &widget, const QString &toolCallId)
{
    const auto cards = widget.findChildren<ToolCallWidget *>();
    for (auto *card : cards) {
        if (card->toolCallId() == toolCallId) {
            return card;
        }
    }
    return nullptr;
}

QString transcriptText(ChatWidget &widget)
{
    QString text;
    for (const QTextBrowser *browser : widget.findChildren<QTextBrowser *>()) {
        text += browser->toPlainText();
        text += u'\n';
    }
    for (const QLabel *label : widget.findChildren<QLabel *>()) {
        if (!label->text().isEmpty()) {
            text += label->text();
            text += u'\n';
        }
    }
    return text;
}

bool messagesContain(const QList<ChatMessage> &messages, const QString &needle)
{
    for (const ChatMessage &message : messages) {
        if (message.content.contains(needle)) {
            return true;
        }
    }
    return false;
}

QJsonObject planStep(const QString &id, const QString &description)
{
    QJsonObject step;
    step.insert(u"id"_s, id);
    step.insert(u"description"_s, description);
    step.insert(u"completed"_s, false);
    step.insert(u"inProgress"_s, true);
    return step;
}

} // namespace

class TestHistoryRestore : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testToolCardsSurviveAConversationSwitch()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"read main.cpp and list the directory"_s;

        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = QString();
        QJsonArray calls;
        calls.append(toolCall(u"call-read"_s, u"read_file"_s, {{u"path"_s, u"src/main.cpp"_s}}));
        calls.append(toolCall(u"call-bash"_s, u"bash"_s, {{u"command"_s, u"ls -la"_s}}));
        calls.append(toolCall(u"call-edit"_s, u"edit_file"_s,
                              {{u"path"_s, u"src/main.cpp"_s},
                               {u"old_string"_s, u"int a = 1;"_s},
                               {u"new_string"_s, u"int a = 2;"_s}}));
        assistant.toolCalls = calls;

        SessionStore::SessionData data;
        data.messages = {user, assistant,
                         toolResultMessage(u"call-read"_s, u"read_file"_s, u"int main() { return 0; }\n"_s),
                         toolResultMessage(u"call-bash"_s, u"bash"_s, u"total 0\nfile1\nfile2\n"_s),
                         toolResultMessage(u"call-edit"_s, u"edit_file"_s, u"Edit applied to src/main.cpp"_s)};
        SessionStore::saveConversation(u"conv-history"_s, data, u"History chat"_s, 50);

        // The widget starts on its own fresh conversation; switching to the
        // saved one is exactly what the history menu does.
        widget.setCurrentConversationId(SessionStore::createNewConversation());
        widget.switchToConversation(u"conv-history"_s);
        QTest::qWait(60);

        const auto cards = widget.findChildren<ToolCallWidget *>();
        QCOMPARE(cards.size(), 3);

        // --- read_file card -------------------------------------------------
        auto *readCard = qobject_cast<ToolCallWidget *>(cardForToolCallId(widget, u"call-read"_s));
        QVERIFY2(readCard, "read_file card missing after the switch");

        auto *readTitle = readCard->findChild<QLabel *>(u"toolTitle"_s);
        QVERIFY(readTitle);
        QVERIFY2(readTitle->text().contains(u"cat src/main.cpp"_s),
                 qPrintable(QStringLiteral("read_file title: %1").arg(readTitle->text())));

        auto *readIcon = readCard->findChild<QLabel *>(u"toolIcon"_s);
        QVERIFY(readIcon);
        QVERIFY2(!readIcon->text().isEmpty(), "read_file card has no icon after the switch");
        QVERIFY2(readIcon->styleSheet().contains(ChatTheme::toolColor(u"read_file"_s)),
                 qPrintable(readIcon->styleSheet()));
        QVERIFY2(readCard->styleSheet().contains(ChatTheme::toolColor(u"read_file"_s)),
                 qPrintable(readCard->styleSheet()));

        auto *readDetails = readCard->findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY(readDetails);
        QVERIFY2(readDetails->toPlainText().contains(u"int main() { return 0; }"_s),
                 qPrintable(QStringLiteral("read_file details: %1").arg(readDetails->toPlainText())));
        // The live card picks the highlighting language from the file extension;
        // the restored one must too, or the output comes back uncoloured.
        QVERIFY2(readDetails->toPlainText().contains(u"C++"_s),
                 qPrintable(QStringLiteral("read_file details language strip: %1").arg(readDetails->toPlainText())));
        QVERIFY2(!readCard->isRunning(), "restored card must be finished, not running");

        // --- bash card ------------------------------------------------------
        auto *bashCard = qobject_cast<ToolCallWidget *>(cardForToolCallId(widget, u"call-bash"_s));
        QVERIFY2(bashCard, "bash card missing after the switch");

        auto *bashTitle = bashCard->findChild<QLabel *>(u"toolTitle"_s);
        QVERIFY(bashTitle);
        QVERIFY2(bashTitle->text().contains(u"ls -la"_s),
                 qPrintable(QStringLiteral("bash title: %1").arg(bashTitle->text())));
        QVERIFY2(bashCard->styleSheet().contains(ChatTheme::toolColor(u"bash"_s)),
                 qPrintable(bashCard->styleSheet()));

        auto *bashDetails = bashCard->findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY(bashDetails);
        QVERIFY2(bashDetails->toPlainText().contains(u"file1"_s),
                 qPrintable(QStringLiteral("bash details: %1").arg(bashDetails->toPlainText())));

        // --- edit_file card -------------------------------------------------
        auto *editCard = qobject_cast<ToolCallWidget *>(cardForToolCallId(widget, u"call-edit"_s));
        QVERIFY2(editCard, "edit_file card missing after the switch");

        auto *editTitle = editCard->findChild<QLabel *>(u"toolTitle"_s);
        QVERIFY(editTitle);
        QVERIFY2(editTitle->text().contains(u"patch src/main.cpp"_s),
                 qPrintable(QStringLiteral("edit_file title: %1").arg(editTitle->text())));
        QVERIFY2(editCard->styleSheet().contains(ChatTheme::toolColor(u"edit_file"_s)),
                 qPrintable(editCard->styleSheet()));

        // The diff stat (+1 -1) only appears when a diff was attached; the
        // rebuild has to reconstruct it from the stored arguments.
        auto *diffStat = editCard->findChild<QLabel *>(u"toolDiffStat"_s);
        if (!diffStat) {
            // Fall back to scanning the header children: the stat label is the
            // only one whose text looks like "+n -n".
            const auto labels = editCard->findChildren<QLabel *>();
            for (auto *label : labels) {
                if (label->text().contains(u"+"_s) && label->text().contains(u"\u2212"_s)) {
                    diffStat = label;
                    break;
                }
            }
        }
        QVERIFY2(diffStat && diffStat->isVisible(),
                 "edit_file card lost its diff stat after the switch");

        auto *editDetails = editCard->findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY(editDetails);
        QVERIFY2(editDetails->toPlainText().contains(u"Edit applied to src/main.cpp"_s),
                 qPrintable(QStringLiteral("edit_file details: %1").arg(editDetails->toPlainText())));
    }

    // Closing and reopening the editor destroys and recreates the chat widget.
    // The transcript has to come back from the conversation the widget saved on
    // teardown, not from an empty agent state.
    void testConversationSurvivesAWidgetTeardownAndRestore()
    {
        const QString conversationId = u"conv-restart"_s;

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"inspect the parser"_s;

        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"The parser looks fine."_s;
        QJsonArray calls;
        calls.append(toolCall(u"restart-read"_s, u"read_file"_s, {{u"path"_s, u"src/parser.cpp"_s}}));
        assistant.toolCalls = calls;

        SessionStore::SessionData data;
        data.messages = {user, assistant,
                         toolResultMessage(u"restart-read"_s, u"read_file"_s, u"int parse() { return 0; }"_s)};

        {
            ChatWidget widget;
            widget.resize(900, 700);
            widget.show();
            widget.setCurrentConversationId(conversationId);
            widget.agent()->restoreSession(data);
            widget.rebuildTranscript();
            QTest::qWait(40);
        }

        const SessionStore::SessionData reloaded = SessionStore::loadConversation(conversationId);
        QVERIFY2(!reloaded.messages.isEmpty(), "the widget did not save its transcript on teardown");

        ChatWidget reopened;
        reopened.resize(900, 700);
        reopened.show();
        reopened.setCurrentConversationId(conversationId);
        reopened.agent()->restoreSession(reloaded);
        reopened.rebuildTranscript();
        QTest::qWait(60);

        QVERIFY2(transcriptText(reopened).contains(u"inspect the parser"_s),
                 qPrintable(transcriptText(reopened)));
        QVERIFY2(transcriptText(reopened).contains(u"The parser looks fine."_s),
                 qPrintable(transcriptText(reopened)));
        auto *card = qobject_cast<ToolCallWidget *>(cardForToolCallId(reopened, u"restart-read"_s));
        QVERIFY2(card, "the tool card was lost across the restart");
        auto *details = card->findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY(details);
        QVERIFY2(details->toPlainText().contains(u"int parse() { return 0; }"_s),
                 qPrintable(details->toPlainText()));
    }

    void testApplyPatchCardKeepsItsDiff()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"apply the patch"_s;

        const QString patch = u"--- a/new.txt\n+++ b/new.txt\n@@ -0,0 +1,2 @@\n+hello\n+world\n"_s;
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = QString();
        QJsonArray calls;
        calls.append(toolCall(u"call-patch"_s, u"apply_patch"_s, {{u"patch"_s, patch}}));
        assistant.toolCalls = calls;

        SessionStore::SessionData data;
        data.messages = {user, assistant,
                         toolResultMessage(u"call-patch"_s, u"apply_patch"_s, u"Applied patch to 1 file(s):\npatched new.txt"_s)};
        SessionStore::saveConversation(u"conv-patch"_s, data, u"Patch chat"_s, 50);

        widget.setCurrentConversationId(SessionStore::createNewConversation());
        widget.switchToConversation(u"conv-patch"_s);
        QTest::qWait(60);

        auto *card = qobject_cast<ToolCallWidget *>(cardForToolCallId(widget, u"call-patch"_s));
        QVERIFY2(card, "apply_patch card missing after the switch");
        auto *diff = card->findChild<QTextBrowser *>(u"toolDiff"_s);
        QVERIFY(diff);
        QVERIFY2(diff->toPlainText().contains(u"hello"_s),
                 qPrintable(QStringLiteral("apply_patch diff: %1").arg(diff->toPlainText())));
        QVERIFY2(card->styleSheet().contains(ChatTheme::toolColor(u"apply_patch"_s)),
                 qPrintable(card->styleSheet()));
    }

    // ACP agents report tool results as they finish, while the assistant
    // message that declares the tool calls is only appended when the whole
    // prompt ends. Stored ACP conversations therefore carry the Tool results
    // *before* the assistant message, and the rebuild must not drop them.
    void testAcpToolCardsSurviveAConversationSwitch()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"fix the build"_s;

        // ACP ordering: results first, the declaring assistant message last.
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"I fixed the build."_s;
        QJsonArray calls;
        calls.append(toolCall(u"acp-read"_s, u"fs/read_text_file"_s, {{u"path"_s, u"src/main.cpp"_s}}));
        calls.append(toolCall(u"acp-edit"_s, u"edit"_s,
                              {{u"path"_s, u"src/main.cpp"_s},
                               {u"old_string"_s, u"int a = 1;"_s},
                               {u"new_string"_s, u"int a = 2;"_s}}));
        calls.append(toolCall(u"acp-term"_s, u"terminal/exec"_s, {{u"command"_s, u"make -j8"_s}}));
        assistant.toolCalls = calls;

        SessionStore::SessionData data;
        data.messages = {user,
                         toolResultMessage(u"acp-read"_s, u"fs/read_text_file"_s, u"int main() { return 0; }\n"_s),
                         toolResultMessage(u"acp-edit"_s, u"edit"_s, u"1 line changed"_s),
                         toolResultMessage(u"acp-term"_s, u"terminal/exec"_s, u"[100%] Built target app\n"_s),
                         assistant};
        SessionStore::saveConversation(u"conv-acp"_s, data, u"ACP chat"_s, 50);

        widget.setCurrentConversationId(SessionStore::createNewConversation());
        widget.switchToConversation(u"conv-acp"_s);
        QTest::qWait(60);

        const auto cards = widget.findChildren<ToolCallWidget *>();
        QCOMPARE(cards.size(), 3);

        auto *readCard = qobject_cast<ToolCallWidget *>(cardForToolCallId(widget, u"acp-read"_s));
        QVERIFY2(readCard, "fs/read_text_file card missing after the switch");
        auto *readDetails = readCard->findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY(readDetails);
        QVERIFY2(readDetails->toPlainText().contains(u"int main() { return 0; }"_s),
                 qPrintable(QStringLiteral("fs/read_text_file details: %1").arg(readDetails->toPlainText())));
        QVERIFY2(readDetails->toPlainText().contains(u"C++"_s),
                 qPrintable(QStringLiteral("fs/read_text_file details language strip: %1").arg(readDetails->toPlainText())));
        QVERIFY2(readCard->styleSheet().contains(ChatTheme::toolColor(u"fs/read_text_file"_s)),
                 qPrintable(readCard->styleSheet()));

        auto *editCard = qobject_cast<ToolCallWidget *>(cardForToolCallId(widget, u"acp-edit"_s));
        QVERIFY2(editCard, "edit card missing after the switch");
        auto *editDetails = editCard->findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY(editDetails);
        QVERIFY2(editDetails->toPlainText().contains(u"1 line changed"_s),
                 qPrintable(QStringLiteral("edit details: %1").arg(editDetails->toPlainText())));
        // An ACP edit is a mutation: the card must not come back as a quiet
        // read (green rail fallback) but keep its write/edit colouring.
        QVERIFY2(editCard->styleSheet().contains(ChatTheme::warning())
                     || editCard->styleSheet().contains(ChatTheme::toolColor(u"edit"_s)),
                 qPrintable(editCard->styleSheet()));

        auto *termCard = qobject_cast<ToolCallWidget *>(cardForToolCallId(widget, u"acp-term"_s));
        QVERIFY2(termCard, "terminal/exec card missing after the switch");
        auto *termDetails = termCard->findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY(termDetails);
        QVERIFY2(termDetails->toPlainText().contains(u"Built target app"_s),
                 qPrintable(QStringLiteral("terminal/exec details: %1").arg(termDetails->toPlainText())));
        QVERIFY2(termCard->styleSheet().contains(ChatTheme::toolColor(u"terminal/exec"_s)),
                 qPrintable(termCard->styleSheet()));
    }

    // A reconstructed transcript must put sub-agents that already reported
    // back into the thread. Recreating every one of them as a docked card left
    // old, finished sub-agents pinned above the composer and the intent dock
    // on screen for a conversation where nothing was running.
    void testFinishedSubAgentCardsRestoreIntoTheTranscript()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"delegate the parser work"_s;

        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"Both agents reported back."_s;
        QJsonArray calls;
        calls.append(toolCall(u"call-sub"_s, u"new_task"_s,
                              {{u"description"_s, u"Review the parser"_s}, {u"agent"_s, u"reviewer"_s}}));
        assistant.toolCalls = calls;

        SessionStore::SessionData data;
        data.messages = {user, assistant,
                         toolResultMessage(u"call-sub"_s, u"new_task"_s,
                                           u"The parser looks correct."_s)};
        SessionStore::saveConversation(u"conv-subtask"_s, data, u"Delegation chat"_s, 50);

        widget.setCurrentConversationId(SessionStore::createNewConversation());
        widget.switchToConversation(u"conv-subtask"_s);
        QTest::qWait(60);

        const auto agents = widget.findChildren<SubtaskWidget *>();
        QCOMPARE(agents.size(), 1);
        SubtaskWidget *agent = agents.first();
        QVERIFY2(!agent->isRunning(), "a restored sub-agent card was recreated as running");

        bool showsResult = false;
        for (const QTextBrowser *browser : agent->findChildren<QTextBrowser *>()) {
            if (browser->isVisibleTo(agent)
                && browser->toPlainText().contains(u"The parser looks correct."_s)) {
                showsResult = true;
            }
        }
        QVERIFY2(showsResult, "the restored sub-agent card lost its result");

        QWidget *dock = widget.findChild<QWidget *>(u"intentDock"_s);
        QVERIFY(dock);
        bool insideDock = false;
        for (QWidget *parent = agent->parentWidget(); parent; parent = parent->parentWidget()) {
            if (parent == dock) {
                insideDock = true;
                break;
            }
        }
        QVERIFY2(!insideDock, "a finished sub-agent was restored into the intent dock");
        QVERIFY2(agent->parentWidget() && agent->parentWidget()->layout()
                     && agent->parentWidget()->layout()->indexOf(agent) >= 0,
                 "the restored sub-agent never made it into the transcript");
        QVERIFY2(!dock->isVisible(), "the intent dock is on screen with no sub-agent running");
    }

    // A saved transcript can carry a delegation whose result was never
    // persisted: Kate closed mid-run, or the call survives only in the display
    // transcript after the model history was compacted. A reload has no child
    // loop behind it, so recreating it in the intent dock left the dock on
    // screen at launch with a card that could never finish.
    void testSubAgentWithoutALiveRunRestoresIntoTheTranscript()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"delegate and close Kate"_s;

        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = QString();
        QJsonArray calls;
        calls.append(toolCall(u"call-orphan"_s, u"new_task"_s,
                              {{u"description"_s, u"Review the parser"_s}, {u"agent"_s, u"reviewer"_s}}));
        assistant.toolCalls = calls;

        // The model history was compacted past the call, so restoreSession()
        // has nothing to repair; only the display transcript remembers it.
        ChatMessage summary;
        summary.role = ChatMessage::Role::User;
        summary.name = u"context_summary"_s;
        summary.content = u"[Earlier conversation was compacted.]"_s;

        SessionStore::SessionData data;
        data.messages = {summary};
        data.transcript = {user, assistant};
        SessionStore::saveConversation(u"conv-orphan-sub"_s, data, u"Orphan delegation"_s, 50);

        widget.setCurrentConversationId(SessionStore::createNewConversation());
        widget.switchToConversation(u"conv-orphan-sub"_s);
        QTest::qWait(60);

        const auto agents = widget.findChildren<SubtaskWidget *>();
        QCOMPARE(agents.size(), 1);
        SubtaskWidget *agent = agents.first();
        QVERIFY2(!agent->isRunning(), "a sub-agent with no live run was restored as running");

        QWidget *dock = widget.findChild<QWidget *>(u"intentDock"_s);
        QVERIFY(dock);
        QVERIFY2(dock->isHidden(), "the intent dock opened for a sub-agent that is not running");

        bool insideDock = false;
        for (QWidget *parent = agent->parentWidget(); parent; parent = parent->parentWidget()) {
            if (parent == dock) {
                insideDock = true;
                break;
            }
        }
        QVERIFY2(!insideDock, "an orphaned sub-agent was docked above the composer");
        QVERIFY2(agent->parentWidget() && agent->parentWidget()->layout()
                     && agent->parentWidget()->layout()->indexOf(agent) >= 0,
                 "the orphaned sub-agent never made it into the transcript");
    }

    void testARestoredAnswerIsNotShownTwice()
    {
        // The snapshot of an unfinished turn holds the text of the answer, and
        // the transcript may already carry the same answer. Rendering both
        // shows the final answer twice.
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        SessionStore::SessionData data;
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"say hello"_s;
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"Hello there"_s;
        data.messages = {user, assistant};
        // The same text, as a session that was saved while the answer was
        // still streaming would hold it.
        data.currentAssistant = u"Hello there"_s;
        SessionStore::saveConversation(u"conv-duplicate"_s, data, u"Duplicate"_s, 50);

        widget.setCurrentConversationId(SessionStore::createNewConversation());
        widget.switchToConversation(u"conv-duplicate"_s);
        QTest::qWait(60);

        int answers = 0;
        for (const QTextBrowser *browser : widget.findChildren<QTextBrowser *>()) {
            if (browser->toPlainText().contains(u"Hello there"_s)) {
                ++answers;
            }
        }
        QCOMPARE(answers, 1);
    }

    // Providers and sub-agents reuse ids ("call_0", a retried delegation).
    // Keying restored cards by id alone collapsed several calls into one card
    // and kept only the last output, so the history looked like work vanished.
    void testRepeatedToolCallIdsRestoreAsSeparateCards()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"read and list"_s;

        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = QString();
        QJsonArray calls;
        calls.append(toolCall(u"call-0"_s, u"read_file"_s, {{u"path"_s, u"src/parser.cpp"_s}}));
        calls.append(toolCall(u"call-0"_s, u"bash"_s, {{u"command"_s, u"ls -la"_s}}));
        assistant.toolCalls = calls;

        SessionStore::SessionData data;
        data.messages = {user, assistant,
                         toolResultMessage(u"call-0"_s, u"read_file"_s, u"parser contents"_s),
                         toolResultMessage(u"call-0"_s, u"bash"_s, u"total 0\nfile1\n"_s)};
        SessionStore::saveConversation(u"conv-repeat-id"_s, data, u"Repeated ids"_s, 50);

        widget.setCurrentConversationId(SessionStore::createNewConversation());
        widget.switchToConversation(u"conv-repeat-id"_s);
        QTest::qWait(60);

        const auto cards = widget.findChildren<ToolCallWidget *>();
        QCOMPARE(cards.size(), 2);

        QStringList titles;
        QStringList details;
        for (const ToolCallWidget *card : cards) {
            if (const auto *title = card->findChild<QLabel *>(u"toolTitle"_s)) {
                titles << title->text();
            }
            if (const auto *body = card->findChild<QTextBrowser *>(u"toolDetails"_s)) {
                details << body->toPlainText();
            }
        }
        const QString allTitles = titles.join(u'\n');
        const QString allDetails = details.join(u'\n');
        QVERIFY2(allTitles.contains(u"src/parser.cpp"_s), qPrintable(allTitles));
        QVERIFY2(allTitles.contains(u"ls -la"_s), qPrintable(allTitles));
        QVERIFY2(allDetails.contains(u"parser contents"_s), qPrintable(allDetails));
        QVERIFY2(allDetails.contains(u"file1"_s), qPrintable(allDetails));
    }

    // A turn saved while a sub-agent was still running gets a synthetic
    // cancelled result on restore. That is not a failure: the card has to say
    // "Cancelled" and use the muted state, not paint a red error over a run the
    // user deliberately stopped.
    void testCancelledSubAgentRestoresAsCancelled()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"delegate and stop"_s;

        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = QString();
        QJsonArray calls;
        calls.append(toolCall(u"call-stop"_s, u"new_task"_s,
                              {{u"description"_s, u"Review the parser"_s}, {u"agent"_s, u"reviewer"_s}}));
        assistant.toolCalls = calls;

        SessionStore::SessionData data;
        data.messages = {user, assistant,
                         toolResultMessage(u"call-stop"_s, u"new_task"_s,
                                           u"The tool call was cancelled before it ran."_s)};
        SessionStore::saveConversation(u"conv-cancelled-sub"_s, data, u"Cancelled delegation"_s, 50);

        widget.setCurrentConversationId(SessionStore::createNewConversation());
        widget.switchToConversation(u"conv-cancelled-sub"_s);
        QTest::qWait(60);

        const auto agents = widget.findChildren<SubtaskWidget *>();
        QCOMPARE(agents.size(), 1);
        SubtaskWidget *agent = agents.first();
        QVERIFY2(!agent->isRunning(), "a cancelled restored sub-agent must not spin");
        auto *status = agent->findChild<QLabel *>(u"subtaskStatus"_s);
        QVERIFY(status);
        QVERIFY2(status->text().contains(u"Cancelled"_s), qPrintable(status->text()));
        auto *caption = agent->findChild<QLabel *>(u"subtaskCaption"_s);
        QVERIFY(caption);
        QVERIFY2(caption->text() == u"Cancelled"_s, qPrintable(caption->text()));
    }

    void testPlanDockResetsWithTheConversation()
    {
        // The plan dock lives above the composer, outside the transcript sweep
        // a rebuild runs. The checklist of the conversation being left must
        // not follow the user into a new one, and a conversation from history
        // that carries a plan must get its own checklist back.
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();
        QTest::qWait(60);

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"do the work"_s;
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"Working on it."_s;

        SessionStore::SessionData plain;
        plain.messages = {user, assistant};
        SessionStore::saveConversation(u"conv-plain"_s, plain, u"Plain"_s, 50);

        QJsonArray plan;
        plan.append(planStep(u"1"_s, u"survey the code"_s));
        plan.append(planStep(u"2"_s, u"apply the fix"_s));
        SessionStore::SessionData withPlan = plain;
        withPlan.currentPlan = plan;
        withPlan.planShown = true;
        SessionStore::saveConversation(u"conv-with-plan"_s, withPlan, u"With plan"_s, 50);

        widget.setCurrentConversationId(SessionStore::createNewConversation());

        auto *todo = widget.findChild<TodoWidget *>();
        QVERIFY(todo);
        QVERIFY2(!todo->isVisible(), "dock must start hidden on a fresh conversation");

        // A running turn puts its plan in the dock.
        todo->setTodos(plan);
        QVERIFY(todo->isVisible());

        // Starting a new conversation drops it again.
        widget.newChat();
        QVERIFY2(!todo->isVisible(), "plan dock survived starting a new conversation");

        // So does switching to a conversation that has no plan of its own.
        todo->setTodos(plan);
        QVERIFY(todo->isVisible());
        widget.switchToConversation(u"conv-plain"_s);
        QTest::qWait(60);
        QVERIFY2(!todo->isVisible(), "plan dock survived switching to a plan-less conversation");

        // A conversation saved with a plan shows its own checklist again.
        widget.switchToConversation(u"conv-with-plan"_s);
        QTest::qWait(60);
        // showTodoList() honours the "show plan as checklist" setting, so the
        // dock is back exactly when that setting says it should be.
        QCOMPARE(todo->isVisible(), widget.settings().showPlanAsChecklist);
    }

    void testRestoreRendersTheWholeHistoryWhenTheContextIsFull()
    {
        // A restored conversation can already be at its context limit, and the
        // readout update that follows folds it for the next request. That used
        // to run while the transcript was still being rebuilt: the rebuild
        // loop continued inside the compacted history and stopped early, so
        // everything after the first user card -- including the newest answer
        // the user came back to read -- never got a card.
        SessionStore::SessionData data;
        const QString filler = QStringLiteral("tool output line. ").repeated(30);

        for (int i = 0; i < 60; ++i) {
            ChatMessage tool;
            tool.role = ChatMessage::Role::Tool;
            tool.toolCallId = u"pre-%1"_s.arg(i);
            tool.name = u"read_file"_s;
            tool.content = filler;
            data.messages.append(tool);
        }

        ChatMessage firstUser;
        firstUser.role = ChatMessage::Role::User;
        firstUser.content = u"first question"_s;
        data.messages.append(firstUser);

        for (int i = 0; i < 40; ++i) {
            ChatMessage tool;
            tool.role = ChatMessage::Role::Tool;
            tool.toolCallId = u"mid-%1"_s.arg(i);
            tool.name = u"read_file"_s;
            tool.content = filler;
            data.messages.append(tool);
        }

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"final question"_s;
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"THE FINAL ANSWER"_s;
        data.messages.append(user);
        data.messages.append(assistant);

        SessionStore::saveConversation(u"conv-full-context"_s, data, u"Full context"_s, 50);

        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();
        Settings settings = widget.settings();
        settings.contextWindow = 1000; // the restored history is far over it
        widget.setSettings(settings);

        widget.setCurrentConversationId(u"conv-full-context"_s);
        widget.agent()->restoreSession(SessionStore::loadConversation(u"conv-full-context"_s));
        widget.rebuildTranscript();
        QTest::qWait(80);

        const QString transcript = transcriptText(widget);
        QVERIFY2(transcript.contains(u"THE FINAL ANSWER"_s),
                 "the newest answer is missing from a restored conversation at its context limit");
        QVERIFY2(transcript.contains(u"first question"_s),
                 "the start of the restored conversation was dropped");
        // Every restored call keeps its card: the rebuild may not be cut short
        // by the compaction that the readout triggers.
        QCOMPARE(widget.findChildren<ToolCallWidget *>().size(), 100);
    }

    void testReopeningAfterCompactionKeepsTheWholeHistory()
    {
        SessionStore::SessionData data;
        const QString filler = QStringLiteral("tool output line. ").repeated(30);

        for (int i = 0; i < 60; ++i) {
            ChatMessage tool;
            tool.role = ChatMessage::Role::Tool;
            tool.toolCallId = u"pre-%1"_s.arg(i);
            tool.name = u"read_file"_s;
            tool.content = filler;
            data.messages.append(tool);
        }

        ChatMessage firstUser;
        firstUser.role = ChatMessage::Role::User;
        firstUser.content = u"first question"_s;
        data.messages.append(firstUser);

        for (int i = 0; i < 40; ++i) {
            ChatMessage tool;
            tool.role = ChatMessage::Role::Tool;
            tool.toolCallId = u"mid-%1"_s.arg(i);
            tool.name = u"read_file"_s;
            tool.content = filler;
            data.messages.append(tool);
        }

        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"final question"_s;
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"THE FINAL ANSWER"_s;
        data.messages.append(user);
        data.messages.append(assistant);

        SessionStore::saveConversation(u"conv-two-reopens"_s, data, u"Two reopens"_s, 50);

        {
            ChatWidget widget;
            widget.resize(900, 700);
            widget.show();
            Settings settings = widget.settings();
            settings.contextWindow = 1000;
            widget.setSettings(settings);
            widget.setCurrentConversationId(u"conv-two-reopens"_s);
            widget.agent()->restoreSession(SessionStore::loadConversation(u"conv-two-reopens"_s));
            widget.rebuildTranscript();
            QTest::qWait(80);
        }

        const SessionStore::SessionData savedAfterFirstOpen =
            SessionStore::loadConversation(u"conv-two-reopens"_s);
        // The test has to keep exercising the compaction: the model history
        // shrinks, while the transcript that gets persisted stays complete.
        QVERIFY2(savedAfterFirstOpen.messages.size() < data.messages.size(),
                 "the first reopen did not compact the model history");
        QCOMPARE(savedAfterFirstOpen.transcript.size(), data.messages.size());
        QVERIFY2(messagesContain(savedAfterFirstOpen.transcript, u"first question"_s),
                 "the first reopen's save dropped the start of the history");

        ChatWidget reopened;
        reopened.resize(900, 700);
        reopened.show();
        Settings settings = reopened.settings();
        settings.contextWindow = 1000;
        reopened.setSettings(settings);
        reopened.setCurrentConversationId(u"conv-two-reopens"_s);
        reopened.agent()->restoreSession(savedAfterFirstOpen);
        reopened.rebuildTranscript();
        QTest::qWait(80);

        const QString transcript = transcriptText(reopened);
        QVERIFY2(transcript.contains(u"THE FINAL ANSWER"_s), qPrintable(transcript));
        QVERIFY2(transcript.contains(u"first question"_s), qPrintable(transcript));
        QCOMPARE(reopened.findChildren<ToolCallWidget *>().size(), 100);
    }
};

// Explicit main rather than QTEST_MAIN: the widget built here autosaves its
// transcript into SessionStore while it is destroyed, and the store override
// has to be in place before the first widget exists.
int main(int argc, char **argv)
{
    isolateSessionStore();

    QApplication app(argc, argv);
    TestHistoryRestore tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_historyrestore.moc"
