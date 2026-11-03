/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * A delegated sub-agent has to be visible while it runs: its card shows the
 * delegated task, names its current step, and keeps a live activity log.
 * Driven through a real ChatWidget against the mock LLM server, which answers
 * the orchestrator with two new_task calls and each child with delayed text.
 */

#include "agentloop.h"
#include "chatwidget.h"
#include "modes.h"
#include "sessionstoreisolation.h"
#include "subtaskwidget.h"
#include "test_python.h"
#include "toolcallwidget.h"
#include "types.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QNetworkProxyFactory>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestSubtaskLive : public QObject
{
    Q_OBJECT

private:
    static QString repoFile(const QString &relativePath)
    {
        QDir dir(QDir::current());
        for (int i = 0; i < 5; ++i) {
            const QString candidate = dir.absoluteFilePath(relativePath);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
            dir.cdUp();
        }
        return {};
    }

    template<typename Predicate>
    static bool waitFor(Predicate predicate, int timeoutMs = 30000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate()) {
            if (timer.elapsed() > timeoutMs) {
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QTest::qWait(10);
        }
        return true;
    }

    static QList<SubtaskWidget *> subtaskCards(QWidget *root)
    {
        return root->findChildren<SubtaskWidget *>();
    }

    // Plain text of every document inside a sub-agent card.
    static QString cardText(SubtaskWidget *card)
    {
        QString out;
        for (QTextBrowser *browser : card->findChildren<QTextBrowser *>()) {
            out += browser->toPlainText();
            out += u'\n';
        }
        return out;
    }

    // The Activity disclosure's label, e.g. "Activity (3)  ▾".
    static QString activityToggleText(SubtaskWidget *card)
    {
        for (QPushButton *button : card->findChildren<QPushButton *>()) {
            if (button->text().startsWith(u"Activity")) {
                return button->text();
            }
        }
        return {};
    }

    std::unique_ptr<QTemporaryDir> m_workspace;
    QProcess *m_server = nullptr;
    QString m_portFile;
    int m_port = 0;

    ChatWidget *makeWidget()
    {
        auto *widget = new ChatWidget;
        widget->resize(900, 700);
        widget->show();

        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = u"http://127.0.0.1:%1/v1"_s.arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"orchestrator"_s;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.sandbox = SandboxProfile::Off;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;
        settings.maxModelRequests = 10;
        settings.maxToolCalls = 30;
        settings.maxParallelSubtasks = 3;
        settings.maxSubtaskDepth = 1;
        settings.checkpointsEnabled = false;
        widget->setSettings(settings);
        widget->agent()->setWorkspace(m_workspace->path());
        return widget;
    }

private Q_SLOTS:
    void initTestCase()
    {
        isolateSessionStore();
        QNetworkProxyFactory::setUseSystemConfiguration(false);
    }

    void init()
    {
        m_workspace = std::make_unique<QTemporaryDir>();
        QVERIFY(m_workspace->isValid());
        QFile marker(m_workspace->path() + u"/README.md"_s);
        QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::Text));
        marker.write("workspace\n");

        m_portFile = m_workspace->path() + u"/port"_s;
        m_server = new QProcess(this);
        const QString script = repoFile(u"tests/mock_llm_server.py"_s);
        QVERIFY2(!script.isEmpty(), "could not locate tests/mock_llm_server.py");
        m_server->start(python3Command(), QStringList{script, m_portFile});
        QVERIFY(m_server->waitForStarted(10000));

        QVERIFY(waitFor([this] {
            return QFileInfo::exists(m_portFile);
        }, 15000));
        QFile portFile(m_portFile);
        QVERIFY(portFile.open(QIODevice::ReadOnly | QIODevice::Text));
        m_port = QString::fromUtf8(portFile.readAll()).trimmed().toInt();
        QVERIFY(m_port > 0);
    }

    void cleanup()
    {
        if (m_server && m_server->state() != QProcess::NotRunning) {
            m_server->kill();
            m_server->waitForFinished(2000);
        }
        m_server = nullptr;
        m_workspace.reset();
    }

    void subagentCardsShowTaskAndLiveActivity()
    {
        std::unique_ptr<ChatWidget> widget(makeWidget());
        widget->ask(u"Split this across two agents."_s);

        // Both delegations surface as live cards while the children run. The
        // children answer after ~1.5s, so there is a wide window in flight.
        QVERIFY2(waitFor([&] {
            return subtaskCards(widget.get()).size() >= 2;
        }), "no sub-agent cards appeared while the children ran");

        // The card names the delegated work: startAgent ran, so the widget is
        // bound to its task id and the live signals reach it.
        QVERIFY2(waitFor([&] {
            for (SubtaskWidget *card : subtaskCards(widget.get())) {
                if (!cardText(card).contains(u"CHILD-TASK")) {
                    return false;
                }
            }
            return true;
        }), "a sub-agent card never showed its delegated task");

        // The activity log relays the child's status lines while it works: an
        // empty log means the live signals never reached the card.
        QVERIFY2(waitFor([&] {
            for (SubtaskWidget *card : subtaskCards(widget.get())) {
                if (!activityToggleText(card).contains(u"(")) {
                    return false;
                }
            }
            return true;
        }), "a sub-agent card's activity box stayed empty while it ran");

        // The turn settles and both cards retire into the transcript with the
        // children's answers instead of spinning in the dock.
        QVERIFY2(waitFor([&] {
            return !widget->agent()->isBusy();
        }, 40000), "the orchestrator turn never finished");
        bool sawChildAnswer = false;
        for (SubtaskWidget *card : subtaskCards(widget.get())) {
            if (cardText(card).contains(u"finished its share")) {
                sawChildAnswer = true;
            }
        }
        QVERIFY2(sawChildAnswer, "no sub-agent card showed its child's answer after the turn");

        // Finished cards retire where the delegation happened, not at the end
        // of the chat: every sub-agent card must sit above the orchestrator's
        // follow-up summary in the transcript.
        QWidget *summaryCard = nullptr;
        for (QWidget *card : widget->findChildren<QWidget *>(u"assistantCard"_s)) {
            for (QTextBrowser *browser : card->findChildren<QTextBrowser *>()) {
                if (browser->toPlainText().contains(u"Both sub-agents reported back")) {
                    summaryCard = card;
                }
            }
        }
        QVERIFY2(summaryCard, "the orchestrator summary card was not found");
        QLayout *transcriptLayout = summaryCard->parentWidget()
            ? summaryCard->parentWidget()->layout()
            : nullptr;
        QVERIFY2(transcriptLayout, "the summary card has no transcript layout");
        const int summaryIndex = transcriptLayout->indexOf(summaryCard);
        for (SubtaskWidget *card : subtaskCards(widget.get())) {
            const int cardIndex = card->parentWidget() && card->parentWidget()->layout()
                ? card->parentWidget()->layout()->indexOf(card)
                : -1;
            QVERIFY2(cardIndex >= 0 && cardIndex < summaryIndex,
                     qPrintable(QStringLiteral("a finished sub-agent card sits at %1, below the summary at %2")
                                    .arg(cardIndex)
                                    .arg(summaryIndex)));
        }
    }

    // Finished cards retire where the delegation happened, in finish order:
    // two runs sharing one anchor chain after each other instead of the
    // second landing before the first (or at the end of the chat).
    void finishedCardsRetireInFinishOrder()
    {
        std::unique_ptr<ChatWidget> widget(makeWidget());

        // An ordinary transcript card for both delegations to anchor to.
        PermissionRequest readRequest;
        readRequest.toolName = u"read_file"_s;
        readRequest.toolCallId = u"r0"_s;
        readRequest.summary = u"Read notes.txt"_s;
        readRequest.risk = ToolRisk::Read;
        widget->agent()->toolStarted(readRequest);

        // Two delegations issued together share that anchor.
        for (const QString &id : {u"t1"_s, u"t2"_s}) {
            PermissionRequest request;
            request.toolName = subtaskToolName();
            request.toolCallId = id;
            request.summary = u"sub-agent (code): work "_s + id;
            request.risk = ToolRisk::Execute;
            widget->agent()->toolStarted(request);
            widget->agent()->subtaskStarted(id, u"coder"_s, u"Coder"_s, u"code"_s,
                                            u"Delegated work "_s + id);
        }

        auto transcriptIndex = [&](QWidget *card) {
            return card && card->parentWidget() && card->parentWidget()->layout()
                ? card->parentWidget()->layout()->indexOf(card)
                : -1;
        };
        auto findToolCard = [&]() -> QWidget * {
            const QList<ToolCallWidget *> tools = widget->findChildren<ToolCallWidget *>();
            return tools.isEmpty() ? nullptr : tools.first();
        };

        // Finish in delegation order: each card lands after the previous one.
        for (const QString &id : {u"t1"_s, u"t2"_s}) {
            ToolResult result;
            result.toolCallId = id;
            result.name = subtaskToolName();
            result.ok = true;
            result.output = u"answer "_s + id;
            widget->agent()->toolFinished(result);
        }

        QList<SubtaskWidget *> cards = subtaskCards(widget.get());
        QCOMPARE(cards.size(), 2);
        QWidget *toolCard = findToolCard();
        QVERIFY2(toolCard, "the anchor tool card was not found in the transcript");
        const int toolIndex = transcriptIndex(toolCard);
        QVERIFY2(toolIndex >= 0, "the anchor tool card has no transcript position");
        // Map finish order to cards by their answers.
        SubtaskWidget *first = nullptr;
        SubtaskWidget *second = nullptr;
        for (SubtaskWidget *card : cards) {
            if (cardText(card).contains(u"answer t1")) {
                first = card;
            } else if (cardText(card).contains(u"answer t2")) {
                second = card;
            }
        }
        QVERIFY2(first && second, "a retired card lost its answer");
        const int firstIndex = transcriptIndex(first);
        const int secondIndex = transcriptIndex(second);
        QVERIFY2(toolIndex < firstIndex && firstIndex < secondIndex,
                 qPrintable(QStringLiteral("expected tool < t1 < t2, got %1 < %2 < %3")
                                .arg(toolIndex)
                                .arg(firstIndex)
                                .arg(secondIndex)));
    }
    // An ACP agent runs task-like tools inside its own process: no native
    // sub-agent is ever dispatched, so no live subtask signals would reach a
    // sub-agent card. Those calls must render as ordinary tool cards (which
    // do show live output and results), not as a sub-agent card with an
    // eternally empty activity box.
    void acpTaskCallsUseToolCardsNotSubagentCards()
    {
        std::unique_ptr<ChatWidget> widget(makeWidget());
        Settings settings;
        settings.provider = Provider::Acp;
        settings.apiFormat = ApiFormat::AcpNative;
        settings.acpAgentId = u"mock"_s;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.sandbox = SandboxProfile::Off;
        widget->setSettings(settings);

        PermissionRequest request;
        request.toolName = subtaskToolName();
        request.toolCallId = u"acp-call-1"_s;
        request.summary = u"sub-agent (code): do a thing"_s;
        request.risk = ToolRisk::Execute;
        widget->agent()->toolStarted(request);
        QVERIFY(waitFor([&] {
            return !widget->findChildren<ToolCallWidget *>().isEmpty();
        }));

        QCOMPARE(subtaskCards(widget.get()).size(), 0);
        QVERIFY(!widget->findChildren<ToolCallWidget *>().isEmpty());
    }

    // Dispatch renames calls whose tool id is empty: the card is born under
    // the empty id but the live signals carry the renamed one. The card must
    // still start and its activity box must still fill.
    void renamedSubtaskCallStillFeedsItsCard()
    {
        std::unique_ptr<ChatWidget> widget(makeWidget());

        PermissionRequest request;
        request.toolName = subtaskToolName();
        request.toolCallId = QString();
        request.summary = u"sub-agent (code): rekeyed work"_s;
        request.risk = ToolRisk::Execute;
        widget->agent()->toolStarted(request);
        QVERIFY2(waitFor([&] {
            return subtaskCards(widget.get()).size() >= 1;
        }), "no sub-agent card appeared for the renamed call");

        widget->agent()->subtaskStarted(u"sub-99"_s, u"coder"_s, u"Coder"_s, u"code"_s,
                                             u"Rekeyed delegated work"_s);
        widget->agent()->subtaskActivity(u"sub-99"_s, u"→ read_file notes.txt"_s, false);
        QVERIFY2(waitFor([&] {
            for (SubtaskWidget *card : subtaskCards(widget.get())) {
                if (cardText(card).contains(u"Rekeyed delegated work")
                    && activityToggleText(card).contains(u"(")) {
                    return true;
                }
            }
            return false;
        }), "the renamed call never started its card: task text and activity missing");
    }
};

QTEST_MAIN(TestSubtaskLive)
#include "test_subtasklive.moc"
