/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "../types.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>

class QIODevice;
class QThread;
class QTimer;

namespace KateAi
{

/**
 * One ACP session backed by a harness loop.
 *
 * The protocol layer owns JSON-RPC and session ids; a host plugs its loop in
 * through this interface. Signals are emitted on the server's thread.
 */
class AcpServerSession : public QObject
{
    Q_OBJECT

public:
    explicit AcpServerSession(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~AcpServerSession() override = default;

    virtual void startPrompt(const QString &text) = 0;
    virtual void cancel() = 0;
    virtual void resolvePermission(PermissionDecision decision,
                                   const QString &toolCallId = QString()) = 0;
    virtual void answerQuestion(const QString &answer) = 0;

Q_SIGNALS:
    void textDelta(const QString &delta);
    void thinkingDelta(const QString &delta);
    void planUpdated(const QJsonArray &plan);
    void toolStarted(const PermissionRequest &request);
    void toolFinished(const ToolResult &result);
    void permissionNeeded(const PermissionRequest &request);
    void questionAsked(const PermissionRequest &request);
    void notice(const QString &text);
    void finished(bool ok, const QString &stopReason, const QString &error);
};

/**
 * Minimal ACP server (JSON-RPC over stdio).
 *
 * Implements initialize, session/new, session/prompt, session/cancel,
 * session/close, permission requests and `_kateai/ask_user`. The server never
 * executes tools itself: every session is a harness loop, so policy,
 * sandboxing, hooks and the event log stay identical to the plugin path.
 */
class AcpServer : public QObject
{
    Q_OBJECT

public:
    using SessionFactory = std::function<AcpServerSession *(const QString &sessionId,
                                                            const QString &cwd,
                                                            QString *error)>;

    explicit AcpServer(SessionFactory factory, QObject *parent = nullptr);
    ~AcpServer() override;

    // stdio transport. Reads on a worker thread and writes on the main thread.
    void startStdio();
    void stop();

    // In-process transport for tests: consume one line and write replies to
    // `output` (a QBuffer works). When null, replies go to stdout.
    void setOutputDevice(QIODevice *output);
    bool handleLine(const QByteArray &line, QString *error = nullptr);

    int sessionCount() const
    {
        return m_sessions.size();
    }
    bool hasPendingPrompt(const QString &sessionId) const;

Q_SIGNALS:
    void protocolError(const QString &message);
    void stdioClosed();

private:
    QJsonObject dispatch(const QJsonObject &message);
    void handleNotification(const QJsonObject &message);
    void handleClientResponse(const QJsonObject &message);
    void writeMessage(const QJsonObject &message);
    void sendResult(const QJsonValue &id, const QJsonObject &result);
    void sendError(const QJsonValue &id, int code, const QString &message);
    void sendNotification(const QString &method, const QJsonObject &params);
    void sendSessionUpdate(const QString &sessionId, const QJsonObject &update);
    void connectSession(const QString &sessionId, AcpServerSession *session);
    void finishSessionPrompt(const QString &sessionId, bool ok, const QString &stopReason,
                             const QString &error);
    QString permissionKind(ToolRisk risk) const;
    // Records that a session did something, so the idle sweep leaves it alone.
    void touchSession(const QString &sessionId);
    // Closes sessions that have had no prompt in flight and no traffic for
    // kIdleSessionMs, so an abandoned client cannot pin harness loops.
    void reapIdleSessions();

    SessionFactory m_factory;
    QHash<QString, AcpServerSession *> m_sessions;
    // session id -> last activity, in ms since the epoch. Only sessions that
    // saw traffic since the server started appear here.
    QHash<QString, qint64> m_sessionActivity;
    QTimer *m_idleTimer = nullptr;
    // session id -> pending session/prompt request id
    QHash<QString, QJsonValue> m_pendingPrompts;
    // outbound request id -> session id (permission/question requests)
    QHash<qint64, QString> m_permissionRequests;
    QHash<qint64, QString> m_questionRequests;
    QHash<qint64, QString> m_permissionToolCalls;
    qint64 m_nextRequestId = 1000;
    QIODevice *m_output = nullptr;
    QThread *m_readerThread = nullptr;
    QObject *m_reader = nullptr;
    bool m_initialized = false;
};

} // namespace KateAi
