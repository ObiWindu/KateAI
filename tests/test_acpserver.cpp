/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/acpserver.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTest>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

namespace
{

class FakeSession : public AcpServerSession
{
public:
    explicit FakeSession(QObject *parent = nullptr)
        : AcpServerSession(parent)
    {
    }

    void startPrompt(const QString &text) override
    {
        lastPrompt = text;
        if (askPermission) {
            PermissionRequest request;
            request.toolName = u"write_file"_s;
            request.toolCallId = u"call-1"_s;
            request.summary = u"Write note.txt"_s;
            request.risk = ToolRisk::Write;
            pendingPermission = true;
            Q_EMIT permissionNeeded(request);
            return;
        }
        Q_EMIT textDelta(u"working"_s);
        Q_EMIT finished(true, u"end_turn"_s, QString());
    }

    void cancel() override
    {
        cancelled = true;
    }

    void resolvePermission(PermissionDecision decision, const QString &toolCallId) override
    {
        lastDecision = decision;
        lastToolCall = toolCallId;
        if (pendingPermission) {
            pendingPermission = false;
            Q_EMIT textDelta(u"approved"_s);
            Q_EMIT finished(true, u"end_turn"_s, QString());
        }
    }

    void answerQuestion(const QString &answer) override
    {
        lastAnswer = answer;
    }

    QString lastPrompt;
    QString lastToolCall;
    QString lastAnswer;
    PermissionDecision lastDecision = PermissionDecision::Deny;
    bool askPermission = false;
    bool pendingPermission = false;
    bool cancelled = false;
};

QList<QJsonObject> messages(const QByteArray &data)
{
    QList<QJsonObject> result;
    for (const QByteArray &line : data.split('\n')) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject()) {
            result.append(document.object());
        }
    }
    return result;
}

} // namespace

