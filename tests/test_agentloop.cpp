#include "agentloop.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestAgentLoop : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void workspaceSetupIsLazy()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QFile source(dir.filePath(u"main.cpp"_s));
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Text));
        source.write("int main() { return 0; }\n");
        source.close();

        AgentLoop agent;
        agent.setWorkspace(dir.path());

        // setWorkspace() runs during Kate plugin construction. It must only
        // establish the sandbox/workspace and must not recursively scan the
        // project until an actual agent turn is started.
        QVERIFY(agent.getProjectGraph() != nullptr);
        QCOMPARE(agent.getProjectGraph()->getWorkspacePath(), QDir::cleanPath(dir.path()));
        QCOMPARE(agent.getProjectGraph()->getNodeCount(), 0);
    }

    void emptyWorkspaceDoesNotInventCurrentDirectory()
    {
        AgentLoop agent;
        agent.setWorkspace({});
        QVERIFY(agent.getProjectGraph() != nullptr);
        QVERIFY(agent.getProjectGraph()->getWorkspacePath().isEmpty());
        QCOMPARE(agent.getProjectGraph()->getNodeCount(), 0);
    }
};

QTEST_GUILESS_MAIN(TestAgentLoop)
#include "test_agentloop.moc"
