#include "acpclient.h"
#include "agentloop.h"
#include "chatwidget.h"
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
#include <QPushButton>
#include <QMetaObject>
#include <QRegularExpression>
#include <QTabWidget>
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

        // Clicking the *current* agent in the ACP Agents tab must show its models.
        QListWidgetItem *target = nullptr;
        for (int i = 0; i < agentsList(&widget)->count(); ++i) {
            if (agentsList(&widget)->item(i)->text().contains(u"Mock One"_s)) {
                target = agentsList(&widget)->item(i);
                break;
            }
        }
        QVERIFY2(target, "Mock One not listed in the ACP Agents tab");
        QMetaObject::invokeMethod(agentsList(&widget), "itemClicked", Q_ARG(QListWidgetItem *, target));

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

        QListWidgetItem *other = nullptr;
        for (int i = 0; i < agentsList(&widget)->count(); ++i) {
            if (agentsList(&widget)->item(i)->text().contains(u"Mock Two"_s)) {
                other = agentsList(&widget)->item(i);
                break;
            }
        }
        QVERIFY2(other, "Mock Two not listed in the ACP Agents tab");
        QMetaObject::invokeMethod(agentsList(&widget), "itemClicked", Q_ARG(QListWidgetItem *, other));

        QVERIFY2(waitFor([&] {
                     QListWidget *list = modelsList(&widget);
                     return list && listText(list).contains(u"gemini-2.5-flash"_s);
                 }),
                 "models for a second installed agent were never displayed");
    }

private:
    QString m_agent;
};

QTEST_MAIN(TestOverflow)
#include "test_overflow.moc"
