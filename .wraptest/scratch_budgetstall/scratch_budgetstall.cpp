/*
 * Scratch reproduction: tool-call budget exhausted on a synchronous tool with
 * nothing async in flight. Hypothesis: executeOne()'s budget path sets
 * WaitingForNextModel and calls processQueue(), which early-returns, so the
 * "summarise" model request is never made and the turn hangs busy.
 */

#include "agentloop.h"
#include "types.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxyFactory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class MockServer : public QObject
{
    Q_OBJECT
public:
    QTcpServer *server = nullptr;
    int port = 0;
    int requests = 0;

    explicit MockServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        server = new QTcpServer(this);
        server->listen(QHostAddress::LocalHost);
        port = server->serverPort();
        connect(server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = server->nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                if (!socket->canReadLine()) {
                    return;
                }
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

    void handle(QTcpSocket *socket)
    {
        ++requests;
        QByteArray body;
        if (requests == 1) {
            // Two read_file tool calls in a single response.
            const QJsonArray calls = [] {
                QJsonArray out;
                for (int i = 0; i < 2; ++i) {
                    QJsonObject fn;
                    fn.insert("name", "read_file");
                    fn.insert("arguments", QString::fromUtf8(
                        QJsonDocument(QJsonObject{{"path", "README.md"}}).toJson(QJsonDocument::Compact)));
                    QJsonObject call;
                    call.insert("id", QStringLiteral("call_%1").arg(i));
                    call.insert("type", "function");
                    call.insert("function", fn);
                    out.append(call);
                }
                return out;
            }();
            body += QByteArray("data: ") + QJsonDocument(QJsonObject{
                {"choices", QJsonArray{QJsonObject{
                    {"index", 0},
                    {"delta", QJsonObject{{"tool_calls", calls}}},
                    {"finish_reason", "tool_calls"},
                }}},
            }).toJson(QJsonDocument::Compact).toStdString().c_str();
            body += "\n\n";
            body += "data: [DONE]\n\n";
        } else {
            body += QByteArray("data: ") + QJsonDocument(QJsonObject{
                {"choices", QJsonArray{QJsonObject{
                    {"index", 0},
                    {"delta", QJsonObject{{"content", "Done summarising."}}},
                    {"finish_reason", "stop"},
                }}},
            }).toJson(QJsonDocument::Compact).toStdString().c_str();
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
};

class TestBudgetStall : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void budgetExhaustionStillEndsTheTurn()
    {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QFile file(workspace.path() + "/README.md");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("workspace\n");
        file.close();

        MockServer mock;
        QVERIFY(mock.port > 0);

        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(mock.port);
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
        while (!finished && timer.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QTest::qWait(10);
        }

        qInfo() << "requests served:" << mock.requests << "turnFinished:" << finished
                << "failure:" << failure;
        QVERIFY2(finished, "the turn never finished after the tool budget was exhausted (stall)");
        QVERIFY(failure.isEmpty());
        QCOMPARE(mock.requests, 2); // the summarise request must actually have been made
    }
};

QTEST_MAIN(TestBudgetStall)
#include "scratch_budgetstall.moc"
