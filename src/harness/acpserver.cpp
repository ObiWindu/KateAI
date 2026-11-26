/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpserver.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <cstdio>
#include <iostream>
#include <string>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kServerError = -32000;

// A session with no prompt in flight and no traffic for this long is closed.
// Long enough that a client can think between turns, short enough that an
// abandoned window does not hold a harness loop for the whole process.
constexpr qint64 kIdleSessionMs = 15 * 60 * 1000;
constexpr int kIdleSweepMs = 60 * 1000;
constexpr int kAcpProtocolVersion = 1;

QString textFromBlocks(const QJsonArray &blocks)
{
    QStringList parts;
    for (const QJsonValue &value : blocks) {
        const QJsonObject block = value.toObject();
        const QString type = block.value(u"type"_s).toString();
        if (type.isEmpty() || type == u"text"_s) {
            const QString text = block.value(u"text"_s).toString();
            if (!text.isEmpty()) {
                parts.append(text);
            }
        } else if (type == u"resource_link"_s) {
            const QString uri = block.value(u"uri"_s).toString();
            if (!uri.isEmpty()) {
                parts.append(uri);
            }
        }
    }
    return parts.join(u"\n"_s);
}

class StdinReader : public QObject
{
public:
    explicit StdinReader(AcpServer *server)
        : m_server(server)
    {
    }

    void readLoop()
    {
        std::string line;
        while (std::getline(std::cin, line)) {
            const QByteArray data = QByteArray::fromStdString(line);
            if (data.trimmed().isEmpty()) {
                continue;
            }
            QPointer<AcpServer> server = m_server;
            QMetaObject::invokeMethod(
                m_server,
                [server, data] {
                    if (server) {
                        server->handleLine(data);
                    }
                },
                Qt::QueuedConnection);
        }
        QPointer<AcpServer> server = m_server;
        QMetaObject::invokeMethod(
            m_server,
            [server] {
                if (server) {
                    Q_EMIT server->stdioClosed();
                }
            },
            Qt::QueuedConnection);
    }

private:
    AcpServer *m_server = nullptr;
};

} // namespace

AcpServer::AcpServer(SessionFactory factory, QObject *parent)
    : QObject(parent)
    , m_factory(std::move(factory))
{
    // A client that opens a session and walks away used to keep a full harness
    // loop, project graph and SQLite connection alive for the life of the
    // process. Sessions are cheap to recreate and ACP clients may open a new
    // one per turn, so idle sessions are reaped.
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(kIdleSweepMs);
    connect(m_idleTimer, &QTimer::timeout, this, &AcpServer::reapIdleSessions);
    m_idleTimer->start();
}

AcpServer::~AcpServer()
{
    stop();
    qDeleteAll(m_sessions);
    m_sessions.clear();
    m_sessionActivity.clear();
}

void AcpServer::startStdio()
{
    if (m_readerThread) {
        return;
    }
    m_readerThread = new QThread(this);
    m_reader = new StdinReader(this);
    m_reader->moveToThread(m_readerThread);
    connect(m_readerThread, &QThread::started, m_reader, [this] {
        static_cast<StdinReader *>(m_reader)->readLoop();
        if (m_readerThread) {
            m_readerThread->quit();
        }
    });
    connect(m_readerThread, &QThread::finished, m_reader, &QObject::deleteLater);
    m_readerThread->start();
}

void AcpServer::stop()
{
    if (m_readerThread) {
        m_readerThread->quit();
        m_readerThread->wait(2000);
        m_readerThread->deleteLater();
        m_readerThread = nullptr;
        m_reader = nullptr;
    }
}

void AcpServer::setOutputDevice(QIODevice *output)
{
    m_output = output;
}

void AcpServer::sendResult(const QJsonValue &id, const QJsonObject &result)
{
    QJsonObject message;
    message.insert(u"jsonrpc"_s, u"2.0"_s);
    message.insert(u"id"_s, id);
    message.insert(u"result"_s, result);
    writeMessage(message);
}

