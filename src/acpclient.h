/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "documentbridge.h"
#include "mcp.h"
#include "sandbox.h"
#include "types.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QTimer;

namespace KateAi
{

// Client for the Agent Client Protocol (JSON-RPC over a stdio subprocess).
// Kate is the ACP client; Grok Build (`grok agent stdio`) is the agent.
class AcpClient : public QObject
{
    Q_OBJECT

public:
    explicit AcpClient(QObject *parent = nullptr);
    ~AcpClient() override;

    void setSettings(const Settings &settings);
    void setWorkspace(const QString &workspace);
    void setDocumentBridge(DocumentBridge *bridge);
    void setMcpServers(const QList<McpServerConfig> &servers);
    void setEditorContext(const QString &context);
    // Session id from a previous Kate conversation, used with session/load
    // or session/resume when the agent advertises those capabilities.
    void setResumeSessionId(const QString &sessionId);

    bool isReady() const
    {
        return m_state == State::Ready;
    }
    bool isBusy() const
    {
        return m_promptId != 0 || m_state == State::Starting;
    }
    bool hasPendingPermission() const
    {
        return m_permissionRpcId.isUndefined() == false && !m_permissionRpcId.isNull();
    }
    QString sessionId() const
    {
        return m_sessionId;
    }
    QString statusText() const
    {
        return m_status;
    }

    // Spawns the agent and runs initialize + session/new (or load/resume).
    void start();
    // Cancels the in-flight prompt. The process stays up for the next turn.
    void cancel();
    // Kills the subprocess and forgets the session.
    void stop();
    // Drops the current ACP session so the next prompt creates a new one.
    void resetSession();

    void prompt(const QString &text);
    void resolvePermission(PermissionDecision decision);

    // Command used to launch the agent, including model / always-approve flags.
    static QString resolvedCommand(const Settings &settings);
    static QStringList agentArguments(const Settings &settings);

Q_SIGNALS:
    void ready();
    void failed(const QString &error);
    void textDelta(const QString &delta);
    void thinkingDelta(const QString &delta);
    void planUpdated(const QJsonArray &plan);
    void toolStarted(const PermissionRequest &request);
    void toolFinished(const ToolResult &result);
    void permissionNeeded(const PermissionRequest &request);
    void promptFinished(const QString &stopReason, const QString &text, const QJsonArray &toolCalls);
    void logMessage(const QString &message);
    void sessionIdChanged(const QString &sessionId);

private:
    enum class State {
        Stopped,
        Starting,
        Ready,
        Failed,
    };

    struct Pending {
        int id = 0;
        QString method;
        std::function<void(const QJsonObject &result, const QString &error)> done;
        QPointer<QTimer> timeout;
    };

    struct Terminal {
        QString id;
        QProcess *process = nullptr;
        QByteArray output;
        int outputByteLimit = 1024 * 1024;
        bool truncated = false;
        bool exited = false;
        int exitCode = 0;
        QString signal;
        QList<QJsonValue> waiters;
    };

    struct QueuedPrompt {
        QString text;
    };

    void setState(State state, const QString &status);
    void spawnProcess();
    void sendInitialize();
    void maybeAuthenticate(const QJsonArray &authMethods);
    void openSession();
    void sendSessionNew();
    void sendSessionLoadOrResume();
    void sendPromptNow(const QString &text);

    int sendRequest(const QString &method, const QJsonObject &params,
                    std::function<void(const QJsonObject &, const QString &)> done = {},
                    int timeoutMs = 30000);
    void sendNotification(const QString &method, const QJsonObject &params);
    void writeMessage(const QJsonObject &message);
    void replyResult(const QJsonValue &id, const QJsonObject &result);
    void replyError(const QJsonValue &id, int code, const QString &message);

    void onStdioReadyRead();
    void handleBuffer();
    void handleMessage(const QJsonObject &message);
    void handleResponse(const QJsonObject &message);
    void handleIncomingRequest(const QJsonObject &message);
    void handleNotification(const QJsonObject &message);
    void handleSessionUpdate(const QJsonObject &update);
    void failAll(const QString &error);
    void finishPrompt(const QString &stopReason, const QString &error = QString());

    void handleFsRead(const QJsonValue &id, const QJsonObject &params);
    void handleFsWrite(const QJsonValue &id, const QJsonObject &params);
    void handleRequestPermission(const QJsonValue &id, const QJsonObject &params);
    void handleTerminalCreate(const QJsonValue &id, const QJsonObject &params);
    void handleTerminalOutput(const QJsonValue &id, const QJsonObject &params);
    void handleTerminalWait(const QJsonValue &id, const QJsonObject &params);
    void handleTerminalKill(const QJsonValue &id, const QJsonObject &params);
    void handleTerminalRelease(const QJsonValue &id, const QJsonObject &params);

    void appendTerminalOutput(Terminal *term, const QByteArray &chunk);
    void markTerminalExited(Terminal *term, int exitCode, const QString &signal);
    void killTerminal(Terminal *term, bool release);
    void releaseAllTerminals();
    QJsonArray mcpServersPayload() const;
    QJsonObject sessionMeta() const;
    QJsonArray promptBlocks(const QString &text) const;
    PermissionRequest permissionFromToolCall(const QJsonObject &toolCall) const;
    QString pickPermissionOption(PermissionDecision decision) const;
    bool autoResolvePermission(const PermissionRequest &request);
    QString sliceLines(const QString &text, int line, int limit) const;
    bool pathAllowed(const QString &path, bool forWrite, QString *error) const;
    Sandbox currentSandbox() const;
    QProcessEnvironment processEnvironment() const;

    Settings m_settings;
    QString m_workspace;
    QString m_editorContext;
    QString m_resumeSessionId;
    DocumentBridge *m_bridge = nullptr;
    QList<McpServerConfig> m_mcpServers;

    State m_state = State::Stopped;
    QString m_status;
    QProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;
    bool m_shuttingDown = false;

    int m_nextId = 1;
    QHash<int, Pending> m_pending;

    QString m_sessionId;
    bool m_loadSession = false;
    bool m_resumeSession = false;
    bool m_closeSession = false;
    bool m_embeddedContext = false;
    int m_protocolVersion = 1;

    int m_promptId = 0;
    QString m_promptText;
    QString m_currentText;
    QString m_currentThinking;
    QJsonArray m_currentPlan;
    QJsonArray m_toolCalls;
    QHash<QString, PermissionRequest> m_liveTools;
    QList<QueuedPrompt> m_queued;

    QJsonValue m_permissionRpcId;
    QJsonArray m_permissionOptions;
    PermissionRequest m_permissionRequest;

    QHash<QString, Terminal *> m_terminals;
    int m_nextTerminal = 1;

    QPointer<QTimer> m_startupTimer;
};

} // namespace KateAi
