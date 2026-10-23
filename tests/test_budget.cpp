/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Drives a real AgentLoop against an in-process mock LLM to pin the behaviour
 * of the tool-call budget: when the budget is exhausted, the turn must still
 * ask the model for the closing summary and finish. It used to stall instead:
 * the budget path set WaitingForNextModel and then called processQueue(),
 * whose empty-queue exit returns early on exactly that state, so the summary
 * request was never sent and the turn hung busy.
 */

#include "agentloop.h"
#include "types.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxyFactory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

// Answers the first request with two read_file tool calls in a single
// response and every later request with plain text, and counts the requests
// so the test can prove the summarising request actually left the process.
class MockLlm : public QObject
{
    Q_OBJECT
public:
    int requests = 0;

    explicit MockLlm(QObject *parent = nullptr)
        : QObject(parent)
        , m_server(new QTcpServer(this))
    {
        connect(m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = m_server->nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                while (socket->canReadLine()) {
                    const QString line = QString::fromUtf8(socket->readLine()).trimmed();
                    if (!line.isEmpty()) {
                        continue;
                    }
                    handle(socket);
                    return;
                }
            });
        });
    }

    bool listen()
    {
        return m_server->listen(QHostAddress::LocalHost);
    }

    int port() const
    {
        return m_server->serverPort();
    }

private:
    void handle(QTcpSocket *socket)
    {
        ++requests;
        QByteArray body;
        if (requests == 1) {
            QJsonArray calls;
            for (int i = 0; i < 2; ++i) {
                QJsonObject function;
                function.insert(u"name"_s, u"read_file"_s);
                function.insert(u"arguments"_s, QString::fromUtf8(
                    QJsonDocument(QJsonObject{{u"path"_s, u"README.md"_s}}).toJson(QJsonDocument::Compact)));
                QJsonObject call;
                call.insert(u"id"_s, QStringLiteral("call_%1").arg(i));
                call.insert(u"type"_s, u"function"_s);
                call.insert(u"function"_s, function);
                calls.append(call);
            }
            body += QByteArray("data: ") + QJsonDocument(QJsonObject{
                {u"choices"_s, QJsonArray{QJsonObject{
                    {u"index"_s, 0},
                    {u"delta"_s, QJsonObject{{u"tool_calls"_s, calls}}},
                    {u"finish_reason"_s, u"tool_calls"_s},
                }}},
            }).toJson(QJsonDocument::Compact);
            body += "\n\n";
            body += "data: [DONE]\n\n";
        } else {
            body += QByteArray("data: ") + QJsonDocument(QJsonObject{
                {u"choices"_s, QJsonArray{QJsonObject{
                    {u"index"_s, 0},
                    {u"delta"_s, QJsonObject{{u"content"_s, u"Summarised the work."_s}}},
                    {u"finish_reason"_s, u"stop"_s},
                }}},
            }).toJson(QJsonDocument::Compact);
            body += "\n\n";
            body += "data: [DONE]\n\n";
        }

        const QByteArray head = "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/event-stream\r\n"
                                "Connection: close\r\n"
                                "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
        socket->write(head);
        socket->write(body);
        socket->flush();
        socket->disconnectFromHost();
    }

    QTcpServer *m_server = nullptr;
};

} // namespace

class TestBudget : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // The mock server is on localhost; an exported proxy would route the
        // request away from it and every request would fail.
        QNetworkProxyFactory::setUseSystemConfiguration(false);
    }

    void budgetExhaustionStillEndsTheTurn()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QFile file(workspace.path() + u"/README.md"_s);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("workspace\n");
        file.close();

        MockLlm mock;
        QVERIFY(mock.listen());
        QVERIFY(mock.port() > 0);

        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(mock.port());
        settings.openaiCompatibleApiKey = u"test"_s;
        settings.openaiCompatibleModel = u"mock"_s;
        settings.agentMode = u"code"_s;
        settings.maxToolCalls = 1; // the second read_file hits the budget
        settings.maxModelRequests = 10;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;

        agent.setSettings(settings);
        agent.setWorkspace(workspace.path());

        bool finished = false;
        QString failure;
        connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] { finished = true; });
        connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) { failure = error; });

        agent.start(u"Read the readme twice."_s);

        QElapsedTimer timer;
        timer.start();
        while (!finished && timer.elapsed() < 10000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QTest::qWait(10);
        }

        QVERIFY2(finished, "the turn never finished after the tool budget was exhausted (stall)");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // The first request asked for two reads; the budget stops the second
        // one, and the turn must still make the closing summary request.
        QCOMPARE(mock.requests, 2);
    }
};

QTEST_MAIN(TestBudget)
#include "test_budget.moc"
