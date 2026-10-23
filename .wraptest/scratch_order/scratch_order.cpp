/*
 * Scratch reproduction: transcript order + crashes on stop and new chat.
 * Drives the ChatWidget the way a real turn does (through AgentLoop signals).
 */

#include "chatwidget.h"
#include "agentloop.h"
#include "settings.h"
#include "types.h"

#include <QApplication>
#include <QElapsedTimer>
#include <cstdio>
#include <QTextBrowser>
#include <cmath>
#include <QLabel>
#include <QLayout>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>
#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class ScratchOrder : public QObject
{
    Q_OBJECT

private:
    ChatWidget *m_widget = nullptr;
    std::unique_ptr<QTemporaryDir> m_workspace;

    QStringList dumpOrder(const char *tag)
    {
        auto *scroll = m_widget->findChild<QScrollArea *>();
        auto *container = scroll ? scroll->widget() : nullptr;
        auto *layout = container ? container->layout() : nullptr;
        QStringList order;
        if (layout) {
            for (int i = 0; i < layout->count(); ++i) {
                QLayoutItem *item = layout->itemAt(i);
                if (auto *w = item->widget()) {
                    QString tag = w->objectName();
                    if (!tag.isEmpty()) {
                        tag += u"["_s;
                    }
                    tag += QString::fromUtf8(w->metaObject()->className());
                    if (w->findChild<QWidget *>(u"userMessageBody"_s)) {
                        tag += u"{USER}"_s;
                    } else if (w->findChild<QWidget *>(u"assistantBrowser"_s)) {
                        tag += u"{ASSISTANT}"_s;
                    } else if (w->findChild<QWidget *>(u"thinkingBrowser"_s)) {
                        tag += u"{THINKING}"_s;
                    } else if (QString::fromUtf8(w->metaObject()->className()) == u"KateAi::ToolCallWidget"_s) {
                        tag += u"{TOOL}"_s;
                    } else if (QString::fromUtf8(w->metaObject()->className()) == u"KateAi::SubtaskWidget"_s) {
                        tag += u"{SUBTASK}"_s;
                    }
                    order << tag;
                } else {
                    order << QStringLiteral("<stretch>");
                }
            }
        }
        fprintf(stderr, "ORDER %s -> %s\n", tag, qPrintable(order.join(u" | "_s)));
        return order;
    }

    int indexOf(const QStringList &order, const QString &needle)
    {
        for (int i = 0; i < order.size(); ++i) {
            if (order.at(i).contains(needle)) {
                return i;
            }
        }
        return -1;
    }

    template<typename Predicate>
    static bool waitForScratch(Predicate predicate, int timeoutMs = 20000)
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

private Q_SLOTS:
    void run()
    {
        m_workspace = std::make_unique<QTemporaryDir>();
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

        AgentLoop *agent = m_widget->agent();

        // --- reasoning height follows the laid-out width ---
        Q_EMIT agent->userMessage(u"please do things"_s);
        QTest::qWait(30);
        Q_EMIT agent->thinkingDelta(u"pondering **the task** at hand with a line of reasoning that is long enough to wrap at narrow widths and leave empty space below when the height is stale..."_s);
        QTest::qWait(80);
        auto *browser = m_widget->findChild<QTextBrowser *>(u"reasoningBrowser"_s);
        QVERIFY2(browser, "no reasoning browser");
        // Wait until the pacer has actually revealed the block.
        QVERIFY(waitForScratch([browser] {
            return browser->isVisible() && browser->height() >= 48;
        }, 3000));
        QTest::qWait(120);
        const int width = std::max(120, browser->viewport()->width());
        browser->document()->setTextWidth(width);
        const int expected = std::max(48, qCeil(browser->document()->size().height()) + 20);
        qDebug("height %d expected %d (width %d)", browser->height(), expected, width);
        QVERIFY2(qAbs(browser->height() - expected) <= 2,
                 qPrintable(QStringLiteral("stale height %1, want %2").arg(browser->height()).arg(expected)));

        // A resize hands the browser a new width; the pinned height has to
        // follow the new wrap, or the block shows empty space below the text.
        m_widget->resize(420, 700);
        QTest::qWait(120);
        const int narrowWidth = std::max(120, browser->viewport()->width());
        browser->document()->setTextWidth(narrowWidth);
        const int expectedNarrow = std::max(48, qCeil(browser->document()->size().height()) + 20);
        qDebug("narrow height %d expected %d (width %d)", browser->height(), expectedNarrow, narrowWidth);
        QVERIFY2(qAbs(browser->height() - expectedNarrow) <= 2,
                 qPrintable(QStringLiteral("height did not follow the resize: %1, want %2")
                                .arg(browser->height())
                                .arg(expectedNarrow)));
        m_widget->resize(900, 700);
        QTest::qWait(80);

        // --- a busy turn: user, thinking, streaming, running tool ---
        Q_EMIT agent->userMessage(u"please do things"_s);
        QTest::qWait(30);
        Q_EMIT agent->thinkingDelta(u"pondering the task at hand..."_s);
        QTest::qWait(30);
        Q_EMIT agent->assistantDelta(u"Working on it: "_s);
        QTest::qWait(30);

        // A sub-agent starts first (its card goes to the intent dock), then
        // ordinary tools keep running after it -- the way a real orchestration
        // interleaves work.
        PermissionRequest sub;
        sub.toolName = u"new_task"_s;
        sub.toolCallId = u"call_sub"_s;
        sub.summary = u"[code] handle the parser"_s;
        sub.details = u"{\"description\": \"handle the parser\"}"_s;
        sub.risk = ToolRisk::Execute;
        Q_EMIT agent->toolStarted(sub);
        Q_EMIT agent->subtaskStarted(u"call_sub"_s, u"coder"_s, u"Coder"_s, u"code"_s, u"handle the parser"_s);
        QTest::qWait(30);

        PermissionRequest req;
        req.toolName = u"bash"_s;
        req.toolCallId = u"call_1"_s;
        req.summary = u"sleep 300"_s;
        req.details = u"sleep 300"_s;
        req.risk = ToolRisk::Execute;
        Q_EMIT agent->toolStarted(req);
        QTest::qWait(50);

        QStringList before = dumpOrder("before stop");
        QVERIFY(indexOf(before, u"{USER}"_s) >= 0);
        QVERIFY(indexOf(before, u"{ASSISTANT}"_s) >= 0);
        // The sub-agent's card is in the intent dock while it runs, so the
        // transcript holds: user, assistant, tool.

        // --- press stop ---
        agent->abort();
        QTest::qWait(50);
        QStringList after = dumpOrder("after stop");
        // The sub-agent started BEFORE the bash tool, so its card must retire
        // BEFORE the tool card, not glued to the end of the transcript.
        const int subAt = indexOf(after, u"{SUBTASK}"_s);
        const int toolAt = indexOf(after, u"{TOOL}"_s);
        QVERIFY2(subAt < 0 || toolAt < 0 || subAt < toolAt,
                 qPrintable(QStringLiteral("subagent card retired at %1, tool card at %2").arg(subAt).arg(toolAt)));

        // Time order: the user message must still be first among the cards.
        const int userAt = indexOf(after, u"{USER}"_s);
        const int assistantAt = indexOf(after, u"{ASSISTANT}"_s);
        QVERIFY2(userAt >= 0, "user card vanished");
        QVERIFY2(assistantAt < 0 || userAt < assistantAt, "assistant card moved above the user card");

        // --- the + button ---
        m_widget->newChat();
        QTest::qWait(50);
        dumpOrder("after newChat");

        // --- a fresh turn must still work ---
        Q_EMIT agent->userMessage(u"second turn"_s);
        QTest::qWait(30);
        QStringList second = dumpOrder("second turn");
        QVERIFY2(indexOf(second, u"{USER}"_s) >= 0, "no user card in the new conversation");

        delete m_widget;
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    ScratchOrder tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "scratch_order.moc"
