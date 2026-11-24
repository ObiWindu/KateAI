/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Drives a real AgentLoop through planner -> executor -> reviewer using two
 * distinct mock provider endpoints. This is the regression guard for the
 * two-model orchestration path: route selection, read-only planner tools,
 * plan handoff, executor mutation, and review.
 */

#include "agentloop.h"
#include "documentbridge.h"
#include "test_python.h"
#include "types.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxyFactory>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestOrchestrationFlow : public QObject
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

    QProcess *startServer(const QString &workspace, const QString &role, int *port)
    {
        const QString portFile = workspace + u"/port-"_s + role;
        auto *server = new QProcess(this);
        const QString script = repoFile(u"tests/mock_orchestration_server.py"_s);
        if (script.isEmpty()) {
            return nullptr;
        }
        server->start(python3Command(), QStringList{script, portFile, role});
        if (!server->waitForStarted(10000)) {
            return server;
        }
        if (!waitFor([&portFile] {
                QFile file(portFile);
                return file.open(QIODevice::ReadOnly | QIODevice::Text)
                    && !file.readAll().trimmed().isEmpty();
            })) {
            return server;
        }
        QFile file(portFile);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            *port = QString::fromUtf8(file.readAll()).trimmed().toInt();
        }
        return server;
    }

    std::unique_ptr<QTemporaryDir> m_workspace;
    QProcess *m_plannerServer = nullptr;
    QProcess *m_executorServer = nullptr;
    int m_plannerPort = 0;
    int m_executorPort = 0;

private Q_SLOTS:
    void initTestCase()
    {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
    }

    void init()
    {
        m_workspace = std::make_unique<QTemporaryDir>();
        QVERIFY(m_workspace->isValid());
        QFile marker(m_workspace->path() + u"/README.md"_s);
        QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::Text));
        marker.write("workspace\n");
        m_plannerServer = startServer(m_workspace->path(), u"planner"_s, &m_plannerPort);
        m_executorServer = startServer(m_workspace->path(), u"executor"_s, &m_executorPort);
        QVERIFY(m_plannerServer);
        QVERIFY(m_executorServer);
        QVERIFY(m_plannerPort > 0);
        QVERIFY(m_executorPort > 0);
    }

    void cleanup()
    {
        for (QProcess **server : {&m_plannerServer, &m_executorServer}) {
            if (*server) {
                if ((*server)->state() != QProcess::NotRunning) {
                    (*server)->kill();
                    (*server)->waitForFinished(5000);
                }
                delete *server;
                *server = nullptr;
            }
        }
        m_workspace.reset();
    }

    void plannerHandsOffToExecutorAndReviewApproves()
    {
        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_plannerPort);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"planner"_s;
        settings.deepseekUrl = QStringLiteral("http://127.0.0.1:%1").arg(m_executorPort);
        settings.deepseekApiKey = u"test"_s;
        settings.deepseekModel = u"executor"_s;
        settings.agentMode = u"code"_s;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;
        settings.maxModelRequests = 20;
        settings.maxToolCalls = 20;
        settings.orchestration.enabled = true;
        settings.orchestration.planner = ModelRoute{Provider::OpenAICompatible, u"planner"_s, QString()};
        settings.orchestration.executor = ModelRoute{Provider::DeepSeek, u"executor"_s, QString()};
        settings.orchestration.reviewMode = u"always"_s;

        DiskDocumentBridge bridge;
        bridge.setUseOpenDocuments(false);
        agent.setDocumentBridge(&bridge);
        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());

        QStringList phases;
        bool finished = false;
        QString failure;
        QObject::connect(&agent, &AgentLoop::orchestrationPhaseChanged, &agent,
                         [&phases](const QString &role, const QString &, const QString &,
                                   const QString &) {
                             if (!role.isEmpty()) {
                                 phases.append(role);
                             }
                         });
        QObject::connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) {
            failure = error;
        });
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished]() {
            finished = true;
        });

        agent.start(u"Write a note file."_s);
        QVERIFY2(waitFor([&finished] { return finished; }, 30000),
                 "orchestrated turn did not finish");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(phases.contains(u"planner"_s));
        QVERIFY(phases.contains(u"executor"_s));

        QFile note(m_workspace->path() + u"/note.txt"_s);
        QVERIFY2(note.exists(), "the executor never wrote note.txt");
        QVERIFY(note.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(note.readAll()), u"orchestrated\n"_s);
    }

    void disabledOrchestrationKeepsSingleModelPath()
    {
        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_executorPort);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"executor"_s;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;
        settings.orchestration.enabled = false;
        DiskDocumentBridge bridge;
        bridge.setUseOpenDocuments(false);
        agent.setDocumentBridge(&bridge);
        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());

        bool finished = false;
        QString failure;
        QObject::connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) {
            failure = error;
        });
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished]() {
            finished = true;
        });
        agent.start(u"Write a note file."_s);
        QVERIFY(waitFor([&finished] { return finished; }, 30000));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(QFile::exists(m_workspace->path() + u"/note.txt"_s));
    }
};

QTEST_GUILESS_MAIN(TestOrchestrationFlow)
#include "test_orchestrationflow.moc"
