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
#include "toolcallwidget.h"
#include "types.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
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
