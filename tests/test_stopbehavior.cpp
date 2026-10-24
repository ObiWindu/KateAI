/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The stop button and the new-thread button, driven through a real AgentLoop
 * against the mock LLM server.
 *
 * The mock answers "Run a long command." with a bash call that sleeps far
 * longer than the turn should ever be allowed to run, and "Type slowly." with
 * a stream spaced out over several seconds -- both are windows in which the
 * user has to be able to press Stop. The new-thread button then has to clear
 * what the aborted turn left behind and leave a chat that works again.
 */

#include "agentloop.h"
#include "chatwidget.h"
#include "sessionstoreisolation.h"
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
#include <QPushButton>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestStopBehavior : public QObject
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
    static bool waitFor(Predicate predicate, int timeoutMs = 20000)
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

    // The composer's single round button morphs between send and stop; it is
    // the only one carrying either tooltip.
    static QPushButton *sendButton(QWidget *root)
    {
        for (QPushButton *button : root->findChildren<QPushButton *>()) {
            if (button->toolTip() == u"Send message"_s || button->toolTip() == u"Stop response"_s) {
                return button;
            }
        }
        return nullptr;
    }

    // The toolbar's new-thread button is identified by its tooltip, the same
    // way the send button is.
    static QPushButton *newThreadButton(QWidget *root)
    {
        for (QPushButton *button : root->findChildren<QPushButton *>()) {
            if (button->toolTip() == u"New Thread"_s) {
                return button;
            }
        }
        return nullptr;
    }

    // Everything the transcript currently shows, as plain text, so a test can
    // ask whether some piece of content survived a stop or a clear.
    static QString transcriptText(QWidget *root)
    {
        QString out;
        const QList<QTextBrowser *> browsers = root->findChildren<QTextBrowser *>();
        for (QTextBrowser *browser : browsers) {
            out += browser->toPlainText();
            out += QLatin1Char('\n');
        }
        return out;
    }

    static QLabel *threadTitleLabel(QWidget *root)
    {
        const QList<QLabel *> labels = root->findChildren<QLabel *>();
        for (QLabel *label : labels) {
            if (label->text() == u"New Thread"_s) {
                return label;
            }
        }
        return nullptr;
    }

    // The assistant card whose text contains the needle, so a test can tell
    // two turns' cards apart and compare their places in the transcript.
    static QWidget *assistantCardContaining(QWidget *root, const QString &needle)
    {
        const QList<QWidget *> cards = root->findChildren<QWidget *>(u"assistantCard"_s);
        for (QWidget *card : cards) {
            for (QTextBrowser *browser : card->findChildren<QTextBrowser *>()) {
                if (browser->toPlainText().contains(needle)) {
                    return card;
                }
            }
        }
        return nullptr;
    }

    // The streaming pulse dot in an assistant card's header. Freeze hides it,
    // so a card that still shows it after the turn ended was never frozen.
    static bool pulseVisibleIn(QWidget *card)
    {
        for (QLabel *label : card->findChildren<QLabel *>()) {
            if (label->text() == u"●"_s) {
                return label->isVisible();
            }
        }
        return false;
    }

    // Where a card sits in the transcript's layout, so ordering between two
    // cards can be asserted.
    static int transcriptIndexOf(QWidget *card)
    {
        if (!card || !card->parentWidget() || !card->parentWidget()->layout()) {
            return -1;
        }
        return card->parentWidget()->layout()->indexOf(card);
    }

    std::unique_ptr<QTemporaryDir> m_workspace;
    QProcess *m_server = nullptr;
    QString m_portFile;
    int m_port = 0;

    // One fresh widget per turn, wired to the mock server the same way the
    // widget is wired to a real provider.
    ChatWidget *makeWidget(bool structuredThinking = false)
    {
        auto *widget = new ChatWidget;
        widget->resize(900, 700);
        widget->show();

        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = u"http://127.0.0.1:%1/v1"_s.arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"code"_s;
        // The stop must be able to fire while the command runs, so the bash
        // call from the mock is approved without a prompt.
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.sandbox = SandboxProfile::Off;
        settings.structuredThinking = structuredThinking;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;
        settings.maxModelRequests = 5;
        settings.maxToolCalls = 20;
        widget->setSettings(settings);
        widget->agent()->setWorkspace(m_workspace->path());
        return widget;
    }