void AcpServer::sendError(const QJsonValue &id, int code, const QString &message)
{
    QJsonObject error;
    error.insert(u"code"_s, code);
    error.insert(u"message"_s, message);
    QJsonObject response;
    response.insert(u"jsonrpc"_s, u"2.0"_s);
    response.insert(u"id"_s, id);
    response.insert(u"error"_s, error);
    writeMessage(response);
}

void AcpServer::sendNotification(const QString &method, const QJsonObject &params)
{
    QJsonObject message;
    message.insert(u"jsonrpc"_s, u"2.0"_s);
    message.insert(u"method"_s, method);
    message.insert(u"params"_s, params);
    writeMessage(message);
}

void AcpServer::sendSessionUpdate(const QString &sessionId, const QJsonObject &update)
{
    sendNotification(u"session/update"_s,
                     QJsonObject{{u"sessionId"_s, sessionId}, {u"update"_s, update}});
}

void AcpServer::writeMessage(const QJsonObject &message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (m_output) {
        m_output->write(line);
        m_output->waitForBytesWritten(0);
        return;
    }
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

bool AcpServer::handleLine(const QByteArray &line, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        sendError(QJsonValue::Null, kParseError, u"Invalid JSON: %1"_s.arg(parseError.errorString()));
        if (error) {
            *error = parseError.errorString();
        }
        return false;
    }
    const QJsonObject message = document.object();
    if (!message.contains(u"method"_s)) {
        if (message.contains(u"id"_s)) {
            handleClientResponse(message);
            return true;
        }
        sendError(QJsonValue::Null, kInvalidRequest, u"A message needs a method or a response id."_s);
        return false;
    }
    const QJsonValue id = message.value(u"id"_s);
    if (id.isUndefined()) {
        handleNotification(message);
        return true;
    }
    const QJsonObject response = dispatch(message);
    if (!response.isEmpty()) {
        writeMessage(response);
    }
    return true;
}

