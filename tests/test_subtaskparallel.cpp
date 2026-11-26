#include "agentloop.h"
#include "documentbridge.h"
#include "harness/skills.h"
#include "sessionstore.h"
#include "test_python.h"
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
        qputenv("KATEAI_SUBAGENT_DIR", (m_workspace->path() + u"/subagents"_s).toUtf8());
        // Keep skills and their trust store inside the test tree so the
        // developer's own config cannot change what the fork test sees.
        SkillRegistry::setHomeForTests(m_workspace->path() + u"/home"_s);
        SkillRegistry::setTrustStorePath(m_workspace->path() + u"/skill-trust.json"_s);
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
            QFile f(m_portFile);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                return false;
            }
            return QString::fromUtf8(f.readAll()).trimmed().toInt() > 0;
        }, 15000));
        QFile portFile(m_portFile);
        QVERIFY(portFile.open(QIODevice::ReadOnly | QIODevice::Text));
        m_port = QString::fromUtf8(portFile.readAll()).trimmed().toInt();
        QVERIFY(m_port > 0);
    }

    void cleanup()
    {
        qunsetenv("KATEAI_SUBAGENT_DIR");
        SkillRegistry::setHomeForTests(QString());
        SkillRegistry::setTrustStorePath(QString());
        if (m_server) {
            if (m_server->state() != QProcess::NotRunning) {
                m_server->kill();
                m_server->waitForFinished(5000);
            }
            delete m_server;
            m_server = nullptr;
        }
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

        // Each run has a structured record and a readable sidechain, even
        // though the parent only received the summary prose.
        const QList<SubagentRecord> records = agent.subagents();
        QCOMPARE(records.size(), 2);
        for (const SubagentRecord &record : records) {
            QCOMPARE(record.status, u"ok"_s);
            QVERIFY(!record.summary.isEmpty());
            QVERIFY(!record.sidechainPath.isEmpty());
            const QString transcript = agent.subagentTranscript(record.taskId);
            QVERIFY(transcript.contains(u"assistant"_s));
            QVERIFY(!transcript.contains(u"No sub-agent"_s));
        }
    }

    void mutationRunsAfterItsCheckpoint()
    {
        // With checkpoints on, a mutation is parked until the snapshot - which
        // runs off the UI thread - has reported back. The turn must still
        // write the file, take exactly one checkpoint, and finish.
        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"code"_s;
        settings.planMode = false;
        settings.maxToolCalls = 20;
        settings.maxModelRequests = 10;
        settings.checkpointsEnabled = true;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;

        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());

        // write_file goes through the document bridge; without it the tool
        // fails instead of writing.
        DiskDocumentBridge bridge;
        agent.setDocumentBridge(&bridge);

        bool finished = false;
        QString failure;
        int checkpoints = 0;
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] {
            finished = true;
        });
        QObject::connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) {
            failure = error;
        });
        QObject::connect(&agent, &AgentLoop::checkpointCreated, &agent, [&checkpoints](const QString &, const QString &) {
            ++checkpoints;
        });

        agent.start(u"Write the note file."_s);

        QVERIFY(waitFor([&finished] {
            return finished;
        }, 40000));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(checkpoints, 1);
        QVERIFY(!agent.checkpoints().isEmpty());

        QFile notes(m_workspace->path() + u"/notes.txt"_s);
        QVERIFY(notes.exists());
        QVERIFY(notes.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(notes.readAll()).trimmed(), u"written by the mock model"_s);
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

        agent.start(u"Split this across two agents."_s);
        QVERIFY(waitFor([&agent] {
            return agent.runningSubtaskCount() == 2;
        }, 20000));
        const QString firstId = agent.runningSubtaskIds().isEmpty()
            ? QString()
            : agent.runningSubtaskIds().constFirst();
        QVERIFY(!firstId.isEmpty());
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

    void aForkedSkillCanAskTheUserAndResume()
    {
        QDir().mkpath(m_workspace->path() + u"/.kateai/skills/fork-ask"_s);
        QFile skill(m_workspace->path() + u"/.kateai/skills/fork-ask/SKILL.md"_s);
        QVERIFY(skill.open(QIODevice::WriteOnly | QIODevice::Text));
        skill.write("---\n"
                    "name: fork-ask\n"
                    "description: Forked skill that asks before proceeding.\n"
                    "context: fork\n"
                    "---\n"
                    "SKILL-CHILD-QUESTION\n"
                    "Ask the user before doing the work.\n");
        skill.close();

        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"code"_s;
        settings.planMode = false;
        settings.maxToolCalls = 20;
        settings.maxModelRequests = 10;
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
        QString trustError;
        QVERIFY2(agent.trustSkill(u"fork-ask"_s, &trustError), qPrintable(trustError));

        QString askedTaskId;
        QString askedCallId;
        int questions = 0;
        QObject::connect(&agent, &AgentLoop::subtaskQuestionAsked, &agent,
                         [&](const QString &taskId, const PermissionRequest &request) {
                             ++questions;
                             askedTaskId = taskId;
                             askedCallId = request.toolCallId;
                             agent.answerSubtaskQuestion(taskId, u"Yes"_s);
                         });

        bool finished = false;
        QString failure;
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] {
            finished = true;
        });
        QObject::connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) {
            failure = error;
        });

        agent.start(u"Run the fork skill."_s);
        QVERIFY2(waitFor([&finished] {
                     return finished;
                 }, 40000),
                 "the parent turn never resumed after the forked skill asked its question");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(questions, 1);
        QVERIFY2(askedCallId.startsWith(u"subtask:"_s), qPrintable(askedCallId));
        QVERIFY(!askedTaskId.isEmpty());
        QCOMPARE(agent.runningSubtaskCount(), 0);

        const QList<SubagentRecord> records = agent.subagents();
        QCOMPARE(records.size(), 1);
        QCOMPARE(records.first().agentId, u"fork-ask"_s);
        QCOMPARE(records.first().status, u"ok"_s);
        QVERIFY(!records.first().summary.isEmpty());
    }

    void aForkedSkillPermissionIsForwardedToTheParent()
    {
        QDir().mkpath(m_workspace->path() + u"/.kateai/skills/fork-write"_s);
        QFile skill(m_workspace->path() + u"/.kateai/skills/fork-write/SKILL.md"_s);
        QVERIFY(skill.open(QIODevice::WriteOnly | QIODevice::Text));
        skill.write("---\n"
                    "name: fork-write\n"
                    "description: Forked skill that writes a note.\n"
                    "context: fork\n"
                    "---\n"
                    "SKILL-WRITE-CHILD\n"
                    "Write the note.\n");
        skill.close();

        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"code"_s;
        settings.planMode = false;
        settings.maxToolCalls = 20;
        settings.maxModelRequests = 10;
        settings.maxParallelSubtasks = 1;
        settings.maxSubtaskDepth = 1;
        settings.subtaskTimeoutMs = 30000;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::Ask;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;

        DiskDocumentBridge bridge;
        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());
        agent.setDocumentBridge(&bridge);
        QString trustError;
        QVERIFY2(agent.trustSkill(u"fork-write"_s, &trustError), qPrintable(trustError));

        QString permittedCallId;
        QString permittedTool;
        int approvals = 0;
        QObject::connect(&agent, &AgentLoop::permissionNeeded, &agent,
                         [&](const PermissionRequest &request) {
                             ++approvals;
                             permittedCallId = request.toolCallId;
                             permittedTool = request.toolName;
                             agent.resolvePermission(PermissionDecision::AllowOnce,
                                                     request.toolCallId);
                         });

        bool finished = false;
        QString failure;
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] {
            finished = true;
        });
        QObject::connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) {
            failure = error;
        });

        agent.start(u"Run the fork write skill."_s);
        QVERIFY2(waitFor([&finished] {
                     return finished;
                 }, 40000),
                 "the parent turn never resumed after the forked skill requested approval");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(approvals, 1);
        QVERIFY2(permittedCallId.startsWith(u"subtask:"_s), qPrintable(permittedCallId));
        QCOMPARE(permittedTool, u"write_file"_s);

        QFile note(m_workspace->path() + u"/skill-note.txt"_s);
        QVERIFY(note.exists());
        QVERIFY(note.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(note.readAll()), u"written by the forked skill"_s);

        const QList<SubagentRecord> records = agent.subagents();
        QCOMPARE(records.size(), 1);
        QCOMPARE(records.first().status, u"ok"_s);
    }

    void aPathScopedForkedSkillStaysGatedLikeAnInlineOne()
    {
        QDir().mkpath(m_workspace->path() + u"/.kateai/skills/path-fork"_s);
        QFile skill(m_workspace->path() + u"/.kateai/skills/path-fork/SKILL.md"_s);
        QVERIFY(skill.open(QIODevice::WriteOnly | QIODevice::Text));
        skill.write("---\n"
                    "name: path-fork\n"
                    "description: Only applies under src.\n"
                    "paths: [\"src/**\"]\n"
                    "context: fork\n"
                    "---\n"
                    "Do the scoped work.\n");
        skill.close();

        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"code"_s;
        settings.maxToolCalls = 20;
        settings.maxModelRequests = 10;
        settings.maxSubtaskDepth = 1;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;

        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());
        QString trustError;
        QVERIFY2(agent.trustSkill(u"path-fork"_s, &trustError), qPrintable(trustError));

        bool finished = false;
        QString failure;
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] {
            finished = true;
        });
        QObject::connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) {
            failure = error;
        });

        agent.start(u"Run the path-scoped skill."_s);
        QVERIFY2(waitFor([&finished] {
                     return finished;
                 }, 30000),
                 "the parent turn never finished");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY2(agent.subagents().isEmpty(), "a path-scoped fork started without a matching touch");

        bool sawGate = false;
        for (const ChatMessage &message : agent.messages()) {
            if (message.role == ChatMessage::Role::Tool
                && message.content.contains(u"path-scoped"_s)) {
                sawGate = true;
            }
        }
        QVERIFY(sawGate);
    }

    void aRestoredPatchIsLoadedOnlyWhenApplyRuns()
    {
        QTemporaryDir patches;
        QVERIFY(patches.isValid());
        const QString patchPath = patches.filePath(u"restored.patch"_s);
        QFile patchFile(patchPath);
        QVERIFY(patchFile.open(QIODevice::WriteOnly | QIODevice::Text));
        patchFile.write("RESTORED-PATCH-CONTENT\n");
        patchFile.close();

        QJsonObject record;
        record.insert(u"taskId"_s, u"restored"_s);
        record.insert(u"status"_s, u"ok"_s);
        record.insert(u"patchPath"_s, patchPath);

        SessionStore::SessionData data;
        data.subagents.append(record);

        AgentLoop agent;
        Settings settings;
        settings.checkpointsEnabled = false;
        agent.setSettings(settings);
        agent.restoreSession(data);
        QCOMPARE(agent.subagent(u"restored"_s).taskId, u"restored"_s);
        QCOMPARE(agent.subagent(u"restored"_s).patchPath, patchPath);
        QVERIFY2(agent.subagent(u"restored"_s).patch.isEmpty(),
                 "the patch was loaded during restore instead of on demand");

        // Apply reads the diff into a local buffer and hands it to the
        // worktree apply; the record never holds it again, so a failed apply
        // cannot pin the loaded patch in memory.
        QString error;
        QVERIFY(!agent.applySubagentWorktree(u"restored"_s, &error));
        QVERIFY2(!error.isEmpty(), "apply did not report a failure");
        QVERIFY2(agent.subagent(u"restored"_s).patch.isEmpty(),
                 "apply pinned the loaded patch in the record");

        // The diff reads on demand: once the file is gone, the failure moves
        // to the load step, which restore itself never performed.
        QVERIFY(QFile::remove(patchPath));
        error.clear();
        QVERIFY(!agent.applySubagentWorktree(u"restored"_s, &error));
        QVERIFY2(error.contains(u"saved sub-agent patch"_s), qPrintable(error));
    }

    void transcriptPagingReadsPastTheOldOneMiBPrefix()
    {
        AgentLoop agent;
        agent.setConversationId(u"conv-paging"_s);
        const QString path = m_workspace->path()
            + u"/subagents/conv-paging/large.jsonl"_s;
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QByteArray prefix(1200 * 1024, 'a');
        const QByteArray tail("TAIL-MARKER");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(prefix), static_cast<qint64>(prefix.size()));
        QCOMPARE(file.write(tail), static_cast<qint64>(tail.size()));
        file.close();

        const QString page = agent.subagentTranscript(u"large"_s, prefix.size(), 1000);
        QVERIFY2(page.contains(u"TAIL-MARKER"_s), qPrintable(page.left(200)));
        QVERIFY(page.contains(u"from character"_s));

        const QString first = agent.subagentTranscript(u"large"_s, 0, 16);
        QVERIFY(first.contains(u"more follows"_s));
        QVERIFY(first.contains(u"aaaaaaaaaaaaaaaa"_s));
    }

private:
    std::unique_ptr<QTemporaryDir> m_workspace;
    QProcess *m_server = nullptr;
    QString m_portFile;
    int m_port = 0;
};

QTEST_MAIN(TestSubtaskParallel)
#include "test_subtaskparallel.moc"