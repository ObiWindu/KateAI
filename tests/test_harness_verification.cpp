/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Drives a real AgentLoop through the 0.7 verification gate: a mutating turn
 * cannot finish while the configured check fails, the failure is fed back to
 * the model, and the turn completes only after the check passes. The same
 * turn also proves the event log records the sequence.
 */

#include "agentloop.h"
#include "documentbridge.h"
#include "harness/eventstore.h"
#include "sessionstore.h"
#include "test_python.h"
#include "types.h"

#include <QCoreApplication>
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

namespace
{

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
            auto buffer = std::make_shared<QByteArray>();
            auto replied = std::make_shared<bool>(false);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, replied] {
                if (*replied) {
                    return;
                }
                buffer->append(socket->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                int contentLength = 0;
                for (const QByteArray &line : buffer->left(headerEnd).split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        contentLength = line.mid(15).trimmed().toInt();
                        break;
                    }
                }
                if (buffer->size() < headerEnd + 4 + contentLength) {
                    return;
                }
                *replied = true;
                handle(socket);
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
        if (requests == 1) {
            writeFileCall(socket, u"note.txt"_s, u"hello"_s);
        } else if (requests == 3) {
            writeFileCall(socket, u"verify-ok"_s, u"yes"_s);
        } else {
            text(socket, requests == 2 ? u"Done."_s : u"Finished."_s);
        }
    }

    void text(QTcpSocket *socket, const QString &content)
    {
        QByteArray body = "data: ";
        body += QJsonDocument(QJsonObject{
            {u"choices"_s, QJsonArray{QJsonObject{
                {u"index"_s, 0},
                {u"delta"_s, QJsonObject{{u"content"_s, content}}},
                {u"finish_reason"_s, u"stop"_s},
            }}},
        }).toJson(QJsonDocument::Compact);
        body += "\n\ndata: [DONE]\n\n";
        send(socket, body);
    }

    void writeFileCall(QTcpSocket *socket, const QString &path, const QString &content)
    {
        QJsonObject function;
        function.insert(u"name"_s, u"write_file"_s);
        function.insert(u"arguments"_s,
                        QString::fromUtf8(QJsonDocument(QJsonObject{{u"path"_s, path},
                                                                    {u"content"_s, content}})
                                              .toJson(QJsonDocument::Compact)));
        QJsonObject call;
        call.insert(u"id"_s, u"call_write"_s);
        call.insert(u"type"_s, u"function"_s);
        call.insert(u"function"_s, function);

        QByteArray body = "data: ";
        body += QJsonDocument(QJsonObject{
            {u"choices"_s, QJsonArray{QJsonObject{
                {u"index"_s, 0},
                {u"delta"_s, QJsonObject{{u"tool_calls"_s, QJsonArray{call}}}},
                {u"finish_reason"_s, u"tool_calls"_s},
            }}},
        }).toJson(QJsonDocument::Compact);
        body += "\n\ndata: [DONE]\n\n";
        send(socket, body);
    }

    void send(QTcpSocket *socket, const QByteArray &body)
    {
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

bool writeText(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

} // namespace

class TestHarnessVerification : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
    }

    void gateBlocksUntilTheCheckPasses()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qputenv("KATEAI_SESSION_DB", dir.filePath(u"sessions.sqlite"_s).toUtf8());
        qputenv("KATEAI_HARNESS_FLAGS", "+verificationGate,+eventLog");

        QJsonObject config;
        config.insert(u"verify"_s, QJsonArray{
            QStringLiteral("%1 -c \"import os,sys; sys.exit(0 if os.path.exists('verify-ok') else 1)\"")
                .arg(python3Command())});
        QVERIFY(writeText(dir.filePath(u".kateai/config.json"_s),
                          QJsonDocument(config).toJson()));
        QVERIFY(writeText(dir.filePath(u"README.md"_s), "workspace\n"));

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
        settings.maxModelRequests = 10;
        settings.checkpointsEnabled = false;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        settings.sandbox = SandboxProfile::Off;
        settings.structuredThinking = false;
        settings.structuredPlanning = false;
        settings.enableAutoRetry = false;
        settings.requireVerification = true;
        settings.maxVerificationAttempts = 2;

        const QString conversation = SessionStore::createNewConversation();
        QVERIFY(!conversation.isEmpty());
        agent.setConversationId(conversation);
        agent.setSettings(settings);
        DiskDocumentBridge bridge;
        agent.setDocumentBridge(&bridge);
        agent.setWorkspace(dir.path());

        QStringList notices;
        bool finished = false;
        QString failure;
        connect(&agent, &AgentLoop::turnFinished, &agent, [&finished] { finished = true; });
        connect(&agent, &AgentLoop::failed, &agent, [&failure](const QString &error) { failure = error; });
        connect(&agent, &AgentLoop::noticeRaised, &agent,
                [&notices](const QString &text) { notices.append(text); });

        agent.start(u"Create the note and the verification marker."_s);

        QElapsedTimer timer;
        timer.start();
        while (!finished && timer.elapsed() < 20000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QTest::qWait(10);
        }

        QVERIFY2(finished, "the verification gate never let the turn finish");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        if (!QFile::exists(dir.filePath(u"note.txt"_s))) {
            QString dump;
            for (const ChatMessage &message : agent.messages()) {
                dump += u"--- %1\n%2\n"_s.arg(static_cast<int>(message.role)).arg(message.content);
            }
            qWarning().noquote() << "requests" << mock.requests << "notices" << notices.join(u" | "_s);
            qWarning().noquote() << dump;
        }
        QVERIFY(QFile::exists(dir.filePath(u"note.txt"_s)));
        QVERIFY(QFile::exists(dir.filePath(u"verify-ok"_s)));
        // write note, "done", write marker, "done", evidence summary.
        QCOMPARE(mock.requests, 5);

        QString transcript;
        for (const ChatMessage &message : agent.messages()) {
            transcript += message.content + u'\n';
        }
        QVERIFY(transcript.contains(u"Verification failed"_s));
        QVERIFY(transcript.contains(u"configured checks passed"_s));

        const QString tracePath = dir.filePath(u"trace.jsonl"_s);
        QString traceError;
        QVERIFY2(agent.exportTrace(tracePath, &traceError), qPrintable(traceError));
        QFile trace(tracePath);
        QVERIFY(trace.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString traceText = QString::fromUtf8(trace.readAll());
        QVERIFY(traceText.contains(u"turn/start"_s));
        QVERIFY(traceText.contains(u"tool/call"_s));
        QVERIFY(traceText.contains(u"tool/result"_s));
        QVERIFY(traceText.contains(u"verification"_s));
        QVERIFY(traceText.contains(u"turn/end"_s));

        qunsetenv("KATEAI_HARNESS_FLAGS");
        qunsetenv("KATEAI_SESSION_DB");
    }
};

QTEST_MAIN(TestHarnessVerification)
#include "test_harness_verification.moc"