QJsonObject AcpServer::dispatch(const QJsonObject &message)
{
    const QString method = message.value(u"method"_s).toString();
    const QJsonValue id = message.value(u"id"_s);
    const QJsonObject params = message.value(u"params"_s).toObject();

    auto ok = [&id](QJsonObject result) {
        return QJsonObject{{u"jsonrpc"_s, u"2.0"_s},
                           {u"id"_s, id},
                           {u"result"_s, result}};
    };
    auto fail = [&id](int code, const QString &text) {
        return QJsonObject{{u"jsonrpc"_s, u"2.0"_s},
                           {u"id"_s, id},
                           {u"error"_s, QJsonObject{{u"code"_s, code}, {u"message"_s, text}}}};
    };

    if (method == u"initialize"_s) {
        m_initialized = true;
        QJsonObject capabilities;
        capabilities.insert(u"loadSession"_s, false);
        capabilities.insert(u"promptCapabilities"_s,
                            QJsonObject{{u"image"_s, false},
                                        {u"audio"_s, false},
                                        {u"embeddedContext"_s, true}});
        capabilities.insert(u"sessionCapabilities"_s,
                            QJsonObject{{u"close"_s, QJsonObject()}});
        QJsonObject info;
        info.insert(u"name"_s, u"kateai"_s);
        info.insert(u"title"_s, u"Kate AI"_s);
#ifdef KATEAI_VERSION
        info.insert(u"version"_s, QStringLiteral(KATEAI_VERSION));
#else
        info.insert(u"version"_s, u"1.0.0"_s);
#endif
        return ok(QJsonObject{{u"protocolVersion"_s, kAcpProtocolVersion},
                              {u"agentCapabilities"_s, capabilities},
                              {u"agentInfo"_s, info},
                              {u"authMethods"_s, QJsonArray()}});
    }

    if (method == u"authenticate"_s) {
        return ok(QJsonObject());
    }

    if (method == u"session/new"_s) {
        // Every session owns a full harness loop, graph and SQLite connection.
        // A client that opens sessions without closing them must not be able to
        // grow the process without bound.
        constexpr int kMaxSessions = 64;
        if (m_sessions.size() >= kMaxSessions) {
            return fail(kServerError,
                        u"Too many open sessions; close one before creating another."_s);
        }
        const QString cwd = params.value(u"cwd"_s).toString();
        const QString workdir = cwd.isEmpty() ? QDir::currentPath() : cwd;
        if (!QFileInfo(workdir).isDir()) {
            return fail(kInvalidParams, u"cwd does not exist: %1"_s.arg(workdir));
        }
        if (!m_factory) {
            return fail(kServerError, u"No harness session factory is installed."_s);
        }
        const QString sessionId =
            u"sess-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QString sessionError;
        AcpServerSession *session = m_factory(sessionId, workdir, &sessionError);
        if (!session) {
            return fail(kServerError, sessionError.isEmpty()
                                          ? u"Cannot create a harness session."_s
                                          : sessionError);
        }
        m_sessions.insert(sessionId, session);
        touchSession(sessionId);
        connectSession(sessionId, session);
        return ok(QJsonObject{{u"sessionId"_s, sessionId}});
    }

    if (method == u"session/prompt"_s) {
        const QString sessionId = params.value(u"sessionId"_s).toString();
        AcpServerSession *session = m_sessions.value(sessionId, nullptr);
        if (!session) {
            return fail(kInvalidParams, u"Unknown session: %1"_s.arg(sessionId));
        }
        if (m_pendingPrompts.contains(sessionId)) {
            return fail(kServerError, u"This session already has a turn in flight."_s);
        }
        const QString text = textFromBlocks(params.value(u"prompt"_s).toArray());
        if (text.trimmed().isEmpty()) {
            return fail(kInvalidParams, u"The prompt has no text content."_s);
        }
        m_pendingPrompts.insert(sessionId, id);
        touchSession(sessionId);
        session->startPrompt(text);
        return QJsonObject();
    }

    if (method == u"session/close"_s) {
        const QString sessionId = params.value(u"sessionId"_s).toString();
        AcpServerSession *session = m_sessions.take(sessionId);
        if (!session) {
            return fail(kInvalidParams, u"Unknown session: %1"_s.arg(sessionId));
        }
        m_pendingPrompts.remove(sessionId);
        m_sessionActivity.remove(sessionId);
        session->deleteLater();
        return ok(QJsonObject());
    }

    if (method == u"session/cancel"_s) {
        // Spec form is a notification, but accept a request id too.
        const QString sessionId = params.value(u"sessionId"_s).toString();
        if (AcpServerSession *session = m_sessions.value(sessionId, nullptr)) {
            session->cancel();
        }
        return ok(QJsonObject());
    }

    if (method == u"session/load"_s) {
        return fail(kMethodNotFound, u"Session loading is not supported by this server."_s);
    }

    if (method == u"_kateai/ping"_s) {
        return ok(QJsonObject{{u"ok"_s, true}});
    }
    if (method == u"_kateai/capabilities"_s) {
        QJsonObject capabilities;
        capabilities.insert(u"eventLog"_s, true);
        capabilities.insert(u"verification"_s, true);
        capabilities.insert(u"policy"_s, true);
        capabilities.insert(u"hooks"_s, true);
        capabilities.insert(u"permissions"_s, true);
        capabilities.insert(u"questions"_s, true);
        capabilities.insert(u"sessions"_s, true);
        return ok(capabilities);
    }

    return fail(kMethodNotFound, u"Unknown method: %1"_s.arg(method));
}

void AcpServer::handleNotification(const QJsonObject &message)
{
    const QString method = message.value(u"method"_s).toString();
    const QJsonObject params = message.value(u"params"_s).toObject();
    if (method == u"session/cancel"_s) {
        const QString sessionId = params.value(u"sessionId"_s).toString();
        if (AcpServerSession *session = m_sessions.value(sessionId, nullptr)) {
            session->cancel();
        }
        return;
    }
    if (method == u"initialized"_s) {
        return;
    }
}