private Q_SLOTS:
    void initTestCase()
    {
        // The widget autosaves its transcript, so without a scratch database
        // every test run would drop mock prompts into the user's real
        // conversation history.
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
        m_server->start(u"python3"_s, QStringList{script, m_portFile});
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

    // A stop pressed while a command is still running has to end the turn
    // long before the command would have, and settle the card it started.
    void stopEndsATurnWhileItsCommandRuns()
    {
        std::unique_ptr<ChatWidget> widget(makeWidget());

        widget->ask(u"Run a long command."_s);
        QVERIFY2(waitFor([&] {
            return widget->agent()->isBusy();
        }), "the turn never started");

        // The mock issues `sleep 30`; the card must say it is running before
        // there is anything to stop.
        ToolCallWidget *card = nullptr;
        QVERIFY2(waitFor([&] {
            card = widget->findChild<ToolCallWidget *>(u"ToolCallWidget_call_sleep"_s);
            return card && card->isRunning();
        }), "the mock never started the long command");

        QPushButton *stop = sendButton(widget.get());
        QVERIFY2(stop, "composer send button not found");
        QCOMPARE(stop->text(), u"\u25A0"_s);
        QCOMPARE(stop->toolTip(), u"Stop response"_s);
        QVERIFY2(stop->isEnabled(), "the stop button is disabled while the command runs");

        stop->click();

        // The command would hold the turn for thirty seconds; the stop has
        // to end it in a fraction of that.
        QVERIFY2(waitFor([&] {
            return !widget->agent()->isBusy();
        }, 8000), "the stop did not end the turn promptly");
        QVERIFY2(!card->isRunning(), "the card still runs after the turn was stopped");

        // The composer is back to offering a send, not a stop.
        QPushButton *send = sendButton(widget.get());
        QVERIFY2(send, "composer send button not found");
        QCOMPARE(send->text(), u"\u25B2"_s);
        QCOMPARE(send->toolTip(), u"Send message"_s);
    }

    // A stop pressed mid-stream has to cut the answer short without taking
    // the part that already arrived off the screen.
    void stopKeepsThePartialAnswerOnScreen()
    {
        std::unique_ptr<ChatWidget> widget(makeWidget());

        widget->ask(u"Type slowly."_s);
        QVERIFY2(waitFor([&] {
            return widget->agent()->isBusy();
        }), "the turn never started");

        // Wait until some of the slow answer has been typed into the chat.
        QVERIFY2(waitFor([&] {
            return transcriptText(widget.get()).contains(u"Streaming line 0"_s);
        }), "the mock never streamed any text");

        QPushButton *stop = sendButton(widget.get());
        QVERIFY2(stop, "composer send button not found");
        QVERIFY2(stop->isEnabled(), "the stop button is disabled mid-stream");

        stop->click();

        // The full stream would take six seconds; the stop has to cut it
        // short well before it plays out.
        QVERIFY2(waitFor([&] {
            return !widget->agent()->isBusy();
        }, 3000), "the stop did not cut the slow stream short");

        // The part that arrived before the stop stays on screen: it is the
        // only record of an answer the user deliberately cut short.
        const QString text = transcriptText(widget.get());
        QVERIFY2(text.contains(u"Streaming line 0"_s), qPrintable(text));
    }

    // A stop has to freeze the live card outright: the reasoning pacer and
    // the coalesced stream render must not keep typing into it after Stop,
    // and the streaming pulse has to go out with the same breath.
    void stopFreezesTheLiveCardOutright()
    {
        std::unique_ptr<ChatWidget> widget(makeWidget(true));

        widget->ask(u"Think slowly."_s);
        QVERIFY2(waitFor([&] {
            return widget->agent()->isBusy();
        }), "the turn never started");

        // The mock paces its hidden reasoning over five seconds; the stop
        // must land while that reveal is still running.
        QVERIFY2(waitFor([&] {
            return transcriptText(widget.get()).contains(u"Thought 0"_s);
        }), "the mock never streamed any reasoning");

        QPushButton *stop = sendButton(widget.get());
        QVERIFY2(stop && stop->isEnabled(), "the stop button was not available mid-reasoning");
        stop->click();

        QVERIFY2(waitFor([&] {
            return !widget->agent()->isBusy();
        }, 3000), "the stop did not end the reasoning turn");

        QWidget *card = assistantCardContaining(widget.get(), u"Thought 0"_s);
        QVERIFY2(card, "the reasoning card was not found after the stop");
        QVERIFY2(!pulseVisibleIn(card),
                 "the streaming pulse is still on a card whose turn was stopped");

        // The frozen card is final: nothing more may be typed into it.
        const QString frozen = transcriptText(widget.get());
        QTest::qWait(800);
        QCOMPARE(transcriptText(widget.get()), frozen);
    }

    // The turn after a stopped one must stream into a fresh card at the
    // bottom of the transcript. A stopped turn that never froze its card
    // leaves it active, and the next answer lands inside that stale card --
    // in the middle of the thread, above the newer user message.
    void nextTurnAnswersInACardBelowTheStoppedOne()
    {
        std::unique_ptr<ChatWidget> widget(makeWidget());

        widget->ask(u"Type slowly."_s);
        QVERIFY2(waitFor([&] {
            return transcriptText(widget.get()).contains(u"Streaming line 0"_s);
        }), "the mock never streamed any text");

        QPushButton *stop = sendButton(widget.get());
        QVERIFY2(stop && stop->isEnabled(), "the stop button was not available mid-stream");
        stop->click();
        QVERIFY2(waitFor([&] {
            return !widget->agent()->isBusy();
        }, 3000), "the stop did not end the turn");

        QWidget *stoppedCard = assistantCardContaining(widget.get(), u"Streaming line 0"_s);
        QVERIFY2(stoppedCard, "the stopped turn's card was not found");

        widget->ask(u"Say something."_s);
        QVERIFY2(waitFor([&] {
            return !widget->agent()->isBusy();
        }), "the follow-up turn never finished");

        // The follow-up answer is on screen, and it is in its own card below
        // the stopped one -- not typed into the stale card in the middle.
        QWidget *followUpCard = assistantCardContaining(widget.get(),
                                                        u"Both sub-agents reported back."_s);
        QVERIFY2(followUpCard, "the follow-up answer card was not found");
        QVERIFY2(followUpCard != stoppedCard,
                 "the follow-up turn streamed into the stopped turn's card");
        QVERIFY2(transcriptIndexOf(followUpCard) > transcriptIndexOf(stoppedCard),
                 "the follow-up answer appeared above the stopped turn's card");
    }

    // The new-thread button drops the whole conversation -- the partial
    // answer of a stopped turn included -- and leaves a chat that can carry
    // the next turn.
    void newThreadButtonClearsAStoppedTurnAndKeepsWorking()
    {
        std::unique_ptr<ChatWidget> widget(makeWidget());

        widget->ask(u"Type slowly."_s);
        QVERIFY2(waitFor([&] {
            return transcriptText(widget.get()).contains(u"Streaming line 0"_s);
        }), "the mock never streamed any text");

        QPushButton *newThread = newThreadButton(widget.get());
        QVERIFY2(newThread, "the new-thread button was not found");
        newThread->click();

        QVERIFY2(waitFor([&] {
            return !widget->agent()->isBusy();
        }), "the new-thread button did not stop the running turn");

        // The transcript was rebuilt from the cleared agent state, so the
        // aborted answer is gone rather than sitting in a cleared chat.
        QVERIFY2(!transcriptText(widget.get()).contains(u"Streaming line"_s),
                 "the stopped turn's answer survived the new thread");
        QVERIFY(widget->findChildren<ToolCallWidget *>().isEmpty());
        QVERIFY2(threadTitleLabel(widget.get()), "the thread title did not reset to a new thread");
        QVERIFY(widget->agent()->messages().isEmpty());

        // The fresh thread has to work: a plain turn through the same mock
        // answers with prose, and lands in the rebuilt transcript.
        widget->ask(u"Say something."_s);
        QVERIFY2(waitFor([&] {
            return !widget->agent()->isBusy();
        }), "the follow-up turn on the new thread never finished");
        QVERIFY2(transcriptText(widget.get()).contains(u"Both sub-agents reported back."_s),
                 qPrintable(transcriptText(widget.get())));
    }
};

QTEST_MAIN(TestStopBehavior)
#include "test_stopbehavior.moc"
