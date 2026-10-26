/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "acpclient.h"
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
    QString workspace() const { return m_workspace; }
    void setDocumentBridge(DocumentBridge *bridge);
    void setEditorContext(const QString &context);

    void updateProjectGraph(const QString &filePath, const QString &content);
    ProjectGraph *getProjectGraph() const { return m_projectGraph.get(); }
    QList<GraphNode *> getProjectNodes() const;
    QList<GraphEdge *> getProjectEdges() const;

    // Native ACP owns its prompt lifecycle separately from the controller's
    // turn flag. Keep the UI in its active/stop state for the full ACP prompt.
    bool isBusy() const { return m_busy || (usesAcpNative(m_settings) && m_acp.isBusy()); }

    LlmClient *client() { return &m_client; }
    AcpClient *acpClient() { return &m_acp; }
    DocumentBridge *documentBridge() { return m_bridge; }

    void start(const QString &userText);
    void abort();
    void resetConversation();
    void resolvePermission(PermissionDecision decision);
    // Answers the blocking `question` tool with the user's text; the agent loop
    // resumes from the answer.
    void answerQuestion(const QString &answer);
    // Reclaims a full context window by folding the whole conversation into a
    // summary that keeps the live user turn. Returns false (and does nothing)
    // while a turn is running or when there is nothing worth compacting.
    bool compactContext();
    void fetchModels(Provider provider);
    void fetchAcpModels();

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
    // The answer streamed so far was discarded (a retry replaces it); the UI
    // has to drop what it already rendered.
    void assistantReset();
    void thinkingDelta(const QString &delta);
    void thinkingFinished(const QString &text);
    void assistantFinished(const QString &text);
    void planUpdated(const QJsonArray &plan);
    // An ACP agent posted a suggestion through the Kate client method; the
    // native suggest tool reaches the same card through toolStarted instead.
    void suggestionPosted(const Suggestion &suggestion);
    void toolStarted(const PermissionRequest &request);
    void toolFinished(const ToolResult &result);
    void permissionNeeded(const PermissionRequest &request);
    // The agent asked the user something with the `question` tool and is
    // waiting. The request carries the question and the suggested answers.
    void questionAsked(const PermissionRequest &request);
    // Partial output from a running tool, so its card can stream instead of
    // waiting for the final result.
    void toolProgress(const QString &toolCallId, const QString &partialOutput);
    // The context was reset to a summary: carries the token count before and
    // after, so the caller can report what was reclaimed.
    void contextCompacted(int beforeTokens, int afterTokens);
    void statusChanged(const QString &status);
    void activityUpdated(const QString &text);
    void failed(const QString &error);
    void turnFinished();
    void modelsReceived(Provider provider, const QStringList &models);
    void modelsFailed(Provider provider, const QString &error);
    // Effort/thought levels the live ACP agent advertised for its session.
    void effortsReceived(const QStringList &efforts);
    void modesChanged();
    void mcpToolsChanged();
    void mcpStatusChanged(const QString &summary);
    void mcpLogMessage(const QString &message);
    void checkpointCreated(const QString &id, const QString &label);
    void checkpointFailed(const QString &error);
    void checkpointRestored(const QString &id);
    void subtaskStarted(const QString &taskId, const QString &agentId, const QString &agentName, const QString &modeId, const QString &description);
    void subtaskActivity(const QString &taskId, const QString &line, bool isError);
    void subtaskMessageDelta(const QString &taskId, const QString &delta);
    void subtaskFinished(const QString &taskId, bool ok);

private:
    enum class State {
        Idle,
        WaitingForNextModel,
        WaitingForModel,
        ExecutingTools,
        WaitingForPermission,
        WaitingForQuestion,
    };

    void scheduleNextModelStep();
        // Milliseconds to wait before the next provider call, or 0 when the
        // requests-per-minute budget allows it now.
        qint64 rateLimitDelayMs(qint64 now) const;
    void sendToModel();
    void startAcpTurn(const QString &userText);
    void onAcpPromptFinished(const QString &stopReason, const QString &text, const QJsonArray &toolCalls);
    void syncAcpClient();
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
    // Gives every tool call of the last assistant message that has no result
    // yet a cancelled result, so the next request is not rejected by the
    // provider.
    void appendCancelledToolResults();
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
    // The one tool that stops the turn and waits for the user to type.
    bool isQuestionTool(const QString &toolName) const;
    // Issues a `bash` and reports back through finishAsyncTool. The queue holds
    // while it runs, so only one command is ever in flight; unlike the other
    // async tools it streams its output through toolProgress.
    void startBashTool(const ToolCall &call);
        bool isWebTool(const QString &toolName) const;
        // Issues a web_search / web_fetch and reports back through finishAsyncTool.
        void startWebTool(const ToolCall &call);
    // Takes the shared edit lock for a mutating tool, if the team has one.
    bool acquireEditLock(const ToolCall &call, QString *deniedBy);
    void releaseEditLocksFor(const ToolCall &call);
    // Pushes the active mode's tool set (and the MCP tools) to the client.
    void syncToolAccess();
    // Rebuilds the sandbox and the tool runner from the current settings and
    // workspace. Destroys any in-flight bash process, so it must not run
    // while a turn is executing; the callers defer it to the turn's end.
    void rebuildTools();
    // Defers a tool call until the pre-mutation snapshot has been taken.
    // Returns true when the call was parked; the snapshot's completion (or
    // failure) resumes it through resumeAfterCheckpoint().
    bool deferForCheckpoint(const ToolCall &call, bool asBash);
    // Runs the tool call parked by deferForCheckpoint() after the snapshot
    // reported back. A no-op when nothing was parked.
    void resumeAfterCheckpoint();
    // Records a tool call as executed, so identical repeats can be told apart
    // from retries of a call the user denied or that never ran.
    void markActionExecuted(const QString &signature);
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
    AcpClient m_acp;
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
    // Paths this loop currently holds a lock on.
    QStringList m_owedPaths;
    int m_subtaskDepth = 0;
    quint64 m_asyncCounter = 0;
    bool m_checkpointTaken = false;
    // A snapshot is being taken for the call below; the queue is held open
    // until it reports back, so no mutation can slip past the checkpoint.
    bool m_awaitingCheckpoint = false;
    ToolCall m_checkpointCall;
    bool m_checkpointIsBash = false;
    // The sandbox/tool-runner configuration changed while a turn was running;
    // rebuildTools() applies it once the turn ends.
    bool m_sandboxDirty = false;
    QString m_taskSummary;

    QList<ChatMessage> m_messages;
    QList<ToolCall> m_queue;
    QList<ToolResult> m_pendingResults;
    ToolCall m_waitingCall;
    PermissionRequest m_waitingRequest;
    // The `bash` call currently streaming, if any. One at a time: the queue
    // waits for it, so nothing else is executing while it is set.
    QString m_runningBashId;
    // Connection of the current bash call's finished handler. Kept so the next
    // command can drop the previous one: without it every command leaks a
    // connection that fires (and early-returns) on every later result.
    QMetaObject::Connection m_bashFinishedConnection;

    bool m_busy = false;
    State m_state = State::Idle;
    int m_modelRequests = 0;
    int m_toolCalls = 0;
    bool m_cancelRequested = false;
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
    int m_repeatedTruncatedObservationCount = 0;
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