void AcpServer::handleClientResponse(const QJsonObject &message)
{
    const QJsonValue idValue = message.value(u"id"_s);
    bool ok = false;
    const qint64 id = idValue.toVariant().toLongLong(&ok);
    if (!ok) {
        return;
    }
    const QJsonObject result = message.value(u"result"_s).toObject();
    const QJsonObject error = message.value(u"error"_s).toObject();
    const bool failed = !error.isEmpty();

    if (m_permissionRequests.contains(id)) {
        const QString sessionId = m_permissionRequests.take(id);
        const QString toolCallId = m_permissionToolCalls.take(id);
        AcpServerSession *session = m_sessions.value(sessionId, nullptr);
        if (!session) {
            return;
        }
        PermissionDecision decision = PermissionDecision::Deny;
        if (!failed) {
            QString option;
            const QJsonObject outcome = result.value(u"outcome"_s).toObject();
            option = outcome.value(u"optionId"_s).toString();
            if (option.isEmpty()) {
                option = result.value(u"optionId"_s).toString();
            }
            if (option == u"allow_once"_s) {
                decision = PermissionDecision::AllowOnce;
            } else if (option == u"allow_session"_s) {
                decision = PermissionDecision::AllowSession;
            }
        }
        session->resolvePermission(decision, toolCallId);
        return;
    }

    if (m_questionRequests.contains(id)) {
        const QString sessionId = m_questionRequests.take(id);
        AcpServerSession *session = m_sessions.value(sessionId, nullptr);
        if (!session) {
            return;
        }
        QString answer = result.value(u"answer"_s).toString();
        if (answer.isEmpty()) {
            answer = result.value(u"text"_s).toString();
        }
        const QString outcome = result.value(u"outcome"_s).toString();
        if (answer.isEmpty() && (failed || outcome == u"cancelled"_s)) {
            answer.clear();
        }
        session->answerQuestion(answer);
    }
}

