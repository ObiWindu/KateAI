#include "acpclient.h"
#include "agentloop.h"
#include "chattheme.h"
#include "chatwidget.h"
#include "sessionstoreisolation.h"
#include "types.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPoint>
#include <QPushButton>
#include <QMetaObject>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestOverflow : public QObject
{
    Q_OBJECT

private:
    static QString mockAgentPath()
    {
        QDir dir(QDir::current());
        for (int i = 0; i < 5; ++i) {
            const QString candidate = dir.absoluteFilePath(u"tests/mock_acp_agent.py"_s);
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
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QTest::qWait(10);
        }
        return true;
    }

    static QPushButton *modelSelectorButton(QWidget *root)
    {
        const QList<QPushButton *> buttons = root->findChildren<QPushButton *>();
        for (QPushButton *button : buttons) {
            if (button->text().contains(QChar(0x25BE))) {
                return button;
            }
        }
        return nullptr;
    }

    static QListWidget *findList(QWidget *root, const QString &text)
    {
        const QList<QListWidget *> lists = root->findChildren<QListWidget *>();
        for (QListWidget *list : lists) {
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->text().contains(text)) {
                    return list;
                }
            }
        }
        return nullptr;
    }

    // Two lists live in the popup: the installed ACP agents and the model
    // results. Both carry an object name, because identifying them by the rows
    // they hold does not work: the model results show a "Models for <agent>"
    // header that mentions the agent too.
    static QListWidget *agentsList(QWidget *root)
    {
        QFrame *popup = root->findChild<QFrame *>(u"modelPopup"_s);
        return popup ? popup->findChild<QListWidget *>(u"acpAgents"_s) : nullptr;
    }

    static QString listText(QListWidget *list)
    {
        QString out;
        for (int i = 0; i < list->count(); ++i) {
            out += list->item(i)->text();
            out += QLatin1Char('\n');
        }
        return out;
    }

    static QListWidget *modelsList(QWidget *root)
    {
        QFrame *popup = root->findChild<QFrame *>(u"modelPopup"_s);
        return popup ? popup->findChild<QListWidget *>(u"modelResults"_s) : nullptr;
    }

    static QList<QListWidget *> popupLists(QWidget *root)
    {
        QFrame *popup = root->findChild<QFrame *>(u"modelPopup"_s);
        return popup ? popup->findChildren<QListWidget *>() : QList<QListWidget *>();
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_agent = mockAgentPath();
        QVERIFY2(!m_agent.isEmpty(), "could not locate tests/mock_acp_agent.py");
    }

    // The composer's context readout is a percentage of the window, not an
    // empty chip: whatever the colours say, the number has to be there.
    static QPushButton *sendButton(QWidget *root)
    {
        // The composer's single round button morphs between send and stop; it
        // is the only one carrying either tooltip.
        for (QPushButton *button : root->findChildren<QPushButton *>()) {
            if (button->toolTip() == u"Send message"_s || button->toolTip() == u"Stop response"_s) {
                return button;
            }
        }
        return nullptr;
    }

    void sendButtonSwitchesToStopWhileTheAgentWorks()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        Settings settings;
        settings.provider = Provider::Acp;
        settings.apiFormat = ApiFormat::AcpNative;
        settings.acpAgentId = u"mock-perm"_s;
        settings.acpCommand = u"python3"_s;
        settings.acpArgs = u'"' + m_agent + u"\" permission"_s;
        settings.permissionMode = PermissionMode::Ask;
        settings.sandbox = SandboxProfile::Off;
        widget.setSettings(settings);

        // A turn cannot start without a workspace, and the widget's own view
        // normally supplies one.
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        widget.agent()->setWorkspace(workspace.path());

        QPushButton *send = sendButton(&widget);
        QVERIFY2(send, "composer send button not found");
        QCOMPARE(send->text(), u"▲"_s);
        QCOMPARE(send->toolTip(), u"Send message"_s);
        QVERIFY(!send->isEnabled());

        // The mock agent asks for a tool permission and waits for the answer, so
        // the turn stays open for as long as this test needs.
        widget.ask(u"write something"_s);
        QVERIFY2(waitFor([&] { return widget.agent()->isBusy(); }), "agent never started working");

        // While it works, the button has to be the stop button: red, filled and
        // tappable. A composer that keeps offering "Send" leaves the user with
        // no way to stop a turn that is already running.
        QCOMPARE(send->text(), u"■"_s);
        QCOMPARE(send->toolTip(), u"Stop response"_s);
        QVERIFY2(send->isEnabled(), "stop button is disabled while the agent works");

        QApplication::processEvents();
        QTest::qWait(50);
    }

    void sendButtonSwitchesToStopWhileAnHttpTurnRuns()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        // A server that accepts the request and answers nothing: the request
        // stays in flight for as long as this check needs, which is exactly the
        // window where the user has to be offered a stop button.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        widget.agent()->setWorkspace(workspace.path());

        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = u"http://127.0.0.1:%1/v1"_s.arg(port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.permissionMode = PermissionMode::Ask;
        settings.sandbox = SandboxProfile::Off;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;
        settings.maxModelRequests = 5;
        settings.maxToolCalls = 20;
        widget.setSettings(settings);

        QPushButton *send = sendButton(&widget);
        QVERIFY2(send, "composer send button not found");
        QCOMPARE(send->text(), u"▲"_s);
        QCOMPARE(send->toolTip(), u"Send message"_s);

        widget.ask(u"do something long"_s);
        QVERIFY2(waitFor([&] { return widget.agent()->isBusy(); }, 10000), "the turn never started");

        QCOMPARE(send->text(), u"■"_s);
        QCOMPARE(send->toolTip(), u"Stop response"_s);
        QVERIFY2(send->isEnabled(), "the stop button is disabled while the agent works");

        QApplication::processEvents();
        QTest::qWait(50);
    }

    // The thinking pill's brain glyph is a colour emoji, and Qt renders a widget
    // that carries a QGraphicsOpacityEffect through an offscreen surface where
    // those glyphs do not survive: the pill came out blank or hollow. The pulse
    // now dims the stylesheet instead, so the pill must carry no graphics
    // effect and the glyph must sit in a regular-weight span rather than the
    // pill's 600 weight, which emoji fonts rarely have.
    void thinkingIndicatorPaintsTheBrainWithoutAnOpacityEffect()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        QLabel *pill = widget.findChild<QLabel *>(u"thinkingIndicator"_s);
        QVERIFY2(pill, "the thinking pill was not found in the chat");
        // The brain is above the BMP, so the whole surrogate pair has to be in
        // the text: QChar(0x1F9E0) alone would be a lone surrogate half.
        QVERIFY2(pill->text().contains(u"\U0001F9E0"_s), qPrintable(pill->text()));
        QVERIFY2(pill->textFormat() == Qt::RichText, "the pill needs rich text to carry the span");
        QVERIFY2(pill->text().contains(u"font-weight: 400"_s), qPrintable(pill->text()));
        QVERIFY2(!pill->graphicsEffect(), "an opacity effect on the pill breaks the emoji glyph");
        // Bare text: the sheet is the theme's own, with no box and no fill.
        QVERIFY2(pill->styleSheet() == ChatTheme::thinkingPill(), qPrintable(pill->styleSheet()));
        QVERIFY2(!pill->styleSheet().contains(u"background-color"_s), qPrintable(pill->styleSheet()));
        QVERIFY2(!pill->styleSheet().contains(u"border: 1px"_s), qPrintable(pill->styleSheet()));
    }

    // The indicators float over the transcript's very bottom left corner:
    // bare small text pinned to the bottom edge, not a row in the flow, so
    // they never push the conversation around and cannot be scrolled away.
    void indicatorsOverlayTheBottomLeftCorner()
    {
        isolateSessionStore();
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        auto *scroll = widget.findChild<QScrollArea *>();
        QVERIFY(scroll);
        auto *row = widget.findChild<QWidget *>(u"indicatorsContainer"_s);
        QVERIFY2(row, "the indicators row was not found");
        QVERIFY2(row->parentWidget() == scroll, "the indicators row is not an overlay on the transcript");
        QVERIFY2(!row->isVisible(), "the strip shows while nothing is running");

        // A thinking delta turns the strip on; it must sit flush with the
        // bottom left corner of the transcript viewport.
        Q_EMIT widget.agent()->thinkingDelta(u"considering"_s);
        QVERIFY(QTest::qWaitFor([&row] {
            return row->isVisible();
        }));
        QTest::qWait(60);
        const QPoint topLeft = row->mapTo(scroll, QPoint(0, 0));
        QVERIFY2(topLeft.x() <= 8,
                 qPrintable(QStringLiteral("the strip is not at the left edge: %1+%2 of %3")
                                .arg(topLeft.x()).arg(row->width()).arg(scroll->width())));
        QVERIFY2(topLeft.y() + row->height() >= scroll->height() - 8,
                 qPrintable(QStringLiteral("the strip is not at the bottom edge: %1+%2 of %3")
                                .arg(topLeft.y()).arg(row->height()).arg(scroll->height())));
        // Smaller than the boxed pill it replaces (~30px tall with padding).
        QVERIFY2(row->height() <= 16,
                 qPrintable(QStringLiteral("the strip did not shrink: %1px").arg(row->height())));

        // Clicks pass through to the transcript underneath.
        QVERIFY2(row->testAttribute(Qt::WA_TransparentForMouseEvents),
                 "the strip swallows clicks meant for the transcript");
    }

    // The reasoning browser's height is pinned to its document, and the
    // document wraps at the browser's width. A fit measured before the layout
    // gave the browser its real width used to stay pinned, so a block whose
    // text re-wrapped to fewer lines showed empty space below it.
    void thinkingHeightFollowsTheLaidOutWidth()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        Q_EMIT widget.agent()->thinkingDelta(
            u"a line of reasoning that is long enough to wrap at narrow widths, so the pinned height has to follow the width the layout actually gives the browser"_s);

        auto *browser = widget.findChild<QTextBrowser *>(u"reasoningBrowser"_s);
        QVERIFY2(browser, "the reasoning browser was not found");
        // The pacer reveals the text over ticks; wait for it to be on screen.
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 4000 && !(browser->isVisible() && browser->height() >= 48)) {
            QTest::qWait(20);
        }
        QVERIFY2(browser->isVisible() && browser->height() >= 48, "the reasoning browser never settled");

        const auto fitsDocument = [browser] {
            const int width = std::max(120, browser->viewport()->width());
            browser->document()->setTextWidth(width);
            return std::max(48, qCeil(browser->document()->size().height()) + 20);
        };
        QVERIFY2(qAbs(browser->height() - fitsDocument()) <= 2,
                 "the pinned height does not match the document at the real width");

        // The panel narrows: the text wraps to more lines and the pinned
        // height has to follow, or the block shows clipped text or empty
        // space below it.
        widget.resize(420, 700);
        QTest::qWait(120);
        QVERIFY2(qAbs(browser->height() - fitsDocument()) <= 2,
                 "the pinned height did not follow the resize");
    }

    // An ACP agent reports a running tool through in-progress tool_call_update
    // notifications. Before those were forwarded, the card sat empty for the
    // whole command and filled in only once it ended.
    void acpToolStreamsItsExecutionInsteadOfWaitingForTheEnd()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        Settings settings;
        settings.provider = Provider::Acp;
        settings.apiFormat = ApiFormat::AcpNative;
        settings.acpAgentId = u"mock-stream"_s;
        settings.acpCommand = u"python3"_s;
        settings.acpArgs = u'"' + m_agent + u"\" toolstream"_s;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.sandbox = SandboxProfile::Off;
        widget.setSettings(settings);

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        widget.agent()->setWorkspace(workspace.path());

        QStringList order;
        QString streamed;
        connect(widget.agent(), &AgentLoop::toolProgress, &widget, [&](const QString &, const QString &partial) {
            streamed = partial;
            order << u"progress"_s;
        });
        bool finished = false;
        connect(widget.agent(), &AgentLoop::toolFinished, &widget, [&](const ToolResult &) {
            order << u"finished"_s;
            finished = true;
        });

        widget.ask(u"build"_s);
        QVERIFY2(waitFor([&] { return finished; }, 20000), "the tool never finished");
        QApplication::processEvents();

        // Live output has to reach the card before the result does, or the card
        // shows nothing until the tool ends.
        QVERIFY2(order.contains(u"progress"_s), "no live output reached the transcript");
        QVERIFY2(order.indexOf(u"progress"_s) < order.indexOf(u"finished"_s),
                 "the stream only arrived after the tool had already finished");
        QVERIFY2(streamed.contains(u"scanning dependencies"_s), qPrintable(streamed));
    }

    void contextPercentageIsShown()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        Settings settings;
        settings.provider = Provider::Grok;
        settings.grokApiKey = u"test-key"_s;
        settings.grokModel = u"grok-4"_s;
        settings.permissionMode = PermissionMode::Ask;
        settings.sandbox = SandboxProfile::Off;
        widget.setSettings(settings);

        QLabel *readout = widget.findChild<QLabel *>(u"tokenCount"_s);
        QVERIFY2(readout, "context readout not found in the composer");
        QVERIFY2(readout->isVisible(), "context readout is hidden");
        QVERIFY2(QRegularExpression(u"^\\d{1,3}% ctx$"_s).match(readout->text()).hasMatch(),
                 qPrintable(u"context readout text was '%1'"_s.arg(readout->text())));
    }

    void installedAgentClickShowsModels()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        Settings settings;
        settings.provider = Provider::Acp;
        settings.apiFormat = ApiFormat::AcpNative;
        settings.acpAgentId = u"mock-one"_s;
        settings.acpCommand = u"python3"_s;
        settings.acpArgs = u'"' + m_agent + u"\" models"_s;
        settings.permissionMode = PermissionMode::Ask;
        settings.sandbox = SandboxProfile::Off;

        QJsonArray installed;
        QJsonObject one;
        one.insert(u"id"_s, u"mock-one"_s);
        one.insert(u"name"_s, u"Mock One"_s);
        one.insert(u"description"_s, u"first mock agent"_s);
        one.insert(u"version"_s, u"1.0"_s);
        one.insert(u"command"_s, u"python3"_s);
        one.insert(u"args"_s, QJsonArray{m_agent, u"models"_s});
        installed.append(one);
        QJsonObject two;
        two.insert(u"id"_s, u"mock-two"_s);
        two.insert(u"name"_s, u"Mock Two"_s);
        two.insert(u"description"_s, u"second mock agent"_s);
        two.insert(u"version"_s, u"1.0"_s);
        two.insert(u"command"_s, u"python3"_s);
        two.insert(u"args"_s, QJsonArray{m_agent, u"legacy-models"_s});
        installed.append(two);
        settings.acpInstalledAgents = QString::fromUtf8(QJsonDocument(installed).toJson(QJsonDocument::Compact));

        widget.setSettings(settings);

        QPushButton *selector = modelSelectorButton(&widget);
        QVERIFY2(selector, "model selector chip not found");
        QTest::mouseClick(selector, Qt::LeftButton);
        QCOMPARE(popupLists(&widget).size(), 2);
        QVERIFY2(waitFor([&] { return agentsList(&widget) != nullptr; }),
                 "model popup never filled its lists");

        // The current agent's models arrive on the Models tab in normal mode.
        QVERIFY2(waitFor([&] {
                     QListWidget *list = modelsList(&widget);
                     return list && listText(list).contains(u"grok-4.7"_s);
                 }),
                 "current agent models never appeared on the Models tab");

        // A real click, not itemClicked invoked by hand: the release that
        // delivers the click is still using the row, and rebuilding the list
        // inside that release used to free it before the models were fetched.
        QString clickDiag;
        auto clickAgent = [&](const QString &name) -> bool {
            QFrame *popup = widget.findChild<QFrame *>(u"modelPopup"_s);
            QTabWidget *tabs = popup ? popup->findChild<QTabWidget *>() : nullptr;
            if (!tabs) {
                clickDiag = u"no tabs"_s;
                return false;
            }
            tabs->setCurrentIndex(1);
            if (!waitFor([&] { return agentsList(&widget) && agentsList(&widget)->isVisible()
                               && agentsList(&widget)->count() > 0; })) {
                QListWidget *agents = agentsList(&widget);
                clickDiag = u"agents not shown count=%1 visible=%2"_s
                                .arg(agents ? agents->count() : -1)
                                .arg(agents && agents->isVisible());
                return false;
            }
            QListWidget *agents = agentsList(&widget);
            QListWidgetItem *target = nullptr;
            for (int i = 0; i < agents->count(); ++i) {
                if (agents->item(i)->text().contains(name)) {
                    target = agents->item(i);
                    break;
                }
            }
            if (!target) {
                clickDiag = u"missing '%1' in:\n%2"_s.arg(name, listText(agents));
                return false;
            }
            agents->scrollToItem(target);
            const QRect rect = agents->visualItemRect(target);
            if (!rect.isValid() || rect.isEmpty()) {
                QString rows;
                for (int i = 0; i < agents->count(); ++i) {
                    const QRect r = agents->visualItemRect(agents->item(i));
                    rows += u" [%1] hint=%2 rect=%3x%4@%5,%6 %7"_s
                                .arg(i)
                                .arg(agents->sizeHintForRow(i))
                                .arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y())
                                .arg(agents->item(i)->text().left(40).replace(u'\n', u' '));
                }
                clickDiag = u"empty rect viewport %1x%2 hidden=%3 rows:%4"_s
                                .arg(agents->viewport()->width())
                                .arg(agents->viewport()->height())
                                .arg(agents->isHidden())
                                .arg(rows);
                return false;
            }
            QTest::mouseClick(agents->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
            return true;
        };

        QVERIFY2(clickAgent(u"Mock One"_s), qPrintable(clickDiag));

        QVERIFY2(waitFor([&] {
                     return listText(modelsList(&widget)).contains(u"grok-4.7"_s);
                 }),
                 "models for the current agent never switched into the model view");

        // And a different installed agent is fetched too. Its row lives on the
        // ACP Agents tab, so leave the model view the first click switched to:
        // the tab change restores the agents list.
        QFrame *popup = widget.findChild<QFrame *>(u"modelPopup"_s);
        QTabWidget *tabs = popup ? popup->findChild<QTabWidget *>() : nullptr;
        QVERIFY2(tabs, "model picker tab widget not found");
        QMetaObject::invokeMethod(tabs, "setCurrentIndex", Q_ARG(int, 1));
        QVERIFY2(waitFor([&] { return agentsList(&widget)->count() > 0; }),
                 "ACP Agents tab never came back after the model view");

        QVERIFY2(clickAgent(u"Mock Two"_s), qPrintable(clickDiag));

        QVERIFY2(waitFor([&] {
                     QListWidget *list = modelsList(&widget);
                     return list && listText(list).contains(u"gemini-2.5-flash"_s);
                 }),
                 "models for a second installed agent were never displayed");
    }

private:
    QString m_agent;
};

// Explicit main rather than QTEST_MAIN: the widgets built here autosave their
// transcript into SessionStore while they are destroyed, and the store override
// has to be in place before the first widget exists. Without it a test run
// makes a mock prompt the active conversation, and Kate opens on that thread
// instead of the welcome screen the next time the plugin starts.
int main(int argc, char **argv)
{
    isolateSessionStore();

    QApplication app(argc, argv);
    TestOverflow tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_overflow.moc"
