/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "agentlocks.h"
#include "agentteam.h"
#include "checkpoint.h"
#include "documentbridge.h"
#include "llmclient.h"
#include "mcp.h"
#include "modes.h"
#include "permissions.h"
#include "sandbox.h"
#include "sessionstore.h"
#include "tools.h"
#include "types.h"
#include "websearch.h"
#include "graph/projectgraph.h"

#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QHash>
#include <QTimer>
#include <memory>

namespace KateAi
{

class AgentLoop : public QObject
{
    Q_OBJECT

public:
    explicit AgentLoop(QObject *parent = nullptr);
        ~AgentLoop() override;

    void setSettings(const Settings &settings);
    void setWorkspace(const QString &workspace);
    void setDocumentBridge(DocumentBridge *bridge);
    void setEditorContext(const QString &context);

    void updateProjectGraph(const QString &filePath, const QString &content);
    ProjectGraph *getProjectGraph() const { return m_projectGraph.get(); }
    QList<GraphNode *> getProjectNodes() const;
    QList<GraphEdge *> getProjectEdges() const;

    bool isBusy() const { return m_busy; }

    LlmClient *client() { return &m_client; }
    DocumentBridge *documentBridge() { return m_bridge; }

    void start(const QString &userText);
    void abort();
    void resetConversation();
    void resolvePermission(PermissionDecision decision);
    void fetchModels(Provider provider);

    // --- Modes ----------------------------------------------------------------
    ModeRegistry *modeRegistry()
    {
        return &m_modes;
    }
    ModeDefinition activeMode() const
    {
        return m_modes.modeOrDefault(m_settings.agentMode);
    }
    // Tools the model may call right now, from the mode and plan mode.
    ToolAccess activeToolAccess() const;
    void setMode(const QString &modeId);

    // --- MCP ------------------------------------------------------------------
    McpManager *mcpManager() const
    {
        return m_mcp;
    }
    // Sub-agents borrow the pointer; ownership stays with the root loop.
    void setMcpManager(McpManager *manager);

    // --- Agent team -------------------------------------------------------------
    AgentTeam *agentTeam()
    {
        return &m_team;
    }
    // Shared with sub-agents so parallel edits cannot collide.
    void setLocks(WorkspaceLocks *locks);
    WorkspaceLocks *locks() const
    {
        return m_locks;
    }
    // Task ids of the sub-agents currently running.
    QStringList runningSubtaskIds() const;
    int runningSubtaskCount() const
    {
        return m_activeSubtasks.size();
    }
    // Stops one sub-agent, or every sub-agent when taskId is empty.
    void cancelSubtask(const QString &taskId = QString());
    // Identity used for the shared edit locks; unique per sub-agent.
    QString selfId() const
    {
        return m_selfId;
    }
    void setSelfId(const QString &id)
    {
        m_selfId = id;
    }
    // Marks this loop as deliberately stopped, so a turn that unwinds through
    // cancelSubtask() reports "cancelled" instead of a bogus success.
    void requestCancel()
    {
        m_cancelRequested = true;
    }
    bool cancelRequested() const
    {
        return m_cancelRequested;
    }

    // --- Checkpoints -----------------------------------------------------------
    CheckpointManager *checkpointManager()
    {
        return &m_checkpoints;
    }
    QList<CheckpointInfo> checkpoints() const;
    QString createCheckpoint(const QString &label);
    bool restoreCheckpoint(const QString &id, QString *error);
    QString diffAgainstCheckpoint(const QString &id) const;

    const QList<ChatMessage> &messages() const { return m_messages; }

    // Session persistence
    SessionStore::SessionData sessionData() const;
    void restoreSession(const SessionStore::SessionData &data);
    void clearSession();

    PermissionPolicy &policy() { return m_policy; }

Q_SIGNALS:
    void userMessage(const QString &text);
    void assistantDelta(const QString &delta);
    void thinkingDelta(const QString &delta);
    void thinkingFinished(const QString &text);
    void assistantFinished(const QString &text);
    void planUpdated(const QJsonArray &plan);
    void toolStarted(const PermissionRequest &request);
    void toolFinished(const ToolResult &result);
    void permissionNeeded(const PermissionRequest &request);
    void statusChanged(const QString &status);
    void activityUpdated(const QString &text);
    void failed(const QString &error);
    void turnFinished();
    void modelsReceived(Provider provider, const QStringList &models);
    void modelsFailed(Provider provider, const QString &error);
    void modesChanged();
    void mcpToolsChanged();
    void mcpStatusChanged(const QString &summary);
    void mcpLogMessage(const QString &message);
    void checkpointCreated(const QString &id, const QString &label);
    void checkpointFailed(const QString &error);
    void checkpointRestored(const QString &id);
    void subtaskStarted(const QString &taskId, const QString &agentId, const QString &agentName, const QString &modeId, const QString &description);
    void subtaskActivity(const QString &taskId, const QString &line, bool isError);
    void subtaskFinished(const QString &taskId, bool ok);

private:
    enum class State {
        Idle,
        WaitingForNextModel,
        WaitingForModel,
        ExecutingTools,
        WaitingForPermission,
    };