void AcpServer::connectSession(const QString &sessionId, AcpServerSession *session)
{
    connect(session, &AcpServerSession::textDelta, this, [this, sessionId](const QString &delta) {
        sendSessionUpdate(sessionId,
                          QJsonObject{{u"sessionUpdate"_s, u"agent_message_chunk"_s},
                                      {u"content"_s,
                                       QJsonObject{{u"type"_s, u"text"_s},
                                                   {u"text"_s, delta}}}});
    });
    connect(session, &AcpServerSession::thinkingDelta, this,
            [this, sessionId](const QString &delta) {
                sendSessionUpdate(sessionId,
                                  QJsonObject{{u"sessionUpdate"_s, u"agent_thought_chunk"_s},
                                              {u"content"_s,
                                               QJsonObject{{u"type"_s, u"text"_s},
                                                           {u"text"_s, delta}}}});
            });
    connect(session, &AcpServerSession::planUpdated, this,
            [this, sessionId](const QJsonArray &plan) {
                QJsonArray entries;
                for (const QJsonValue &value : plan) {
                    const QJsonObject step = value.toObject();
                    const QString description = step.value(u"description"_s).toString();
                    if (description.isEmpty()) {
                        continue;
                    }
                    QString status = u"pending"_s;
                    if (step.value(u"completed"_s).toBool()) {
                        status = u"completed"_s;
                    } else if (step.value(u"inProgress"_s).toBool()) {
                        status = u"in_progress"_s;
                    }
                    entries.append(QJsonObject{
                        {u"content"_s,
                         QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, description}}},
                        {u"status"_s, status},
                        {u"priority"_s, u"medium"_s},
                    });
                }
                sendSessionUpdate(sessionId,
                                  QJsonObject{{u"sessionUpdate"_s, u"plan"_s},
                                              {u"entries"_s, entries}});
            });
    connect(session, &AcpServerSession::toolStarted, this,
            [this, sessionId](const PermissionRequest &request) {
                QJsonObject update;
                update.insert(u"sessionUpdate"_s, u"tool_call"_s);
                update.insert(u"toolCallId"_s, request.toolCallId);
                update.insert(u"title"_s,
                              request.summary.isEmpty() ? request.toolName : request.summary);
                update.insert(u"kind"_s, permissionKind(request.risk));
                update.insert(u"status"_s, u"in_progress"_s);
                QJsonArray content;
                if (!request.details.isEmpty()) {
                    content.append(QJsonObject{
                        {u"type"_s, u"content"_s},
                        {u"content"_s,
                         QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, request.details}}}});
                }
                update.insert(u"content"_s, content);
                if (request.details.startsWith(u'{')) {
                    const QJsonDocument raw = QJsonDocument::fromJson(request.details.toUtf8());
                    if (raw.isObject()) {
                        update.insert(u"rawInput"_s, raw.object());
                    }
                }
                sendSessionUpdate(sessionId, update);
            });
    connect(session, &AcpServerSession::toolFinished, this,
            [this, sessionId](const ToolResult &result) {
                QJsonObject update;
                update.insert(u"sessionUpdate"_s, u"tool_call_update"_s);
                update.insert(u"toolCallId"_s, result.toolCallId);
                update.insert(u"status"_s,
                              result.ok ? u"completed"_s
                                        : (result.cancelled ? u"cancelled"_s : u"failed"_s));
                QJsonArray content;
                content.append(QJsonObject{
                    {u"type"_s, u"content"_s},
                    {u"content"_s,
                     QJsonObject{{u"type"_s, u"text"_s},
                                 {u"text"_s, result.output.left(8000)}}}});
                update.insert(u"content"_s, content);
                sendSessionUpdate(sessionId, update);
            });
    connect(session, &AcpServerSession::permissionNeeded, this,
            [this, sessionId](const PermissionRequest &request) {
                const qint64 requestId = m_nextRequestId++;
                m_permissionRequests.insert(requestId, sessionId);
                m_permissionToolCalls.insert(requestId, request.toolCallId);
                QJsonObject toolCall;
                toolCall.insert(u"toolCallId"_s, request.toolCallId);
                toolCall.insert(u"title"_s,
                                request.summary.isEmpty() ? request.toolName : request.summary);
                toolCall.insert(u"kind"_s, permissionKind(request.risk));
                toolCall.insert(u"status"_s, u"pending"_s);
                if (!request.details.isEmpty()) {
                    toolCall.insert(u"content"_s,
                                    QJsonArray{QJsonObject{
                                        {u"type"_s, u"content"_s},
                                        {u"content"_s,
                                         QJsonObject{{u"type"_s, u"text"_s},
                                                     {u"text"_s, request.details}}}}});
                }
                QJsonArray options{
                    QJsonObject{{u"optionId"_s, u"allow_once"_s},
                                {u"name"_s, u"Allow once"_s},
                                {u"kind"_s, u"allow_once"_s}},
                    QJsonObject{{u"optionId"_s, u"allow_session"_s},
                                {u"name"_s, u"Allow for this session"_s},
                                {u"kind"_s, u"allow_always"_s}},
                    QJsonObject{{u"optionId"_s, u"reject_once"_s},
                                {u"name"_s, u"Reject"_s},
                                {u"kind"_s, u"reject_once"_s}},
                };
                QJsonObject params;
                params.insert(u"sessionId"_s, sessionId);
                params.insert(u"toolCall"_s, toolCall);
                params.insert(u"options"_s, options);
                QJsonObject outgoing;
                outgoing.insert(u"jsonrpc"_s, u"2.0"_s);
                outgoing.insert(u"id"_s, requestId);
                outgoing.insert(u"method"_s, u"session/request_permission"_s);
                outgoing.insert(u"params"_s, params);
                writeMessage(outgoing);
            });
    connect(session, &AcpServerSession::questionAsked, this,
            [this, sessionId](const PermissionRequest &request) {
                const qint64 requestId = m_nextRequestId++;
                m_questionRequests.insert(requestId, sessionId);
                QJsonObject params;
                params.insert(u"sessionId"_s, sessionId);
                params.insert(u"question"_s,
                              request.questionText.isEmpty() ? request.summary
                                                             : request.questionText);
                params.insert(u"options"_s, QJsonArray::fromStringList(request.questionOptions));
                params.insert(u"allowFreeform"_s, request.allowFreeform);
                QJsonObject outgoing;
                outgoing.insert(u"jsonrpc"_s, u"2.0"_s);
                outgoing.insert(u"id"_s, requestId);
                outgoing.insert(u"method"_s, u"_kateai/ask_user"_s);
                outgoing.insert(u"params"_s, params);
                writeMessage(outgoing);
            });
    connect(session, &AcpServerSession::notice, this, [this, sessionId](const QString &text) {
        sendNotification(u"_kateai/notice"_s,
                         QJsonObject{{u"sessionId"_s, sessionId}, {u"message"_s, text}});
    });
    connect(session, &AcpServerSession::finished, this,
            [this, sessionId](bool ok, const QString &stopReason, const QString &error) {
                finishSessionPrompt(sessionId, ok, stopReason, error);
            });
}

