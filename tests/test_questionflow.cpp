/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Drives a real AgentLoop against the mock LLM endpoint to pin the two
 * properties the `question` tool exists for:
 *
 *  1. The turn stops until the user answers -- no second provider request is
 *     made, and the turn cannot finish, while the question is outstanding.
 *  2. The answer becomes the tool result, and the agent resumes from it.
 */

#include "agentloop.h"
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

class TestQuestionFlow : public QObject
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

    std::unique_ptr<QTemporaryDir> m_workspace;
    QProcess *m_server = nullptr;
    QString m_portFile;
    int m_port = 0;

private Q_SLOTS:
    void initTestCase()
    {
        // The mock server is on localhost; an exported proxy would route the
        // request away from it and every request would fail.
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

    void turnStopsUntilTheUserAnswers()
    {
        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"code"_s;
        settings.maxToolCalls = 20;
        settings.maxModelRequests = 10;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;

        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());
        // The question tool must be advertised in every mode, including the
        // read-only ones: that is exactly when a turn is otherwise stuck.
        QVERIFY(agent.activeToolAccess().allows(u"question"_s));

        bool asked = false;
        PermissionRequest question;
        bool finished = false;
        QString failure;
        QObject::connect(&agent, &AgentLoop::questionAsked, &agent, [&asked, &question](const PermissionRequest &request) {
            asked = true;
            question = request;
        });
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] {
            finished = true;
        });
        QObject::connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) {
            failure = error;
        });

        agent.start(u"Ask the user something."_s);
        QVERIFY2(waitFor([&asked] {
            return asked;
        }), "the mock never asked its question");

        QCOMPARE(question.questionText, u"Rewrite the file, or patch it in place?"_s);
        QCOMPARE(question.questionOptions, QStringList({u"Rewrite"_s, u"Patch"_s}));
        QVERIFY(question.allowFreeform);

        // The turn must not finish on its own: an agent that carries on without
        // an answer has just guessed.
        QTest::qWait(1500);
        QVERIFY2(!finished, qPrintable(QStringLiteral("the turn ended without an answer (%1)").arg(failure)));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        agent.answerQuestion(u"Patch"_s);
        QVERIFY2(waitFor([&finished] {
            return finished;
        }), "the agent never resumed from the answer");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        // The user's answer has to land in the history as the tool result, or
        // the next model request cannot know what was decided.
        bool sawAnswer = false;
        for (const ChatMessage &message : agent.messages()) {
            if (message.role == ChatMessage::Role::Tool && message.name == u"question"_s) {
                sawAnswer = message.content.contains(u"Patch"_s);
                break;
            }
        }
        QVERIFY2(sawAnswer, "the answer never reached the conversation as the tool result");
    }

    void anEmptyQuestionIsRefused()
    {
        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"code"_s;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;

        agent.setSettings(settings);
        agent.setWorkspace(m_workspace->path());

        bool asked = false;
        bool finished = false;
        QString failure;
        QObject::connect(&agent, &AgentLoop::questionAsked, &agent, [&asked] {
            asked = true;
        });
        QObject::connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] {
            finished = true;
        });
        QObject::connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) {
            failure = error;
        });

        agent.start(u"Ask nothing at all."_s);
        QVERIFY2(waitFor([&finished] {
            return finished;
        }), "the turn never finished after an empty question");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // A question with no text must not park the turn: there is nothing for
        // the user to answer, so the agent is told so instead.
        QVERIFY2(!asked, qPrintable(QStringLiteral("the loop asked a question with no text in it")));
        QVERIFY2(!agent.isBusy(), "the turn is still parked on a question nobody was asked");
    }
};

QTEST_MAIN(TestQuestionFlow)
#include "test_questionflow.moc"