    void scheduleNextModelStep();
        // Milliseconds to wait before the next provider call, or 0 when the
        // requests-per-minute budget allows it now.
        qint64 rateLimitDelayMs(qint64 now) const;
    void sendToModel();
    void finishTurn();
    void finishWithFailure(const QString &error);
    bool canStartModelRequest(QString *error) const;
        // The history as it should be sent: budgeted and compacted to fit the
        // model's context window.
        QList<ChatMessage> m_messagesForRequest() const;
    QString actionSignature(const ToolCall &call) const;
    bool isRepeatSensitiveTool(const QString &toolName) const;
    void appendControllerMessage(const QString &content);
    QString formatToolResult(const ToolCall &call, const ToolResult &result) const;
    QString describePlannedWork(const QList<ToolCall> &calls) const;
    QString summarizeCompletedWork() const;
    bool isMutationTool(const QString &toolName) const;
    bool isVerificationTool(const QString &toolName) const;
    bool isVerificationForChangedFiles(const ToolCall &call) const;
    void appendToolResult(const ToolCall &call, ToolResult result);
    void appendToolResultsToConversation();
    void addBudgetFailureResults(const QString &reason);
    void requestVerificationTurn();
    void emitPlanUpdate();
    void onFinished(const QString &text, const QList<ToolCall> &toolCalls);
    void onFailed(const QString &error);
    void processQueue();
    void executeOne(const ToolCall &call);
    QString systemPrompt() const;
    QList<ToolCall> bundleSimilarTools(const QList<ToolCall> &calls);

    // Routes a tool call to MCP, a sub-agent, or the built-in runner.
    PermissionRequest describeTool(const ToolCall &call) const;
    // Comma-separated tool names the model may call right now.
    QString accessList() const;
    bool isAsyncTool(const QString &toolName) const;
    void dispatchAsyncTool(const ToolCall &call);
    void dispatchSubtask(const ToolCall &call);
    void finishAsyncTool(const ToolCall &call, const ToolResult &result);
    bool isSubtaskTool(const QString &toolName) const;
        bool isWebTool(const QString &toolName) const;
        // Issues a web_search / web_fetch and reports back through finishAsyncTool.
        void startWebTool(const ToolCall &call);
    // Takes the shared edit lock for a mutating tool, if the team has one.
    bool acquireEditLock(const ToolCall &call, QString *deniedBy);
    void releaseEditLocksFor(const ToolCall &call);
    // Pushes the active mode's tool set (and the MCP tools) to the client.
    void syncToolAccess();
    // Snapshots the workspace before the first mutation of a turn.
    void maybeCheckpoint(const QString &label);
    void syncPermissionPolicy();
    void setSubtaskDepth(int depth)
    {
        m_subtaskDepth = depth;
    }

    Settings m_settings;
    QString m_workspace;
    QString m_editorContext;
    DocumentBridge *m_bridge = nullptr;
    LlmClient m_client;
        // Shared by every loop in a turn, so the in-flight replies can be cancelled
        // as a unit when the turn is aborted.
        WebSearch m_web;
    PermissionPolicy m_policy;
    std::unique_ptr<Sandbox> m_sandbox;
    std::unique_ptr<ToolRunner> m_tools;
    std::unique_ptr<ProjectGraph> m_projectGraph;

    ModeRegistry m_modes;
    AgentTeam m_team;
    // Owned by the root loop only; sub-agents borrow the pointer. QPointer so
    // a sub-agent that outlives its parent sees null instead of a dangling one.
    QPointer<McpManager> m_mcp;
    bool m_ownsMcp = false;
    // Shared with sub-agents so parallel edits to one file cannot collide.
    // Borrowed, so it must be a QPointer for the same reason as m_mcp.
    QPointer<WorkspaceLocks> m_locks;
    CheckpointManager m_checkpoints;
    // taskId -> child loop, for every sub-agent running right now.
    QHash<QString, AgentLoop *> m_activeSubtasks;
    // How many async tools (MCP or sub-agent) have been dispatched but have
    // not reported back yet. The queue is held open while this is non-zero.
    int m_inFlight = 0;
    // Identity used for the shared edit locks.
    QString m_selfId = QStringLiteral("main");
    bool m_cancelRequested = false;
    // Paths this loop currently holds a lock on.
    QStringList m_owedPaths;
    int m_subtaskDepth = 0;
    quint64 m_asyncCounter = 0;
    bool m_checkpointTaken = false;
    QString m_taskSummary;

    QList<ChatMessage> m_messages;
    QList<ToolCall> m_queue;
    QList<ToolResult> m_pendingResults;
    ToolCall m_waitingCall;
    PermissionRequest m_waitingRequest;

    bool m_busy = false;
    State m_state = State::Idle;
    int m_modelRequests = 0;
    int m_toolCalls = 0;
    // Extra compaction pressure after a context-window rejection. Reset at
    // the start of each user turn so a later overflow can compact further
    // instead of aborting the prompt.
    int m_compactionLevel = 0;
    int m_contextOverflowRetries = 0;
    QQueue<qint64> m_modelRequestTimes;
    QTimer m_nextModelTimer;

    quint64 m_stateEpoch = 0;
    QSet<QString> m_actionSignatures;
    QHash<QString, int> m_actionRepeatCounts;
    QSet<QString> m_actionsThisModelTurn;
    int m_recoveryPromptCount = 0;
    QSet<QString> m_changedPaths;
    bool m_changesNeedVerification = false;
    bool m_verificationAttempted = false;
    int m_verificationPromptCount = 0;

    // Hidden reasoning and structured plan for the current turn.
    QString m_currentThinking;
    QJsonArray m_currentPlan;
    bool m_planShown = false;
    bool m_thinkingFinishedEmitted = false;

    QString m_currentAssistant;
};

} // namespace KateAi
