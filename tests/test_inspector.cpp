#include "agentloop.h"
#include "inspectorpanel.h"
#include "types.h"

#include <QApplication>
#include <QTabWidget>
#include <QTest>
#include <QTemporaryDir>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestInspector : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void panelShowsEveryReadOnlyTabAndRefreshes()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"code"_s;
        settings.checkpointsEnabled = false;
        settings.loadAgentRules = false;
        agent.setSettings(settings);
        agent.setWorkspace(workspace.path());

        InspectorPanel panel(&agent);
        auto *tabs = panel.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 7);
        QStringList titles;
        for (int i = 0; i < tabs->count(); ++i) {
            titles.append(tabs->tabText(i));
        }
        QVERIFY(titles.contains(u"Context"_s));
        QVERIFY(titles.contains(u"Tools"_s));
        QVERIFY(titles.contains(u"Rules & Memory"_s));
        QVERIFY(titles.contains(u"Hooks & Skills"_s));
        QVERIFY(titles.contains(u"Trace"_s));
        QVERIFY(titles.contains(u"Cost"_s));
        QVERIFY(titles.contains(u"Rewind"_s));

        // Refresh is read-only: no turn, no checks, no restore.
        panel.refresh();
        QVERIFY(!agent.isBusy());
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    TestInspector test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_inspector.moc"
