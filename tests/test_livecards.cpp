/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Guards the content a card carries while it executes.
 *
 * Every card used to be a header row for as long as its work took: a tool that
 * ran for thirty seconds showed its name and a timer and nothing else, a
 * suggestion appeared only once it had already posted, and a question sat in a
 * bare row above a separate card that held the text. A card that is executing
 * now says what it is doing -- the tool's arguments, a live stream, the diff,
 * the question, the advice -- from the moment it appears.
 *
 * The cards are driven through AgentLoop's own signals, which is the path the
 * widget takes from a real turn.
 */

#include "agentloop.h"
#include "chattheme.h"
#include "chatwidget.h"
#include "settings.h"
#include "toolcallwidget.h"
#include "questionwidget.h"
#include "suggestionwidget.h"
#include "types.h"

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextFragment>
#include <QVBoxLayout>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestLiveCards : public QObject
{
    Q_OBJECT

private:
    ChatWidget *m_widget = nullptr;
    std::unique_ptr<QTemporaryDir> m_workspace;

    PermissionRequest request(const QString &toolName, const QString &id, const QString &details) const
    {
        PermissionRequest req;
        req.toolName = toolName;
        req.toolCallId = id;
        req.summary = details;
        req.details = details;
        req.risk = ToolRisk::Read;
        return req;
    }

    ToolResult result(const QString &toolName, const QString &id, const QString &output) const
    {
        ToolResult out;
        out.toolCallId = id;
        out.name = toolName;
        out.ok = true;
        out.output = output;
        return out;
    }

    // The single body a card expands into, named so it can be found without
    // reaching into the widget's private members.
    QTextBrowser *cardBody(QWidget *card) const
    {
        return card->findChild<QTextBrowser *>(QStringLiteral("toolDetails"));
    }

    template<typename Widget>
    Widget *cardOfClass(const QString &objectName) const
    {
        const auto cards = m_widget->findChildren<Widget *>();
        for (Widget *card : cards) {
            if (card->objectName() == objectName) {
                return card;
            }
        }
        return nullptr;
    }

    SuggestionWidget *suggestionCard(const QString &id) const
    {
        const auto cards = m_widget->findChildren<SuggestionWidget *>();
        for (SuggestionWidget *card : cards) {
            if (card->suggestionId() == id) {
                return card;
            }
        }
        return nullptr;
    }

    QuestionWidget *questionCard(const QString &id) const
    {
        const auto cards = m_widget->findChildren<QuestionWidget *>();
        for (QuestionWidget *card : cards) {
            if (card->questionCallId() == id) {
                return card;
            }
        }
        return nullptr;
    }

    QJsonObject functionCall(const QString &id, const QString &name, const QString &arguments) const
    {
        QJsonObject fn;
        fn.insert(QStringLiteral("name"), name);
        fn.insert(QStringLiteral("arguments"), arguments);
        QJsonObject call;
        call.insert(QStringLiteral("id"), id);
        call.insert(QStringLiteral("type"), QStringLiteral("function"));
        call.insert(QStringLiteral("function"), fn);
        return call;
    }

    int mappedTop(QWidget *widget) const
    {
        return widget->mapTo(m_widget, QPoint(0, 0)).y();
    }

    void setup()
    {
        m_workspace = std::make_unique<QTemporaryDir>();
        QVERIFY(m_workspace->isValid());

        m_widget = new ChatWidget;
        m_widget->resize(900, 700);
        m_widget->show();

        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = u"http://127.0.0.1:1/v1"_s;
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.sandbox = SandboxProfile::Off;
        m_widget->setSettings(settings);
        m_widget->agent()->setWorkspace(m_workspace->path());
    }

    void teardown()
    {
        delete m_widget;
        m_widget = nullptr;
        m_workspace.reset();
    }

private Q_SLOTS:
    void initTestCase()
    {
    }

    void cleanup()
    {
        teardown();
    }

    // A read is the plainest card there is: no output to stream, no change to
    // show. While it runs it used to be a header row and nothing else.
    void testRunningToolCardShowsWhatItIsWorkingOn()
    {
        setup();
        const QString args = QStringLiteral("{\n    \"path\": \"src/agentloop.cpp\",\n    \"start_line\": 100\n}");
        Q_EMIT m_widget->agent()->toolStarted(request(u"read_file"_s, u"tc-read"_s, args));
        QTest::qWait(80);

        ToolCallWidget *card = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_tc-read"));
        QVERIFY2(card, "no card was created for the tool call");
        QVERIFY2(card->isRunning(), "the card should be running");
        QTextBrowser *body = cardBody(card);
        QVERIFY2(body, "the card has no body surface");
        QVERIFY2(body->toPlainText().contains(QStringLiteral("src/agentloop.cpp")),
                 qPrintable(body->toPlainText()));
        QVERIFY2(body->height() > 0, qPrintable(QStringLiteral("body has no height while the tool runs")));
        // A header-only card is ~40px; anything carrying content is taller.
        QVERIFY2(card->height() > 60, qPrintable(QStringLiteral("card is header-only at %1px").arg(card->height())));

        // The result replaces what was shown while it ran.
        Q_EMIT m_widget->agent()->toolFinished(result(u"read_file"_s, u"tc-read"_s, u"100: int main() {}"_s));
        QTest::qWait(80);
        QVERIFY2(!card->isRunning(), "the card never finished");
        QVERIFY2(body->toPlainText().contains(QStringLiteral("int main()")), qPrintable(body->toPlainText()));
        QVERIFY2(!body->toPlainText().contains(QStringLiteral("start_line")), qPrintable(body->toPlainText()));
    }

    // A command already streams its output; the arguments must not sit on top of
    // that and bury the thing worth reading.
    void testStreamOutranksTheArguments()
    {
        setup();
        Q_EMIT m_widget->agent()->toolStarted(request(u"bash"_s, u"tc-bash"_s, u"{\n    \"command\": \"make -j8\"\n}"_s));
        Q_EMIT m_widget->agent()->toolProgress(u"tc-bash"_s, u"[ 50%] Building CXX object\n"_s);
        // The progress signal is coalesced so a chatty command does not repaint
        // per chunk; let it land.
        QTest::qWait(300);

        ToolCallWidget *card = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_tc-bash"));
        QVERIFY2(card, "no card was created for the command");
        QTextBrowser *body = cardBody(card);
        QVERIFY2(body->toPlainText().contains(QStringLiteral("Building")), qPrintable(body->toPlainText()));
        QVERIFY2(!body->toPlainText().contains(QStringLiteral("command")), qPrintable(body->toPlainText()));
    }

    // A question is the one tool whose payload is prose rather than arguments,
    // and the user has to read it before the turn can go on.
    void testQuestionCardShowsTheQuestionWhileItIsAsked()
    {
        setup();
        PermissionRequest req = request(u"question"_s, u"tc-question"_s, u"{}"_s);
        req.questionText = u"Rewrite the file, or patch it in place?"_s;
        req.questionOptions = QStringList({u"Rewrite"_s, u"Patch"_s});
        req.allowFreeform = true;
        Q_EMIT m_widget->agent()->toolStarted(req);
        Q_EMIT m_widget->agent()->questionAsked(req);
        QTest::qWait(80);

        // The card in the transcript carries the question, not an empty row.
        ToolCallWidget *card = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_tc-question"));
        QVERIFY2(card, "no card was created for the question");
        QTextBrowser *body = cardBody(card);
        QVERIFY2(body, "the question card has no body surface");
        QVERIFY2(body->toPlainText().contains(QStringLiteral("patch it in place")), qPrintable(body->toPlainText()));

        // And the prompt in the dock above the input does too.
        QuestionWidget *prompt = questionCard(u"tc-question"_s);
        QVERIFY2(prompt, "no question prompt in the intent dock");
        QVERIFY2(prompt->height() > 40, qPrintable(QStringLiteral("the prompt is header-only at %1px").arg(prompt->height())));
    }

    // A suggestion is advice the user is meant to read, and it is fully written
    // by the time the tool is called: waiting for the result before showing it
    // meant the card appeared with nothing in it, or not at all.
    void testSuggestionIsPostedWhileItsToolRuns()
    {
        setup();
        PermissionRequest req = request(u"suggest"_s, u"tc-suggest"_s, QString());
        req.details = QStringLiteral("{\n    \"suggestion\": \"Use std::string_view here\",\n    \"path\": \"src/main.cpp\",\n    \"line\": 42\n}");
        Q_EMIT m_widget->agent()->toolStarted(req);
        QTest::qWait(80);

        SuggestionWidget *card = suggestionCard(u"tc-suggest"_s);
        QVERIFY2(card, "no suggestion card was posted while the tool ran");
        const QList<QTextBrowser *> bodies = card->findChildren<QTextBrowser *>();
        QVERIFY2(!bodies.isEmpty(), "the posted card has no body");
        QVERIFY2(bodies.first()->toPlainText().contains(QStringLiteral("std::string_view")),
                 qPrintable(bodies.first()->toPlainText()));

        // The result settles the card instead of posting a second one.
        Q_EMIT m_widget->agent()->toolFinished(result(u"suggest"_s, u"tc-suggest"_s, u"ok"_s));
        QTest::qWait(80);
        QCOMPARE(m_widget->findChildren<SuggestionWidget *>().size(), 1);

        // A failure lands on the same card rather than advising the user on
        // something that never arrived.
        PermissionRequest failing = request(u"suggest"_s, u"tc-fail"_s, QString());
        failing.details = QStringLiteral("{\n    \"suggestion\": \"Add a test\",\n    \"path\": \"src/x.cpp\"\n}");
        Q_EMIT m_widget->agent()->toolStarted(failing);
        QTest::qWait(80);
        ToolResult failed;
        failed.toolCallId = u"tc-fail"_s;
        failed.name = u"suggest"_s;
        failed.ok = false;
        failed.output = u"suggestion must not be empty"_s;
        Q_EMIT m_widget->agent()->toolFinished(failed);
        QTest::qWait(80);
        SuggestionWidget *failedCard = suggestionCard(u"tc-fail"_s);
        QVERIFY2(failedCard, "the failing call produced no card");
        const QList<QTextBrowser *> failedBodies = failedCard->findChildren<QTextBrowser *>();
        QVERIFY2(!failedBodies.isEmpty(), "the failed card has no body");
        QVERIFY2(failedBodies.first()->toPlainText().contains(QStringLiteral("must not be empty")),
                 qPrintable(failedBodies.first()->toPlainText()));
        QCOMPARE(m_widget->findChildren<SuggestionWidget *>().size(), 2);
    }

    // The user's own turn is markup too. It used to be escaped text run through
    // a QLabel, so a pasted diff lost its fences, `grep` lost its backticks and
    // **bold** stayed literal -- the same text rendering as markdown one card
    // further up the conversation.
    void testUserMessageRendersMarkdown()
    {
        setup();
        Q_EMIT m_widget->agent()->userMessage(u"Compare **this** with `grep`:\n\n```cpp\nint x = 1;\n```\n"_s);
        QTest::qWait(80);

        QTextBrowser *body = m_widget->findChild<QTextBrowser *>(QStringLiteral("userMessageBody"));
        QVERIFY2(body, "the user card has no markup body");
        const QString plain = body->toPlainText();
        QVERIFY2(plain.contains(QStringLiteral("this")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("**")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("grep")), qPrintable(plain));
        // A fenced block survives as a block, in order, behind the prose.
        QVERIFY2(plain.contains(QStringLiteral("int x = 1;")), qPrintable(plain));
        QVERIFY2(plain.indexOf(QStringLiteral("Compare")) < plain.indexOf(QStringLiteral("int x = 1;")),
                 qPrintable(plain));
        QVERIFY2(body->height() > 20, qPrintable(QStringLiteral("body collapsed to %1px").arg(body->height())));

        // The markup parsed rather than being dropped: **this** really is bold
        // and `grep` really is monospace, which is the whole point of rendering
        // the prompt instead of escaping it.
        bool sawBold = false;
        bool sawCodeFont = false;
        for (QTextBlock b = body->document()->begin(); b.isValid(); b = b.next()) {
            for (QTextBlock::Iterator it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment frag = it.fragment();
                const QString text = frag.text();
                if (text.startsWith(QStringLiteral("this")) && frag.charFormat().fontWeight() >= QFont::Bold) {
                    sawBold = true;
                }
                if (text.startsWith(QStringLiteral("grep")) && frag.charFormat().fontFamily() == QStringLiteral("monospace")) {
                    sawCodeFont = true;
                }
            }
        }
        QVERIFY2(sawBold, "bold text did not render as bold");
        QVERIFY2(sawCodeFont, "inline code did not render in a monospace font");
    }

    // A long prompt must wrap and grow the card instead of being clipped by a
    // fixed-height browser.
    void testLongUserMessageGrowsTheCard()
    {
        setup();
        const QString longText = QStringLiteral("line of prompt with **bold** and a fence\n").repeated(24)
            + QStringLiteral("\n```text\nblock\n```\n");
        Q_EMIT m_widget->agent()->userMessage(longText);
        QTest::qWait(120);

        QTextBrowser *body = m_widget->findChild<QTextBrowser *>(QStringLiteral("userMessageBody"));
        QVERIFY2(body, "the user card has no markup body");
        const int viewport = m_widget->width();
        QVERIFY2(body->document()->size().width() <= viewport,
                 qPrintable(QStringLiteral("document is %1px wide in a %2px panel")
                                .arg(body->document()->size().width()).arg(viewport)));
        QVERIFY2(body->height() > 100, qPrintable(QStringLiteral("body is only %1px tall").arg(body->height())));
        const QString plain = body->toPlainText();
        QVERIFY2(plain.count(QStringLiteral("line of prompt")) == 24, qPrintable(QStringLiteral("only %1 lines arrived").arg(plain.count(QStringLiteral("line of prompt")))));
    }

    // A prompt that only looks like markup -- a bare asterisk, a lone fence, a
    // backtick -- must still read as what was typed rather than being eaten by
    // the parser.
    void testUserMessageKeepsTextThatOnlyLooksLikeMarkup()
    {
        setup();
        Q_EMIT m_widget->agent()->userMessage(u"2 * 3 is 6, not a list\n\n```\nfence with no language\n"_s);
        QTest::qWait(80);

        QTextBrowser *body = m_widget->findChild<QTextBrowser *>(QStringLiteral("userMessageBody"));
        QVERIFY2(body, "the user card has no markup body");
        const QString plain = body->toPlainText();
        QVERIFY2(plain.contains(QStringLiteral("2 * 3 is 6, not a list")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("fence with no language")), qPrintable(plain));
    }

    // A one-line prompt used to take the QTextEdit size hint (~192px) because
    // nothing ever fitted the browser to its document.
    void testShortUserMessageHugsItsText()
    {
        setup();
        Q_EMIT m_widget->agent()->userMessage(u"hello"_s);
        QTest::qWait(80);

        QTextBrowser *body = m_widget->findChild<QTextBrowser *>(QStringLiteral("userMessageBody"));
        QVERIFY2(body, "the user card has no markup body");
        QVERIFY2(body->toPlainText().contains(QStringLiteral("hello")), qPrintable(body->toPlainText()));
        const int line = std::max(1, QFontMetrics(body->font()).lineSpacing());
        QVERIFY2(body->height() > line,
                 qPrintable(QStringLiteral("body is %1px, one line is %2px").arg(body->height()).arg(line)));
        QVERIFY2(body->height() < line * 4,
                 qPrintable(QStringLiteral("body is %1px, four lines would be %2px").arg(body->height()).arg(line * 4)));
    }

    // Progress used to restart a 200ms single-shot timer on every chunk, so a
    // command that never paused painted nothing until it exited.
    void testToolProgressPaintsWhileTheCommandIsStillRunning()
    {
        setup();
        Q_EMIT m_widget->agent()->toolStarted(request(u"bash"_s, u"tc-stream"_s, u"{\n    \"command\": \"make\"\n}"_s));
        QString accumulated;
        for (int i = 0; i < 30; ++i) {
            accumulated += QStringLiteral("line %1 of the build\n").arg(i);
            Q_EMIT m_widget->agent()->toolProgress(u"tc-stream"_s, accumulated);
            QTest::qWait(20);
        }

        ToolCallWidget *card = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_tc-stream"));
        QVERIFY2(card, "no card was created for the command");
        QVERIFY2(card->isRunning(), "the card finished even though the command has not");
        QTextBrowser *body = cardBody(card);
        QVERIFY(body);
        const QString plain = body->toPlainText();
        QVERIFY2(plain.contains(QStringLiteral("line 0 of the build")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("line 10 of the build")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("command")), qPrintable(plain));
    }

    // ACP stores every tool result before the assistant message that lists the
    // calls, and those calls carry arguments "{}". Rebuilding used to drop the
    // results and append empty cards after the answer.
    void testAcpHistoryPlacesFilledToolCardsWhereTheToolsRan()
    {
        setup();
        SessionStore::SessionData data;
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"please edit main"_s;
        ChatMessage edit;
        edit.role = ChatMessage::Role::Tool;
        edit.name = u"edit"_s;
        edit.toolCallId = u"call-edit"_s;
        edit.content = u"Edit applied successfully.\n"
                       "--- a/src/main.cpp\n"
                       "+++ b/src/main.cpp\n"
                       "@@ -1 +1 @@\n"
                       "-old\n"
                       "+new\n"_s;
        ChatMessage read;
        read.role = ChatMessage::Role::Tool;
        read.name = u"read"_s;
        read.toolCallId = u"call-read"_s;
        read.content = u"int helper() { return 1; }"_s;
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"Done editing main."_s;
        assistant.toolCalls = QJsonArray{functionCall(u"call-edit"_s, u"edit"_s, u"{}"_s)};
        data.messages = {user, edit, read, assistant};

        m_widget->agent()->restoreSession(data);
        m_widget->rebuildTranscript();
        QTest::qWait(150);

        QWidget *userCard = m_widget->findChild<QWidget *>(QStringLiteral("userCard"));
        ToolCallWidget *editCard = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_call-edit"));
        ToolCallWidget *readCard = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_call-read"));
        QTextBrowser *assistantBody = m_widget->findChild<QTextBrowser *>(QStringLiteral("assistantBrowser"));
        QVERIFY2(userCard && editCard && readCard && assistantBody, "history did not rebuild every part");
        if (QLayout *layout = userCard->parentWidget() ? userCard->parentWidget()->layout() : nullptr) {
            layout->activate();
        }
        const int userY = mappedTop(userCard);
        const int editY = mappedTop(editCard);
        const int readY = mappedTop(readCard);
        const int assistantY = mappedTop(assistantBody);
        QVERIFY2(userY < editY && editY < readY && readY < assistantY,
                 qPrintable(QStringLiteral("order user=%1 edit=%2 read=%3 assistant=%4")
                                .arg(userY).arg(editY).arg(readY).arg(assistantY)));
        QCOMPARE(m_widget->findChildren<ToolCallWidget *>().size(), 2);

        QTextBrowser *diffView = editCard->findChild<QTextBrowser *>(QStringLiteral("toolDiff"));
        QVERIFY2(diffView, "the restored edit has no diff");
        QVERIFY2(diffView->toPlainText().contains(QStringLiteral("+new")), qPrintable(diffView->toPlainText()));
        QVERIFY2(diffView->toHtml().contains(ChatTheme::success()), qPrintable(diffView->toHtml()));
        QVERIFY2(editCard->styleSheet().contains(ChatTheme::toolColor(QStringLiteral("edit_file"))),
                 qPrintable(editCard->styleSheet()));
        QTextBrowser *readBody = cardBody(readCard);
        QVERIFY2(readBody && readBody->toPlainText().contains(QStringLiteral("int helper()")),
                 qPrintable(readBody ? readBody->toPlainText() : QString()));
    }

    // Native turns record the assistant message (and its tool calls) first and
    // the tool result afterwards. The card stays under that bubble and is
    // filled from the result, once.
    void testNativeHistoryKeepsTheToolCardAfterTheAssistant()
    {
        setup();
        SessionStore::SessionData data;
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"fix main"_s;
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"I will patch it."_s;
        assistant.toolCalls = QJsonArray{
            functionCall(u"call-native"_s, u"edit_file"_s,
                         u"{\"path\":\"src/main.cpp\",\"old_string\":\"a\",\"new_string\":\"b\"}"_s)};
        ChatMessage tool;
        tool.role = ChatMessage::Role::Tool;
        tool.name = u"edit_file"_s;
        tool.toolCallId = u"call-native"_s;
        tool.content = u"patched src/main.cpp"_s;
        data.messages = {user, assistant, tool};

        m_widget->agent()->restoreSession(data);
        m_widget->rebuildTranscript();
        QTest::qWait(150);

        QWidget *userCard = m_widget->findChild<QWidget *>(QStringLiteral("userCard"));
        ToolCallWidget *card = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_call-native"));
        QTextBrowser *assistantBody = m_widget->findChild<QTextBrowser *>(QStringLiteral("assistantBrowser"));
        QVERIFY2(userCard && card && assistantBody, "native history did not rebuild every part");
        if (QLayout *layout = userCard->parentWidget() ? userCard->parentWidget()->layout() : nullptr) {
            layout->activate();
        }
        const int userY = mappedTop(userCard);
        const int assistantY = mappedTop(assistantBody);
        const int toolY = mappedTop(card);
        QVERIFY2(userY < assistantY && assistantY < toolY,
                 qPrintable(QStringLiteral("order user=%1 assistant=%2 tool=%3")
                                .arg(userY).arg(assistantY).arg(toolY)));
        QCOMPARE(m_widget->findChildren<ToolCallWidget *>().size(), 1);
        QTextBrowser *body = cardBody(card);
        QVERIFY2(body && body->toPlainText().contains(QStringLiteral("patched src/main.cpp")),
                 qPrintable(body ? body->toPlainText() : QString()));
    }
};

QTEST_MAIN(TestLiveCards)
#include "test_livecards.moc"