class TestAcpServer : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initializeAndPrompt()
    {
        FakeSession *fake = nullptr;
        AcpServer server([&fake](const QString &, const QString &, QString *) -> AcpServerSession * {
            fake = new FakeSession;
            return fake;
        });
        QByteArray output;
        QBuffer buffer(&output);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        server.setOutputDevice(&buffer);

        QString error;
        QVERIFY(server.handleLine(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1}})",
            &error));
        QList<QJsonObject> replies = messages(output);
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.first().value(u"result"_s).toObject()
                     .value(u"protocolVersion"_s).toInt(), 1);
        QVERIFY(replies.first().value(u"id"_s).toInt() == 1);
        output.clear();
        buffer.seek(0);

        QVERIFY(server.handleLine(
            R"({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":".","mcpServers":[]}})",
            &error));
        replies = messages(output);
        QCOMPARE(replies.size(), 1);
        const QString sessionId = replies.first().value(u"result"_s).toObject()
                                      .value(u"sessionId"_s).toString();
        QVERIFY(!sessionId.isEmpty());
        output.clear();
        buffer.seek(0);

        const QByteArray prompt =
            QJsonDocument(QJsonObject{
                              {u"jsonrpc"_s, u"2.0"_s},
                              {u"id"_s, 3},
                              {u"method"_s, u"session/prompt"_s},
                              {u"params"_s,
                               QJsonObject{
                                   {u"sessionId"_s, sessionId},
                                   {u"prompt"_s,
                                    QJsonArray{QJsonObject{{u"type"_s, u"text"_s},
                                                           {u"text"_s, u"hello"_s}}}}}},
                          })
                .toJson(QJsonDocument::Compact);
        QVERIFY(server.handleLine(prompt, &error));
        replies = messages(output);
        bool sawUpdate = false;
        bool sawResult = false;
        for (const QJsonObject &message : replies) {
            if (message.value(u"method"_s).toString() == u"session/update"_s) {
                const QJsonObject update = message.value(u"params"_s).toObject()
                                               .value(u"update"_s).toObject();
                if (update.value(u"sessionUpdate"_s).toString() == u"agent_message_chunk"_s) {
                    sawUpdate = true;
                }
            }
            if (message.value(u"id"_s).toInt() == 3) {
                sawResult = message.value(u"result"_s).toObject()
                                .value(u"stopReason"_s).toString()
                            == u"end_turn"_s;
            }
        }
        QVERIFY(sawUpdate);
        QVERIFY(sawResult);
        QVERIFY(fake);
        QCOMPARE(fake->lastPrompt, u"hello"_s);
    }

    void permissionRoundTrip()
    {
        FakeSession *fake = nullptr;
        AcpServer server([&fake](const QString &, const QString &, QString *) -> AcpServerSession * {
            fake = new FakeSession;
            fake->askPermission = true;
            return fake;
        });
        QByteArray output;
        QBuffer buffer(&output);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        server.setOutputDevice(&buffer);
        QString error;

        QVERIFY(server.handleLine(
            R"({"jsonrpc":"2.0","id":1,"method":"session/new","params":{"cwd":"."}})", &error));
        const QString sessionId = messages(output).first().value(u"result"_s).toObject()
                                      .value(u"sessionId"_s).toString();
        output.clear();
        buffer.seek(0);

        const QByteArray prompt =
            QJsonDocument(QJsonObject{
                              {u"jsonrpc"_s, u"2.0"_s},
                              {u"id"_s, 2},
                              {u"method"_s, u"session/prompt"_s},
                              {u"params"_s,
                               QJsonObject{
                                   {u"sessionId"_s, sessionId},
                                   {u"prompt"_s,
                                    QJsonArray{QJsonObject{{u"type"_s, u"text"_s},
                                                           {u"text"_s, u"write"_s}}}}}},
                          })
                .toJson(QJsonDocument::Compact);
        QVERIFY(server.handleLine(prompt, &error));
        QList<QJsonObject> replies = messages(output);
        QCOMPARE(replies.size(), 1);
        const QJsonObject request = replies.first();
        QCOMPARE(request.value(u"method"_s).toString(), u"session/request_permission"_s);
        const qint64 requestId = request.value(u"id"_s).toVariant().toLongLong();
        QVERIFY(requestId > 0);
        output.clear();
        buffer.seek(0);

        const QByteArray response =
            QJsonDocument(QJsonObject{
                              {u"jsonrpc"_s, u"2.0"_s},
                              {u"id"_s, requestId},
                              {u"result"_s,
                               QJsonObject{
                                   {u"outcome"_s,
                                    QJsonObject{{u"outcome"_s, u"selected"_s},
                                                {u"optionId"_s, u"allow_session"_s}}}}},
                          })
                .toJson(QJsonDocument::Compact);
        QVERIFY(server.handleLine(response, &error));
        QVERIFY(fake);
        QCOMPARE(fake->lastDecision, PermissionDecision::AllowSession);
        QCOMPARE(fake->lastToolCall, u"call-1"_s);

        replies = messages(output);
        bool sawResult = false;
        for (const QJsonObject &message : replies) {
            if (message.value(u"id"_s).toInt() == 2) {
                sawResult = true;
            }
        }
        QVERIFY(sawResult);
    }

    void unknownMethodReturnsError()
    {
        AcpServer server([](const QString &, const QString &, QString *) -> AcpServerSession * {
            return nullptr;
        });
        QByteArray output;
        QBuffer buffer(&output);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        server.setOutputDevice(&buffer);
        QString error;
        QVERIFY(server.handleLine(
            R"({"jsonrpc":"2.0","id":9,"method":"does/not/exist","params":{}})", &error));
        const QList<QJsonObject> replies = messages(output);
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.first().value(u"error"_s).toObject().value(u"code"_s).toInt(), -32601);
    }

    void cancelNotificationReachesSession()
    {
        FakeSession *fake = nullptr;
        AcpServer server([&fake](const QString &, const QString &, QString *) -> AcpServerSession * {
            fake = new FakeSession;
            return fake;
        });
        QByteArray output;
        QBuffer buffer(&output);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        server.setOutputDevice(&buffer);
        QString error;
        QVERIFY(server.handleLine(
            R"({"jsonrpc":"2.0","id":1,"method":"session/new","params":{"cwd":"."}})", &error));
        const QString sessionId = messages(output).first().value(u"result"_s).toObject()
                                      .value(u"sessionId"_s).toString();
        output.clear();
        buffer.seek(0);
        const QByteArray cancel =
            QJsonDocument(QJsonObject{
                              {u"jsonrpc"_s, u"2.0"_s},
                              {u"method"_s, u"session/cancel"_s},
                              {u"params"_s, QJsonObject{{u"sessionId"_s, sessionId}}},
                          })
                .toJson(QJsonDocument::Compact);
        QVERIFY(server.handleLine(cancel, &error));
        QVERIFY(fake->cancelled);
    }
};

QTEST_GUILESS_MAIN(TestAcpServer)

#include "test_acpserver.moc"
