#include "agentloop.h"
#include "types.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QEventLoop>
#include <QNetworkProxyFactory>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

// Drives a real AgentLoop against a mock OpenAI-compatible endpoint so the
// sub-agent scheduling can be observed without a real provider.
class TestSubtaskParallel : public QObject
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

    // Spins the event loop while `predicate` holds.
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

private Q_SLOTS:
    void initTestCase()
    {
        // The mock server is on localhost. If the environment exports proxy
        // variables, Qt would try to route the request through the proxy and
        // every request would fail, so the test talks to the network directly.
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

    void independentSubtasksRunAtTheSameTime()
    {
        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"orchestrator"_s;
        settings.planMode = false;
        settings.maxToolCalls = 20;
        settings.maxModelRequests = 10;
        settings.maxParallelSubtasks = 3;
        settings.maxSubtaskDepth = 1;
        settings.subtaskTimeoutMs = 30000;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;

        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());
        QVERIFY(agent.activeToolAccess().allows(u"new_task"_s));

        int peakConcurrency = 0;
        QObject::connect(&agent, &AgentLoop::subtaskStarted, &agent, [&agent, &peakConcurrency] {
            peakConcurrency = qMax(peakConcurrency, agent.runningSubtaskCount());
        });
        QObject::connect(&agent, &AgentLoop::subtaskFinished, &agent, [&agent, &peakConcurrency](const QString &, bool) {
            peakConcurrency = qMax(peakConcurrency, agent.runningSubtaskCount());
        });

        bool finished = false;
        QString failure;
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] {
            finished = true;
        });
        QObject::connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) {
            failure = error;
        });

        QElapsedTimer timer;
        timer.start();
        agent.start(u"Split this across two agents."_s);

        QVERIFY(waitFor([&finished] {
            return finished;
        }, 40000));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(agent.runningSubtaskCount() == 0);

        // The whole point: both sub-agents were alive at once. With
        // serialized dispatch this could never exceed 1.
        QCOMPARE(peakConcurrency, 2);

        // And both answers came back to the orchestrator.
        bool sawAnswer = false;
        for (const ChatMessage &message : agent.messages()) {
            if (message.role == ChatMessage::Role::Tool && message.content.contains(u"sub-agent"_s)) {
                sawAnswer = true;
            }
        }
        QVERIFY(sawAnswer);
    }

    void concurrencyLimitIsEnforced()
    {
        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"orchestrator"_s;
        settings.maxParallelSubtasks = 1;
        settings.maxSubtaskDepth = 1;
        settings.subtaskTimeoutMs = 30000;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;

        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());

        int peakConcurrency = 0;
        QObject::connect(&agent, &AgentLoop::subtaskStarted, &agent, [&agent, &peakConcurrency] {
            peakConcurrency = qMax(peakConcurrency, agent.runningSubtaskCount());
        });

        bool finished = false;
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] {
            finished = true;
        });

        agent.start(u"Split this across two agents."_s);
        QVERIFY(waitFor([&finished] {
            return finished;
        }, 40000));

        // With one slot the second subtask has to wait its turn.
        QVERIFY(peakConcurrency <= 1);
    }

    void cancellingOneSubtaskStopsOnlyThatAgent()
    {
        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"orchestrator"_s;
        settings.maxParallelSubtasks = 3;
        settings.maxSubtaskDepth = 1;
        settings.subtaskTimeoutMs = 30000;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;

        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());

        QString firstId;
        QObject::connect(&agent, &AgentLoop::subtaskStarted, &agent, [&firstId](const QString &taskId, const QString &, const QString &, const QString &, const QString &) {
            if (firstId.isEmpty()) {
                firstId = taskId;
            }
        });

        agent.start(u"Split this across two agents."_s);
        QVERIFY(waitFor([&firstId] {
            return !firstId.isEmpty();
        }, 20000));
        QCOMPARE(agent.runningSubtaskCount(), 2);

        agent.cancelSubtask(firstId);
        QVERIFY(!agent.runningSubtaskIds().contains(firstId));
        QVERIFY(agent.runningSubtaskCount() == 1);

        agent.abort();
        QCOMPARE(agent.runningSubtaskCount(), 0);
    }

    void abortStopsEveryRunningSubtask()
    {
        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"orchestrator"_s;
        settings.maxParallelSubtasks = 3;
        settings.maxSubtaskDepth = 1;
        settings.subtaskTimeoutMs = 30000;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.enableAutoRetry = false;

        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());

        agent.start(u"Split this across two agents."_s);
        QVERIFY(waitFor([&agent] {
            return agent.runningSubtaskCount() == 2;
        }, 20000));

        agent.abort();
        QCOMPARE(agent.runningSubtaskCount(), 0);
    }

private:
    std::unique_ptr<QTemporaryDir> m_workspace;
    QProcess *m_server = nullptr;
    QString m_portFile;
    int m_port = 0;
};

QTEST_MAIN(TestSubtaskParallel)
#include "test_subtaskparallel.moc"