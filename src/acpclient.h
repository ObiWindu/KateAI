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

namespace KateAi {
class ProjectGraph;
}
class QTimer;

namespace KateAi
{

// MiniMax Code refuses session/new unless its OAuth credential files under
// ~/.minimax/auth are owner-only, and the agent itself can leave them readable
// by group or other. Tightens them to 0600 before the agent is launched; a
// no-op when they are already private or do not exist.
void acpTightenMinimaxCredentials();

// Client for the Agent Client Protocol (JSON-RPC over a stdio subprocess).
// Kate is the ACP client; the agent is any stdio ACP implementation
// configured in Settings (Grok Build, Claude Agent, Gemini CLI, …).
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
    void setProjectGraph(ProjectGraph *graph);
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

    // Command used to launch the agent. Grok Build also receives --model /
    // --always-approve; other agents get the configured args unchanged.
    static QString resolvedCommand(const Settings &settings);
    static QStringList agentArguments(const Settings &settings);

    // Model id the agent last reported as selected.
    QString currentModelId() const
    {
        return m_currentModelId;
    }
    // True while session/set_config_option or session/set_model is in flight.
    bool modelChangePending() const
    {
        return m_modelSetInFlight;
    }

    // Effort/thought levels the agent advertised as config options
    // (category thought_level, or an id like thinking_level / effort).
    QStringList agentEfforts() const
    {
        return m_agentEfforts;
    }
    // Effort option id the agent last reported as selected.
    QString currentEffortId() const
    {
        return m_currentEffortId;
    }

    // Read the model list from the live session. Starts the agent when it
    // is not running so the list can arrive with session/new.
    void fetchModels();

    // Switch the session model. Prefers session/set_config_option (Zed's
    // path) and falls back to session/set_model. Returns false when this
    // process cannot apply the change and the caller should relaunch it.
    bool setModel(const QString &modelId);

    // Switch the session effort level through session/set_config_option.
    // An empty id means Auto: nothing is sent and the agent keeps its own
    // level. Returns false when no live session or effort option exists.
    bool setEffort(const QString &effortId);

Q_SIGNALS:
    void ready();
    void failed(const QString &error);
    void textDelta(const QString &delta);
    void thinkingDelta(const QString &delta);
    void planUpdated(const QJsonArray &plan);
    // An ACP agent called the Kate-specific suggest client method. ChatWidget
    // turns this into the same suggestion card the native tool produces.
    void suggestionPosted(const KateAi::Suggestion &suggestion);
    void toolStarted(const PermissionRequest &request);
    void toolFinished(const ToolResult &result);

    // Partial output of a running tool call. An ACP agent reports execution
    // through repeated tool_call_update notifications long before it reports a
    // final status, and that incremental text is what lets a card stream
    // instead of sitting blank until the command ends.
    void toolProgress(const QString &toolCallId, const QString &partialOutput);

    void permissionNeeded(const PermissionRequest &request);
    void promptFinished(const QString &stopReason, const QString &text, const QJsonArray &toolCalls);
    void logMessage(const QString &message);
    void sessionIdChanged(const QString &sessionId);
    void modelsReceived(const QStringList &models);
    void effortsReceived(const QStringList &efforts);

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
    // The session's cwd: the workspace when one is known, the process working
    // directory otherwise. Agents reject a non-absolute cwd (opencode answers
    // session/new with "cwd must be an absolute path: "), and a client spawned
    // from the Kate config page has no workspace to offer.
    QString sessionCwd() const;
    void sendPromptNow(const QString &text);

    int sendRequest(const QString &method, const QJsonObject &params,
                    std::function<void(const QJsonObject &, const QString &)> done = {},
                    int timeoutMs = 30000);
    void sendNotification(const QString &method, const QJsonObject &params);
    void writeMessage(const QJsonObject &message);
    void replyResult(const QJsonValue &id, const QJsonObject &result);
    void replyError(const QJsonValue &id, int code, const QString &message);
    // Answers whichever permission request is waiting with a "cancelled" outcome
    // and clears the request state. An agent that asks twice must never leave the
    // first reply unsent, because an unanswered session/request_permission blocks
    // that agent until the whole session is gone.
    void cancelPendingPermission();
    // Emits toolFinished for every tool still open and forgets them, so a card is
    // never left spinning because the turn it belonged to is gone.
    void finishLiveTools(const QString &message);

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
    void handleWritePlanFile(const QJsonValue &id, const QJsonObject &params);
    void handleSuggest(const QJsonValue &id, const QJsonObject &params);
    void handleQueryProjectGraph(const QJsonValue &id, const QJsonObject &params);
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

    // Pull the model selector out of a session/new, session/load, or
    // config_option_update payload and emit modelsReceived.
    void ingestSessionModels(const QJsonObject &result, bool applyPreferred);
    void sendSetConfigOption(const QString &modelId);
    void sendSetSessionModel(const QString &modelId);
    // Same job for the effort/thought-level selector: collect the advertised
    // options, re-apply the configured level when the agent drifted away from
    // it, and emit effortsReceived.
    void ingestSessionEfforts(const QJsonObject &result);
    void applyForcedEffort();
    void sendSetEffortOption(const QString &effortId);

    Settings m_settings;
    QString m_workspace;
    QString m_editorContext;
    QString m_resumeSessionId;
    DocumentBridge *m_bridge = nullptr;
    QList<McpServerConfig> m_mcpServers;

    ProjectGraph *m_projectGraph = nullptr;

    State m_state = State::Stopped;
    QString m_status;
    QProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;
    QString m_stderrBuffer;
    bool m_antigravityAuthUrlOpened = false;
    bool m_discardingStdoutFrame = false;
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

    // The last partial output streamed for each live tool call, so an agent that
    // repeats identical content on a heartbeat does not force a re-render.
    QHash<QString, QString> m_toolStreamed;

    QList<QueuedPrompt> m_queued;

    QJsonValue m_permissionRpcId;
    QJsonArray m_permissionOptions;
    PermissionRequest m_permissionRequest;

    QHash<QString, Terminal *> m_terminals;
    int m_nextTerminal = 1;

    QPointer<QTimer> m_startupTimer;
    // Bounded wait for an agent to answer session/cancel. One per attempt: a
    // repeated Stop must not leave a second live timer behind, and the timer
    // must die when the prompt ends for any other reason.
    QPointer<QTimer> m_cancelGraceTimer;
    QStringList m_agentModels;
    QString m_currentModelId;
    QString m_modelConfigId;
    bool m_legacySetModel = false;
    bool m_modelSetInFlight = false;

    QStringList m_agentEfforts;
    QString m_currentEffortId;
    QString m_effortConfigId;
    bool m_effortSetInFlight = false;
};

} // namespace KateAi
