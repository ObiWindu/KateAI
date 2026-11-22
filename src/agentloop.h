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
#include "harness/harnessflags.h"
#include "harness/checks.h"
#include "harness/eventstore.h"
#include "harness/hooks.h"
#include "harness/middleware.h"
#include "harness/policy.h"
#include "harness/turn.h"
#include "llmclient.h"
#include "lsp.h"
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
#include <QMutex>
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

    // Explicit lifecycle of a user turn. The internal state machine has more
    // detail; this is the stable, UI-facing vocabulary. A turn is Prepared
    // when the user message is accepted, Running while the loop schedules a
    // model step, AwaitingModel while the provider request is in flight,
    // AwaitingTool while tools run, AwaitingPermission while the user has to
    // decide, and one of the terminal states afterwards.
    enum class TurnState {
        Idle,
        Preparing,
        Running,
        AwaitingModel,
        AwaitingTool,
        AwaitingPermission,
        Cancelling,
        Completed,
        Failed,
        Interrupted,
    };
    Q_ENUM(TurnState)

    // Stable machine-readable id for logging and tests (for example
    // "awaiting-model"). Never translated.
    static QString turnStateId(TurnState state);
    // Unique identifier of the current (or most recent) turn. A new id is
    // issued for every start(); async results from an older id must never
    // settle the newer turn.
    QString turnId() const
    {
        return m_turnId;
    }
    TurnState turnState() const;
    static bool isTerminalTurnState(TurnState state);

    void setSettings(const Settings &settings);
    void setWorkspace(const QString &workspace);
    QString workspace() const { return m_workspace; }
    // Tool-call id of an ACP agent question whose card is still answerable.
    // The turn can end before the user answers; the card survives until then.
    QString pendingAcpToolQuestionId() const { return m_acpToolQuestionId; }
    void setDocumentBridge(DocumentBridge *bridge);
    void setEditorContext(const QString &context);
    // The editor's language-server surface (KateLspBridge). Null in tests and
    // headless use: the LSP tools then report the feature as unavailable.
    void setLspProvider(LspProvider *provider);
    void setDebugProvider(DebugProvider *provider);
    void setEcosystemProvider(EcosystemProvider *provider);

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
    // Identity-checked variant: a decision from an old card must not approve
    // the request that replaced it.
    void resolvePermission(PermissionDecision decision, const QString &toolCallId);
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
    QString createCheckpoint(const QString &label, QString *error = nullptr);
    bool restoreCheckpoint(const QString &id, QString *error);
    QString diffAgainstCheckpoint(const QString &id) const;

    const QList<ChatMessage> &messages() const { return m_messages; }

    // --- Session persistence -------------------------------------------------
    SessionStore::SessionData sessionData() const;
    void restoreSession(const SessionStore::SessionData &data);
    void clearSession();
    // The conversation the harness event log writes to. Empty means "ask the
    // session store for the active conversation at the next event".
    void setConversationId(const QString &conversationId);
    QString conversationId() const;

    // Runs the discovered check plan now and returns the evidence. It never
    // changes the turn state; the UI uses it for ":kateai verify".
    QString verificationReport();
    // A human-readable summary of the effective policy files and rules.
    QString policyReport() const;
    QString hooksReport() const;
    // Approves the current project's hook file and remembers the approval
    // under its workspace hash. Returns false when the hooks flag is off or
    // there is no workspace.
    bool trustProjectHooks();
    // Exports the conversation's event log as JSONL. Returns false and fills
    // `error` when the log is unavailable.
    bool exportTrace(const QString &path, QString *error = nullptr);

    // Plain-text reports for ":kateai context" and ":kateai usage". Both are
    // best-effort estimates: the provider's own usage counters are not exposed
    // by every transport yet, so the report says what it measured.
    QString contextReport() const;
    QString usageReport() const;

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
    // Also emitted for ACP: an agent's ask_user client method or a question
    // tool call, so every provider parks the same way.
    void questionAsked(const PermissionRequest &request);
    // A question card shown for an earlier ACP tool call is no longer the one
    // the user should answer. The numbered id is the card's tool call id.
    void questionCancelled(const QString &toolCallId);
    // The agent called exit_plan_mode: the UI should leave plan mode (when it
    // is on) so the turn can start editing.
    void exitPlanModeRequested();
    // Partial output from a running tool, so its card can stream instead of
    // waiting for the final result.
    void toolProgress(const QString &toolCallId, const QString &partialOutput);
    // An ACP tool call gained a diff or path after it started. Forwarded from
    // the client so the card and the review queue can catch up.
    void toolDetailsChanged(const PermissionRequest &request);
    // Something the user should know that is not a tool result, such as a tool
    // the active mode does not allow.
    void noticeRaised(const QString &text);
    // The context was reset to a summary: carries the token count before and
    // after, so the caller can report what was reclaimed.
    void contextCompacted(int beforeTokens, int afterTokens);
    void statusChanged(const QString &status);
    void activityUpdated(const QString &text);
    void failed(const QString &error);
    // The turn state changed. Both arguments are stable ids; the turn id lets
    // a consumer discard an event from a turn it no longer displays.
    void turnStateChanged(const QString &turnId, const QString &stateId);
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
    void subtaskProgress(const QString &taskId, const QString &partialOutput);
    void subtaskFinished(const QString &taskId, bool ok);
    // A durable harness event was appended (or would have been, when the log is
    // disabled). Consumers can show it in a trace view or a test.
    void harnessEvent(const QString &type, const QJsonObject &payload);

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
    // The decision on an ACP permission request the user does not have to see:
    // a tool the active mode does not allow is declined, a session grant or the
    // auto-approve list allows, and the accept-edits and always-approve modes
    // allow. Returns false when the user has to decide.
    bool acpAutoDecision(const PermissionRequest &request, PermissionDecision *decision, QString *reason) const;
    // True when the active mode lets the agent use the tool.
    bool acpToolAllowed(const PermissionRequest &request) const;
    void finishTurn();
    void finishWithFailure(const QString &error);
    bool canStartModelRequest(QString *error) const;
        // The history as it should be sent: budgeted and compacted to fit the
        // model's context window.
        QList<ChatMessage> m_messagesForRequest() const;
        // Replace the stored transcript with the compacted request history
        // once compaction has made a material difference. Without this the
        // full session stayed in memory and was rewritten to SQLite on every
        // save, so memory and save cost grew without bound.
        void maybeCompactStoredHistory(const QList<ChatMessage> &requestMessages);
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
    // Ambient editor/workspace/git state for the current request. It is added
    // as a transient user message so the static system prefix stays stable for
    // provider prompt caching.
    QString turnContextMessage() const;
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
    // UI-state side effects of a finished tool (todo list, reasoning, plan-mode
    // exit); separate from the history because they drive signals, not messages.
    void handleToolSideEffects(const ToolCall &call, const ToolResult &result);
    // Issues a `bash` and reports back through finishAsyncTool. The queue holds
    // while it runs, so only one command is ever in flight; unlike the other
    // async tools it streams its output through toolProgress.
    void startBashTool(const ToolCall &call);
    // Issues a read-only disk tool (list_dir/grep/glob) on a worker thread and
    // reports back through finishAsyncTool, like startBashTool but without the
    // one-at-a-time serialization. read_file stays synchronous because it must
    // see the editor's unsaved buffer.
    void startReadTool(const ToolCall &call);
        bool isWebTool(const QString &toolName) const;
        // Issues a web_search / web_fetch and reports back through finishAsyncTool.
        void startWebTool(const ToolCall &call);
    // Unified args: parseToolArgs prefers pre-parsed `arguments`, falls back to
    // `argumentsJson`. Permission, locks and cards must use this, not
    // `call.arguments` directly, or LLM-path calls bypass policy.
    QJsonObject effectiveArgs(const ToolCall &call) const;
    // Takes the shared edit lock for a mutating tool, if the team has one.
    bool acquireEditLock(const ToolCall &call, QString *deniedBy);
    void releaseEditLocksFor(const ToolCall &call);
    // Every file a mutating call writes. A patch names several; the other
    // edit tools name one. Used for locks and changed-path bookkeeping.
    QStringList mutationPathsForCall(const ToolCall &call) const;
    // The key a path is locked under. A relative path is resolved against the
    // workspace and cleaned, so "src/../src/a.cpp" and "src/a.cpp" name one
    // lock. Acquire and release must use the same key, so both go through
    // here.
    QString lockKeyForPath(const QString &path) const;
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
    void clearCheckpointWait();    // Clears the checkpoint-wait bookkeeping. Used when a turn is aborted or
    // reset while a snapshot is still running.
    void markActionExecuted(const QString &signature);
    void syncPermissionPolicy();
    // Loads policy/hook configuration after the workspace or flags change.
    void reloadHarnessConfig();
    // The conversation the event log writes to, or empty when no session is
    // active. Never creates a conversation as a side effect.
    QString currentConversationId() const;
    QString currentStepId();
    // Appends one event; does nothing when the event log is disabled or no
    // conversation is active. Payloads are redacted by Events::encode.
    void emitHarnessEvent(const QString &type, const QJsonObject &payload,
                          const QString &stepId = QString());
    // Runs the configured PreToolUse hooks. Returns false when a hook blocked
    // the call (a result has already been appended); may rewrite `call`.
    bool applyPreToolHooks(ToolCall &call);
    void applyPostToolHooks(const ToolCall &call, ToolResult &result);
    bool runStopHooks();
    // Appends any hook-provided context as a controller message once the batch
    // has no more tool results to send first.
    void flushHookContext();
    // Returns true when the verification gate handled the stop (scheduled a
    // retry or finished the turn); false lets the legacy nudge run.
    bool runVerificationGate();
    // Records a permission decision in the event log and the audit file.
    void recordPermissionDecision(const QString &toolName, const QString &decision,
                                  const QString &ruleId, const QString &reason);
    bool beginHarnessTurn(const QString &userText);
    void endHarnessTurn(const QString &status, const QString &detail = QString());
    void setSubtaskDepth(int depth)
    {
        m_subtaskDepth = depth;
    }

    Settings m_settings;
    QString m_workspace;
    QString m_editorContext;
    DocumentBridge *m_bridge = nullptr;
    LspProvider *m_lsp = nullptr;
    DebugProvider *m_debug = nullptr;
    EcosystemProvider *m_ecosystem = nullptr;
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
    // ACP turn whose prompt is held until the pre-turn snapshot finishes.
    QString m_pendingAcpPrompt;
    // The sandbox/tool-runner configuration changed while a turn was running;
    // rebuildTools() applies it once the turn ends.
    bool m_sandboxDirty = false;
    QString m_taskSummary;

    QList<ChatMessage> m_messages;
    QList<ToolCall> m_queue;
    QList<ToolResult> m_pendingResults;
    ToolCall m_waitingCall;
    PermissionRequest m_waitingRequest;
    // An agent-side ACP question tool call whose card is on screen. ACP has no
    // client-to-agent tool-result channel, so its answer is sent as the next
    // prompt once the current turn closes.
    QString m_acpToolQuestionId;
    QString m_pendingAcpAnswer;
    // The `bash` call currently streaming, if any. One at a time: the queue
    // waits for it, so nothing else is executing while it is set.
    QString m_runningBashId;
    // Connection of the current bash call's finished handler. Kept so the next
    // command can drop the previous one: without it every command leaks a
    // connection that fires (and early-returns) on every later result.
    QMetaObject::Connection m_bashFinishedConnection;

    bool m_busy = false;
    State m_state = State::Idle;
    // Lifecycle: the id of the turn the UI is currently describing, and the
    // terminal state of the most recent turn (Idle until one ends).
    QString m_turnId;
    QString m_lastFailedTurn;
    TurnState m_lastTurnState = TurnState::Idle;
    void setTurnState(TurnState state, const QString &reason = QString());
    bool m_terminalSignalled = true;
    // Incremented when a turn starts or is aborted. Async work captures it so
    // a result that lands after the turn changed cannot settle the new turn.
    quint64 m_turnGeneration = 0;
    int m_modelRequests = 0;
    int m_toolCalls = 0;
    bool m_cancelRequested = false;
    // Extra compaction pressure after a context-window rejection. Reset at
    // the start of each user turn so a later overflow can compact further
    // instead of aborting the prompt.
    int m_compactionLevel = 0;
    int m_contextOverflowRetries = 0;
    mutable QQueue<qint64> m_modelRequestTimes;
    // Guard for the pacing queue; mutable so the const delay query can prune
    // expired entries without const_cast.
    mutable QMutex m_rateMutex;
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
    // Per-request ambient context, cached for the turn so a long turn does not
    // run `git status` on every model call. Keyed by turn id.
    mutable QString m_cachedTurnContext;
    mutable QString m_cachedTurnContextTurnId;
    HarnessFlags m_harness;

    // --- Harness 0.7 ---------------------------------------------------------
    EventStore m_events;
    PolicyEngine m_policyEngine;
    HookRegistry m_hooks;
    TurnContext m_turnContext;
    TurnController m_turnController;
    MiddlewareChain m_middleware;
    // Owned by m_middleware; reset whenever the chain is rebuilt.
    VerificationMiddleware *m_verification = nullptr;
    QString m_conversationId;
    QString m_currentStepId;
    QString m_pendingHookContext;
    QString m_hookLoadError;
    int m_stepNumber = 0;
    bool m_eventLogBroken = false;
    bool m_sessionStartRan = false;
    bool m_verificationEvidenceSent = false;
    bool m_harnessConfigLoaded = false;
    QString m_harnessConfigWorkspace;
};

} // namespace KateAi
