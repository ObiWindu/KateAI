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
#include "sessionstoreisolation.h"
#include "settings.h"
#include "toolcallwidget.h"
#include "todowidget.h"
#include "questionwidget.h"
#include "suggestionwidget.h"
#include "types.h"

#include <QApplication>
#include <QFile>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTest>
#include <QTemporaryDir>
#include <QPoint>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextBrowser>
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
        // The widget autosaves its transcript, so without a scratch database
        // every test run would drop mock prompts like "Run a long command."
        // into the user's real conversation history.
        isolateSessionStore();
    }

    void cleanup()
    {
        teardown();
    }

    void testFirstWorkspaceIndexWritesTheGraphCache()
    {
        setup();
        // The first workspace load must persist the index immediately, so the
        // next start and ACP session can read it even before any edit.
        const QString cache = m_workspace->path() + u"/.kateai/project_graph.sqlite"_s;
        QVERIFY2(QFile::exists(cache), qPrintable(cache));
        QVERIFY(QFileInfo(cache).size() > 0);
        ProjectGraph graph;
        graph.setWorkspacePath(m_workspace->path());
        QVERIFY(graph.loadFromSqlite(cache));
        QVERIFY(graph.getNodeCount() > 0);
    }

    void legacyJsonGraphCacheMigratesWithoutBeingRewritten()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QFile source(workspace.filePath(u"README.md"_s));
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("legacy workspace\n"), QByteArray("legacy workspace\n").size());
        source.close();
        ProjectGraph legacy;
        legacy.setWorkspacePath(workspace.path());
        legacy.generateGraph(workspace.path());
        const QString jsonPath = workspace.filePath(u".kateai/project_graph.json"_s);
        QVERIFY(legacy.saveToFile(jsonPath));
        QFile jsonFile(jsonPath);
        QVERIFY(jsonFile.open(QIODevice::ReadOnly));
        const QByteArray original = jsonFile.readAll();
        jsonFile.close();

        AgentLoop agent;
        agent.setWorkspace(workspace.path());
        const QString sqlitePath = workspace.filePath(u".kateai/project_graph.sqlite"_s);
        QVERIFY(QFile::exists(sqlitePath));
        ProjectGraph migrated;
        migrated.setWorkspacePath(workspace.path());
        QVERIFY(migrated.loadFromSqlite(sqlitePath));
        QVERIFY(migrated.getNodeCount() > 0);
        QVERIFY(jsonFile.open(QIODevice::ReadOnly));
        QCOMPARE(jsonFile.readAll(), original);
    }

    void corruptGraphCacheIsQuarantinedButNewerSchemasArePreserved()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QVERIFY(QDir().mkpath(workspace.filePath(u".kateai"_s)));
        const QString cache = workspace.filePath(u".kateai/project_graph.sqlite"_s);
        const QByteArray corrupt = QByteArrayLiteral("not a sqlite database\n");
        QFile file(cache);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(corrupt), corrupt.size());
        file.close();
        AgentLoop agent;
        agent.setWorkspace(workspace.path());
        ProjectGraph restored;
        restored.setWorkspacePath(workspace.path());
        QVERIFY(restored.loadFromSqlite(cache));
        const QStringList backups = QDir(workspace.filePath(u".kateai"_s))
            .entryList({u"project_graph.sqlite.corrupt-*"_s}, QDir::Files);
        QCOMPARE(backups.size(), 1);
        QFile backup(workspace.filePath(u".kateai/"_s + backups.first()));
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), corrupt);

        QTemporaryDir newerWorkspace;
        QVERIFY(newerWorkspace.isValid());
        ProjectGraph graph;
        graph.setWorkspacePath(newerWorkspace.path());
        graph.generateGraph(newerWorkspace.path());
        const QString newerCache = newerWorkspace.filePath(u".kateai/project_graph.sqlite"_s);
        QVERIFY(graph.saveToSqlite(newerCache));
        {
            const QString connection = u"graph-newer-schema"_s;
            {
                QSqlDatabase database = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
                database.setDatabaseName(newerCache);
                QVERIFY(database.open());
                QSqlQuery query(database);
                QVERIFY(query.exec(u"PRAGMA user_version = 99"_s));
                database.close();
            }
            QSqlDatabase::removeDatabase(connection);
        }
        AgentLoop newerAgent;
        newerAgent.setWorkspace(newerWorkspace.path());
        QVERIFY(QDir(newerWorkspace.filePath(u".kateai"_s))
                    .entryList({u"project_graph.sqlite.corrupt-*"_s}, QDir::Files).isEmpty());
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
        QTextBrowser *promptBody = prompt->findChild<QTextBrowser *>();
        QVERIFY2(promptBody, "the prompt has no body");
        QVERIFY2(promptBody->height() > 20,
                 qPrintable(QStringLiteral("the prompt body collapsed to %1px").arg(promptBody->height())));
        QVERIFY2(promptBody->toPlainText().contains(QStringLiteral("patch it in place")),
                 qPrintable(promptBody->toPlainText()));
    }

    // The question card is the only card whose payload is prose. Once the
    // question is answered the answer takes the body, and the question that
    // prompted it has to stay in the card: the restored card keeps it, and a
    // live card that dropped it read as "the agent answered ok" with no
    // question anywhere near it.
    void testQuestionCardKeepsTheQuestionAfterItIsAnswered()
    {
        setup();
        PermissionRequest req = request(u"question"_s, u"tc-q2"_s, u"{\"question\": \"Which one?\"}"_s);
        req.questionText = u"Which one?"_s;
        req.questionOptions = QStringList({u"Rewrite"_s});
        req.allowFreeform = true;
        Q_EMIT m_widget->agent()->toolStarted(req);
        Q_EMIT m_widget->agent()->questionAsked(req);
        QTest::qWait(80);

        ToolCallWidget *card = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_tc-q2"));
        QVERIFY2(card, "no card was created for the question");
        QTextBrowser *body = cardBody(card);
        QVERIFY2(body, "the question card has no body surface");
        QVERIFY2(body->toPlainText().contains(QStringLiteral("Which one?")),
                 qPrintable(body->toPlainText()));

        // The answer lands in the same card, and the question stays with it.
        Q_EMIT m_widget->agent()->toolFinished(result(u"question"_s, u"tc-q2"_s, u"Rewrite"_s));
        QTest::qWait(80);
        const QString finished = body->toPlainText();
        QVERIFY2(finished.contains(QStringLiteral("Which one?")),
                 qPrintable(QStringLiteral("the finished card lost the question: %1").arg(finished)));
        QVERIFY2(finished.contains(QStringLiteral("Rewrite")),
                 qPrintable(QStringLiteral("the finished card lost the answer: %1").arg(finished)));
    }

    // An agent hands its advice over under a name of its own. The advice is
    // still advice: the card has to show it, not the arguments as a JSON blob.
    // The restored card already matched on the canonical name, so a live call
    // that answered to a different name read completely differently from the
    // same conversation after a reload.
    void testAdviceUnderAnAliasNameStillGetsASuggestionCard()
    {
        setup();
        PermissionRequest suggestion = request(u"suggestion"_s, u"tc-suggest-alias-name"_s, QString());
        suggestion.originalToolName = u"suggestion"_s;
        suggestion.details = QStringLiteral(
            "{\"suggestion\": \"Hoist this allocation out of the loop\", \"path\": \"src/agentloop.cpp\", \"line\": 2100}");
        Q_EMIT m_widget->agent()->toolStarted(suggestion);
        QTest::qWait(80);

        SuggestionWidget *card = suggestionCard(u"tc-suggest-alias-name"_s);
        QVERIFY2(card, "advice under another name produced no suggestion card");
        const QList<QTextBrowser *> bodies = card->findChildren<QTextBrowser *>();
        QVERIFY(!bodies.isEmpty());
        QVERIFY2(bodies.first()->toPlainText().contains(u"Hoist this allocation"_s),
                 qPrintable(bodies.first()->toPlainText()));
        // The generic card would render the arguments as a JSON blob instead of
        // the advice they carry.
        QVERIFY2(!cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_tc-suggest-alias-name")),
                 "advice must not get the generic tool card");
    }

    // The same for a question: the card in the transcript carries the question
    // and the prompt above the composer repeats it.
    void testQuestionUnderAnAliasNameStillShowsTheQuestion()
    {
        setup();
        PermissionRequest question = request(u"ask_user"_s, u"tc-question-alias-name"_s,
                                             u"{\"question\": \"Which strategy should I use?\"}"_s);
        question.originalToolName = u"ask_user"_s;
        Q_EMIT m_widget->agent()->toolStarted(question);
        Q_EMIT m_widget->agent()->questionAsked(question);
        QTest::qWait(80);

        ToolCallWidget *card = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_tc-question-alias-name"));
        QVERIFY2(card, "no card was created for the question");
        QTextBrowser *body = cardBody(card);
        QVERIFY2(body->toPlainText().contains(u"Which strategy should I use?"_s),
                 qPrintable(body->toPlainText()));

        QuestionWidget *prompt = questionCard(u"tc-question-alias-name"_s);
        QVERIFY2(prompt, "no question prompt in the intent dock");
        QTextBrowser *promptBody = prompt->findChild<QTextBrowser *>();
        QVERIFY(promptBody);
        QVERIFY2(promptBody->toPlainText().contains(u"Which strategy should I use?"_s),
                 qPrintable(promptBody->toPlainText()));
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
        QVERIFY2(bodies.first()->height() > 20,
                 qPrintable(QStringLiteral("the posted card body collapsed to %1px").arg(bodies.first()->height())));

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

    // A user turn is shown verbatim so long or structured prompts keep every
    // character and line break in the transcript.
    void testUserMessageRendersVerbatim()
    {
        setup();
        const QString prompt = u"Compare **this** with `grep`:\n\n```cpp\nint x = 1;\n```\n"_s;
        Q_EMIT m_widget->agent()->userMessage(prompt);
        QTest::qWait(80);

        QTextBrowser *body = m_widget->findChild<QTextBrowser *>(QStringLiteral("userMessageBody"));
        QVERIFY2(body, "the user card has no markup body");
        const QString plain = body->toPlainText();
        QCOMPARE(plain, prompt);
        QVERIFY2(body->height() > 20, qPrintable(QStringLiteral("body collapsed to %1px").arg(body->height())));
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

    // The card is measured before the transcript layout gives it its real
    // width. A long prompt was laid out at the provisional width and then
    // re-wrapped at the final one without its pinned height following, which
    // cut the bottom of the message off. Re-fitting on resize keeps the whole
    // prompt visible at any panel width.
    void testVeryLongUserMessageIsNotClipped()
    {
        setup();
        // A narrow panel makes the provisional and final widths differ enough
        // for the stale height to hide a large part of the message.
        m_widget->resize(520, 700);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        QString longText;
        for (int i = 0; i < 400; ++i) {
            longText += QStringLiteral("line %1 of a very long prompt that should wrap and stay visible\n").arg(i);
        }
        Q_EMIT m_widget->agent()->userMessage(longText);
        QTest::qWait(200);

        QTextBrowser *body = m_widget->findChild<QTextBrowser *>(QStringLiteral("userMessageBody"));
        QVERIFY2(body, "the user card has no markup body");

        const auto verifyBodyFitsItsDocument = [&]() {
            const int docH = qCeil(body->document()->size().height()) + 4;
            QVERIFY2(body->height() >= docH,
                     qPrintable(QStringLiteral("body is %1px but its document needs %2px")
                                    .arg(body->height()).arg(docH)));
            QVERIFY2(body->toPlainText().count(QStringLiteral("of a very long prompt")) == 400,
                     qPrintable(QStringLiteral("only %1 lines arrived")
                                    .arg(body->toPlainText().count(QStringLiteral("of a very long prompt")))));
        };

        verifyBodyFitsItsDocument();

        // The panel can be widened later; the pinned height has to follow that
        // re-wrap too, or the card grows a block of empty space.
        m_widget->resize(900, 700);
        QTest::qWait(200);
        verifyBodyFitsItsDocument();
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
    void longHistoryIsVirtualizedWithoutLosingEitherEnd()
    {
        setup();
        SessionStore::SessionData data;
        data.messages.reserve(600);
        for (int i = 0; i < 300; ++i) {
            ChatMessage user;
            user.role = ChatMessage::Role::User;
            user.content = i == 0 ? u"first question"_s : u"question %1: "_s.arg(i) + QString(180, u'q');
            data.messages.append(user);
            ChatMessage assistant;
            assistant.role = ChatMessage::Role::Assistant;
            assistant.content = i == 299 ? u"last answer"_s : u"answer %1: "_s.arg(i) + QString(180, u'a');
            data.messages.append(assistant);
        }
        m_widget->agent()->restoreSession(data);
        m_widget->rebuildTranscript();
        QVERIFY(QTest::qWaitFor([this] {
            return !m_widget->findChildren<QWidget *>(u"userCard"_s).isEmpty();
        }, 1000));
        QCOMPARE(m_widget->agent()->transcriptMessages().size(), 600);
        QCOMPARE(m_widget->findChildren<QWidget *>(u"transcriptVirtualRow"_s).size(), 600);
        const int mounted = m_widget->findChildren<QWidget *>(u"userCard"_s).size();
        QVERIFY(mounted > 0 && mounted < 40);
        QVERIFY(m_widget->findChildren<QTextBrowser *>().size() < 80);
        QVERIFY(QTest::qWaitFor([this] {
            for (const QTextBrowser *browser : m_widget->findChildren<QTextBrowser *>(u"assistantBrowser"_s)) {
                if (browser->toPlainText().contains(u"last answer"_s)) {
                    return true;
                }
            }
            return false;
        }, 1000));
        // Let rebuildTranscript's deferred initial scroll-to-tail finish before
        // simulating the user's scroll-up.
        QTest::qWait(50);
        auto *scroll = m_widget->findChild<QScrollArea *>();
        QVERIFY(scroll);
        scroll->verticalScrollBar()->setValue(0);
        QVERIFY(QTest::qWaitFor([this] {
            for (const QTextBrowser *browser : m_widget->findChildren<QTextBrowser *>(u"userMessageBody"_s)) {
                if (browser->toPlainText().contains(u"first question"_s)) {
                    return true;
                }
            }
            return false;
        }, 1000));
        QVERIFY(m_widget->findChildren<QWidget *>(u"userCard"_s).size() < 40);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QTest::qWait(50);
        QCOMPARE(m_widget->agent()->transcriptMessages().size(), 600);
    }

    void sparseLongHistoryFallsBackToTheEagerRenderer()
    {
        setup();
        SessionStore::SessionData data;
        for (int i = 0; i < 300; ++i) {
            ChatMessage internal;
            internal.role = ChatMessage::Role::System;
            internal.name = u"internal"_s;
            internal.content = u"not rendered"_s;
            data.messages.append(internal);
        }
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"visible question"_s;
        data.messages.append(user);
        ChatMessage answer;
        answer.role = ChatMessage::Role::Assistant;
        answer.content = u"visible answer"_s;
        data.messages.append(answer);
        m_widget->agent()->restoreSession(data);
        m_widget->rebuildTranscript();
        QTest::qWait(30);
        QVERIFY(m_widget->findChild<QWidget *>(u"userCard"_s));
        QVERIFY(m_widget->findChild<QTextBrowser *>(u"assistantBrowser"_s));
        QVERIFY(m_widget->findChildren<QWidget *>(u"transcriptVirtualRow"_s).isEmpty());
    }

    void reusedHistoricalIdDoesNotHijackLiveRouting()
    {
        setup();
        SessionStore::SessionData data;
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"run reads"_s;
        data.messages.append(user);
        for (int i = 0; i < 300; ++i) {
            ChatMessage tool;
            tool.role = ChatMessage::Role::Tool;
            tool.toolCallId = u"reused"_s;
            tool.name = u"read_file"_s;
            tool.content = u"historical output %1"_s.arg(i);
            data.messages.append(tool);
        }
        m_widget->agent()->restoreSession(data);
        m_widget->rebuildTranscript();
        QTest::qWait(50);
        Q_EMIT m_widget->agent()->toolStarted(request(u"read_file"_s, u"reused"_s, u"live input"_s));
        ToolCallWidget *live = nullptr;
        for (ToolCallWidget *card : m_widget->findChildren<ToolCallWidget *>()) {
            if (card->toolCallId() == u"reused"_s && card->isRunning()) {
                live = card;
                break;
            }
        }
        QVERIFY(live);
        m_widget->focusToolCard(1);
        QTest::qWait(30);
        m_widget->focusToolCard(300);
        QCOMPARE(m_widget->focusedToolCardIndex(), 300);
        QVERIFY(live->isExpanded());
        Q_EMIT m_widget->agent()->toolFinished(result(u"read_file"_s, u"reused"_s, u"live result"_s));
        QVERIFY(!live->isRunning());
        QTextBrowser *body = cardBody(live);
        QVERIFY(body);
        QVERIFY(body->toPlainText().contains(u"live result"_s));
    }

    void toolNavigationMaterializesOffscreenHistoryAndRemembersExpansion()
    {
        setup();
        SessionStore::SessionData data;
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"run the reads"_s;
        data.messages.append(user);
        for (int i = 0; i < 300; ++i) {
            ChatMessage tool;
            tool.role = ChatMessage::Role::Tool;
            tool.toolCallId = u"call-%1"_s.arg(i);
            tool.name = u"read_file"_s;
            tool.content = u"output %1"_s.arg(i);
            data.messages.append(tool);
        }
        ChatMessage answer;
        answer.role = ChatMessage::Role::Assistant;
        answer.content = u"all reads complete"_s;
        data.messages.append(answer);
        m_widget->agent()->restoreSession(data);
        m_widget->rebuildTranscript();
        QTest::qWait(80);
        QVERIFY(m_widget->findChildren<ToolCallWidget *>().size() < 40);
        m_widget->focusToolCard(1);
        QTest::qWait(30);
        QCOMPARE(m_widget->focusedToolCardIndex(), 0);
        ToolCallWidget *first = cardOfClass<ToolCallWidget>(u"ToolCallWidget_call-0"_s);
        QVERIFY(first);
        QVERIFY(first->isExpanded());
        m_widget->focusToolCard(299);
        QTest::qWait(50);
        QVERIFY(cardOfClass<ToolCallWidget>(u"ToolCallWidget_call-299"_s));
        QVERIFY(m_widget->findChildren<ToolCallWidget *>().size() < 80);
        m_widget->focusToolCard(-299);
        QTest::qWait(50);
        first = cardOfClass<ToolCallWidget>(u"ToolCallWidget_call-0"_s);
        QVERIFY(first);
        QVERIFY(first->isExpanded());
        Q_EMIT m_widget->agent()->toolStarted(request(u"read_file"_s, u"live-after-restore"_s, u"live tail"_s));
        m_widget->focusToolCard(300);
        QCOMPARE(m_widget->focusedToolCardIndex(), 300);
        QVERIFY(cardOfClass<ToolCallWidget>(u"ToolCallWidget_live-after-restore"_s));
    }

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

    void testReasoningAfterAnswerStartsDoesNotStreamAlongsideIt()
    {
        setup();
        Q_EMIT m_widget->agent()->thinkingDelta(u"Initial reasoning. "_s);
        auto *reasoning = m_widget->findChild<QTextBrowser *>(QStringLiteral("reasoningBrowser"));
        QVERIFY2(reasoning, "the reasoning browser was not found");
        QVERIFY2(QTest::qWaitFor([reasoning] {
            return reasoning->toPlainText().contains(QStringLiteral("Initial reasoning."));
        }), qPrintable(reasoning->toPlainText()));

        // The first visible text chunk starts the answer phase. Everything the
        // agent reasoned before it has to be on screen first.
        Q_EMIT m_widget->agent()->assistantDelta(u"The answer starts. "_s);
        QVERIFY(QTest::qWaitFor([reasoning] {
            return reasoning->toPlainText().contains(QStringLiteral("Initial reasoning."));
        }));
        auto *pill = m_widget->findChild<QLabel *>(QStringLiteral("thinkingIndicator"));
        QVERIFY(pill);
        QVERIFY2(!pill->isVisible(), "the thinking pill kept running while the answer streamed");

        // Some providers keep emitting reasoning after the answer started. It
        // may be kept for the transcript, but it must not animate into the
        // reasoning block while the answer is being written.
        Q_EMIT m_widget->agent()->thinkingDelta(u"Late reasoning that must not animate."_s);
        QTest::qWait(300);
        QVERIFY2(!reasoning->toPlainText().contains(QStringLiteral("Late reasoning")),
                 qPrintable(reasoning->toPlainText()));
        QVERIFY2(!pill->isVisible(), "late reasoning restarted the thinking pill during the answer");

        // Freezing the turn keeps that reasoning in the transcript without
        // having shown it alongside the answer.
        Q_EMIT m_widget->agent()->assistantFinished(u"The answer starts. "_s);
        QVERIFY2(QTest::qWaitFor([reasoning] {
            return reasoning->toPlainText().contains(QStringLiteral("Late reasoning"));
        }), qPrintable(reasoning->toPlainText()));
        auto *answer = m_widget->findChild<QTextBrowser *>(QStringLiteral("assistantBrowser"));
        QVERIFY(answer);
        QVERIFY2(answer->toPlainText().contains(QStringLiteral("The answer starts.")),
                 qPrintable(answer->toPlainText()));
    }

    void testHistoryRestoresSuggestionAndQuestionText()
    {
        setup();
        SessionStore::SessionData data;

        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"Posting advice and asking a question."_s;
        assistant.toolCalls = QJsonArray{
            functionCall(u"call-suggest"_s, u"suggest"_s,
                         u"{\"suggestion\":\"Use std::string_view here\",\"path\":\"src/main.cpp\",\"line\":42}"_s),
            functionCall(u"call-question"_s, u"question"_s,
                         u"{\"question\":\"Rewrite the file, or patch it in place?\",\"options\":[\"Rewrite\",\"Patch\"]}"_s)};

        ChatMessage suggestResult;
        suggestResult.role = ChatMessage::Role::Tool;
        suggestResult.name = u"suggest"_s;
        suggestResult.toolCallId = u"call-suggest"_s;
        suggestResult.content = u"Suggestion posted for the user to accept or dismiss."_s;

        ChatMessage questionResult;
        questionResult.role = ChatMessage::Role::Tool;
        questionResult.name = u"question"_s;
        questionResult.toolCallId = u"call-question"_s;
        questionResult.content = u"Rewrite"_s;

        data.messages = {assistant, suggestResult, questionResult};
        m_widget->agent()->restoreSession(data);
        m_widget->rebuildTranscript();
        QTest::qWait(150);

        SuggestionWidget *suggestion = suggestionCard(u"call-suggest"_s);
        QVERIFY2(suggestion, "history restore dropped the suggestion card");
        QTextBrowser *suggestionBody = suggestion->findChild<QTextBrowser *>();
        QVERIFY(suggestionBody);
        QVERIFY2(suggestionBody->toPlainText().contains(u"std::string_view"_s),
                 qPrintable(suggestionBody->toPlainText()));

        ToolCallWidget *question = cardOfClass<ToolCallWidget>(QStringLiteral("ToolCallWidget_call-question"));
        QVERIFY2(question, "history restore dropped the question card");
        QTextBrowser *questionBody = cardBody(question);
        QVERIFY(questionBody);
        QVERIFY2(questionBody->toPlainText().contains(u"Rewrite the file, or patch it in place?"_s),
                 qPrintable(questionBody->toPlainText()));
    }

    // Providers do not all use the canonical field names. A card that only
    // reads request.questionText / details["suggestion"] is blank when the
    // call carries the same content as prompt/message/description.
    void testCardsReadAliasFields()
    {
        setup();

        PermissionRequest question = request(u"question"_s, u"tc-question-alias"_s,
                                             u"{\"questions\":[{\"prompt\":\"Which strategy should I use?\",\"choices\":[{\"label\":\"Fast\"},{\"label\":\"Correct\"}],\"allowFreeform\":false}]}"_s);
        Q_EMIT m_widget->agent()->toolStarted(question);
        Q_EMIT m_widget->agent()->questionAsked(question);
        QTest::qWait(80);

        QuestionWidget *prompt = questionCard(u"tc-question-alias"_s);
        QVERIFY2(prompt, "the alias-shaped question produced no prompt");
        QTextBrowser *promptBody = prompt->findChild<QTextBrowser *>();
        QVERIFY(promptBody);
        QVERIFY2(promptBody->toPlainText().contains(u"Which strategy should I use?"_s),
                 qPrintable(promptBody->toPlainText()));
        QStringList labels;
        for (QPushButton *button : prompt->findChildren<QPushButton *>()) {
            labels << button->text();
        }
        QVERIFY2(labels.contains(u"Fast"_s) && labels.contains(u"Correct"_s),
                 qPrintable(labels.join(u", "_s)));

        PermissionRequest suggestion = request(u"suggest"_s, u"tc-suggest-alias"_s,
                                              u"{\"message\":\"Use std::string_view here\",\"file_path\":\"src/main.cpp\",\"start_line\":\"42\"}"_s);
        Q_EMIT m_widget->agent()->toolStarted(suggestion);
        QTest::qWait(80);

        SuggestionWidget *card = suggestionCard(u"tc-suggest-alias"_s);
        QVERIFY2(card, "the alias-shaped suggestion produced no card");
        const QList<QTextBrowser *> bodies = card->findChildren<QTextBrowser *>();
        QVERIFY(!bodies.isEmpty());
        QVERIFY2(bodies.first()->toPlainText().contains(u"std::string_view"_s),
                 qPrintable(bodies.first()->toPlainText()));
        QVERIFY2(card->findChildren<QLabel *>().size() > 0, "the suggestion card has no labels");
        bool anchored = false;
        for (QLabel *label : card->findChildren<QLabel *>()) {
            if (label->text().contains(u"src/main.cpp:42"_s)) {
                anchored = true;
            }
        }
        QVERIFY2(anchored, "the alias-shaped suggestion lost its anchor");
    }

    // The plan's todo list docks at the bottom of the chat: a compact strip
    // above the composer that follows the agent's plan updates and stays out
    // of the transcript, so it does not scroll away mid-turn.
    void testPlanDocksAboveTheComposer()
    {
        setup();

        auto *dock = m_widget->findChild<TodoWidget *>();
        QVERIFY2(dock, "the chat has no todo dock");
        QVERIFY2(!dock->isVisible(), "the dock shows with no plan");

        // A plan update from the agent lands in the dock, not in a card.
        QJsonArray plan;
        for (int i = 1; i <= 5; ++i) {
            QJsonObject step;
            step.insert(QStringLiteral("id"), QString::number(i));
            step.insert(QStringLiteral("description"), QStringLiteral("Step %1").arg(i));
            step.insert(QStringLiteral("completed"), i <= 2);
            step.insert(QStringLiteral("inProgress"), i == 3);
            plan.append(step);
        }
        Q_EMIT m_widget->agent()->planUpdated(plan);
        QTest::qWait(80);
        QVERIFY(dock->isVisible());
        QCOMPARE(dock->totalCount(), 5);
        QCOMPARE(dock->completedCount(), 2);
        // The compact window: three rows on screen.
        QCOMPARE(dock->findChildren<QWidget *>(QStringLiteral("todoRow")).size(), 3);

        // Docked at the bottom: outside the scroll area's transcript, below
        // the scroll area's bottom edge.
        auto *scroll = m_widget->findChild<QScrollArea *>();
        QVERIFY(scroll);
        QVERIFY2(dock->parentWidget() == m_widget, "the dock is not a direct child of the chat");
        QVERIFY2(dock->mapTo(m_widget, QPoint(0, 0)).y() >= scroll->mapTo(m_widget, QPoint(0, 0)).y() + scroll->height(),
                 "the dock is not below the transcript");

        // An empty plan retires the dock again.
        Q_EMIT m_widget->agent()->planUpdated(QJsonArray());
        QTest::qWait(80);
        QVERIFY(!dock->isVisible());

        teardown();
    }
};

QTEST_MAIN(TestLiveCards)
#include "test_livecards.moc"