void AcpServer::finishSessionPrompt(const QString &sessionId, bool ok,
                                    const QString &stopReason, const QString &error)
{
    if (!m_pendingPrompts.contains(sessionId)) {
        if (!ok && !error.isEmpty()) {
            sendNotification(u"_kateai/notice"_s,
                             QJsonObject{{u"sessionId"_s, sessionId}, {u"message"_s, error}});
        }
        return;
    }
    const QJsonValue id = m_pendingPrompts.take(sessionId);
    if (!ok && !error.isEmpty()) {
        sendError(id, kServerError, error);
        return;
    }
    QString reason = stopReason;
    if (reason.isEmpty()) {
        reason = u"end_turn"_s;
    }
    if (reason == u"permission"_s || reason == u"question"_s) {
        reason = u"end_turn"_s;
    }
    sendResult(id, QJsonObject{{u"stopReason"_s, reason}});
}

bool AcpServer::hasPendingPrompt(const QString &sessionId) const
{
    return m_pendingPrompts.contains(sessionId);
}

void AcpServer::touchSession(const QString &sessionId)
{
    m_sessionActivity.insert(sessionId, QDateTime::currentMSecsSinceEpoch());
}

void AcpServer::reapIdleSessions()
{
    if (m_sessions.isEmpty()) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QStringList ids = m_sessions.keys();
    for (const QString &sessionId : ids) {
        // A turn in flight, or a permission/question the client has not
        // answered, is live work: only a session with nothing outstanding is a
        // candidate for reaping.
        if (m_pendingPrompts.contains(sessionId)) {
            touchSession(sessionId);
            continue;
        }
        bool waitingOnClient = false;
        for (auto it = m_permissionRequests.cbegin(); it != m_permissionRequests.cend(); ++it) {
            if (it.value() == sessionId) {
                waitingOnClient = true;
                break;
            }
        }
        if (!waitingOnClient) {
            for (auto it = m_questionRequests.cbegin(); it != m_questionRequests.cend(); ++it) {
                if (it.value() == sessionId) {
                    waitingOnClient = true;
                    break;
                }
            }
        }
        if (waitingOnClient) {
            touchSession(sessionId);
            continue;
        }
        const qint64 last = m_sessionActivity.value(sessionId, now);
        if (now - last < kIdleSessionMs) {
            continue;
        }
        AcpServerSession *session = m_sessions.take(sessionId);
        m_sessionActivity.remove(sessionId);
        if (session) {
            session->deleteLater();
        }
    }
}

QString AcpServer::permissionKind(ToolRisk risk) const
{
    switch (risk) {
    case ToolRisk::Read:
        return u"read"_s;
    case ToolRisk::Execute:
        return u"execute"_s;
    case ToolRisk::Write:
    default:
        return u"edit"_s;
    }
}

} // namespace KateAi
