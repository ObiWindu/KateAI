/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

enum class Provider {
    Grok,
    OpenAI,
    OpenRouter,
    OpenAICompatible,
    ClaudeCompatible,
};

enum class PermissionMode {
    Ask,
    AcceptEdits,
    AlwaysApprove,
};

enum class SandboxProfile {
    Off,
    Workspace,
    ReadOnly,
    Strict,
};

enum class PermissionDecision {
    AllowOnce,
    AllowSession,
    Deny,
};

enum class ToolRisk {
    Read,
    Write,
    Execute,
};

struct ChatMessage {
    enum class Role { System, User, Assistant, Tool };
    Role role = Role::User;
    QString content;
    // Hidden reasoning emitted by the model before the visible answer. Kept
    // separate so it can be collapsed in the transcript without losing context.
    QString thinking;
    // Optional structured plan attached to an assistant message. Rendered as a
    // checklist that is checked off as each step is completed.
    QJsonArray plan;
    QString toolCallId;
    QString name;
    QJsonArray toolCalls;
};

struct PlanStep {
    QString id;
    QString description;
    bool completed = false;
    bool inProgress = false;
};

struct ToolCall {
    QString id;
    QString name;
    QString argumentsJson;
    QJsonObject arguments;
};

struct CompletionChunk {
    QString contentDelta;
    QString thinkingDelta;
    QList<ToolCall> completedTools;
    bool finished = false;
    QString finishReason;
    QString error;
};

struct PermissionRequest {
    QString toolName;
    QString toolCallId;
    QString summary;
    QString details;
    QString path;
    // Pre-formatted unified diff for write_file, or the old/new strings for
    // edit_file. Rendered inline by the tool-call card in the chat transcript.
    QString describeDiff;
    ToolRisk risk = ToolRisk::Write;
};

struct ToolResult {
    QString toolCallId;
    QString name;
    QString output;
    bool ok = true;
};

struct Settings {
    Provider provider = Provider::Grok;
    QString grokApiKey;
    QString openaiApiKey;
    QString openrouterApiKey;
    QString openaiCompatibleApiKey;
    QString claudeCompatibleApiKey;
    QString grokModel = QStringLiteral("grok-4.5");
    QString openaiModel = QStringLiteral("gpt-4.1");
    QString openrouterModel = QStringLiteral("x-ai/grok-4");
    QString openaiCompatibleModel;
    QString claudeCompatibleModel;
    QString openaiCompatibleUrl = QStringLiteral("http://localhost:11434/v1");
    QString claudeCompatibleUrl = QStringLiteral("https://api.anthropic.com/v1");
    PermissionMode permissionMode = PermissionMode::Ask;
    SandboxProfile sandbox = SandboxProfile::Workspace;
    // Agent budgets are intentionally separate: API model turns, tool calls, and provider rate.
    int maxModelRequests = 40;
    int maxToolCalls = 80;
    int requestsPerMinute = 15;
    // Number of recent tool-call cards to keep expanded in the chat transcript.
    // Older cards are collapsed to keep the UI responsive during long runs.
    int maxExpandedToolCards = 20;
    // Legacy compatibility with older KateAI settings/UI. Internally maxToolCalls is used.
    int maxIterations = 20;
    int bashTimeoutMs = 60000;
    bool planMode = false;
    bool loadProjectInstructions = true;
    bool thinkingMode = true;
    QString extraSystemPrompt;
    QStringList extraDenyGlobs;
    int contextCompressionLevel = 1; // 0=full, 1=summary, 2=minimal, 3=ultra-minimal
    int maxGraphNodes = 50; // Maximum number of nodes to include in project graph
    int maxGraphEdges = 100; // Maximum number of edges to include in project graph
    bool compressProjectGraph = true; // Whether to compress project graph information
    bool includeFileContents = true; // Whether to include file content in project graph
    int maxFileContentLength = 500; // Maximum characters per file content preview
    bool compressEditorContext = true; // Whether to compress editor context
    int maxEditorContextLength = 200; // Maximum characters for editor context
    bool compressProjectInstructions = true; // Whether to compress project instructions
    int maxProjectInstructionsLength = 2048; // Maximum characters for project instructions
    bool compressSystemPrompt = true; // Whether to compress system prompt
    int maxSystemPromptLength = 1024; // Maximum characters for system prompt
    int messageSpeed = 2; // 0=slow, 1=medium, 2=fast (default fast)

    // --- Optimal-intelligence generation parameters -----------------------
    // These are sent to the model per request and tuned for coding tasks:
    // deterministic enough to be repeatable, creative enough to solve novel
    // problems, and bounded so the agent terminates instead of rambling.
    double temperature = 0.2;
    double topP = 0.95;
    int maxTokens = 0; // 0 = let the provider choose
    double frequencyPenalty = 0.0;
    double presencePenalty = 0.0;
    // Reasoning effort for models that expose it (e.g. xAI grok-reasoning).
    // Empty = do not send the field. "minimal" | "low" | "medium" | "high".
    QString reasoningEffort;
    int contextWindow = 0; // 0 = unknown; used for budgeting the history window
    bool selfCritique = true; // ask the model to check its own work before finishing
    bool parallelToolCalls = true; // let the model batch independent tool calls
    int toolCallTimeoutMs = 120000; // per-tool-call wall-clock budget
    int maxContextMessages = 0; // 0 = keep full history; else sliding window size
    bool compactOnFailure = true; // summarise history after a failed tool call
    int verbosity = 1; // 0= terse, 1= normal, 2= detailed narration

    // --- Enhanced Intelligence Parameters ---------------------------------
    // Structured thinking and planning
    bool structuredThinking = true;  // Require <thinking> block before response
    bool structuredPlanning = true;  // Require structured plan after thinking
    bool autoCollapseThinking = true; // Auto-collapse thinking once answer starts
    bool showPlanAsChecklist = true;  // Render plan as interactive checklist
    int maxThinkingTokens = 4096;    // Max tokens for thinking block
    int maxPlanSteps = 15;           // Max steps in structured plan
    
    // Context management for performance
    bool smartContextTruncation = true; // Intelligently truncate old context
    int contextWindowReserve = 8192;    // Reserve tokens for response
    bool compressOldMessages = true;    // Compress messages beyond window
    int compressionThreshold = 2048;    // Start compressing after this many chars
    
    // Agent behavior tuning
    bool requireVerification = true;    // Require verification after mutations
    int maxVerificationAttempts = 2;    // Max verification retries
    bool adaptiveTemperature = true;    // Adjust temperature based on task phase
    double explorationTemperature = 0.4; // Higher temp for exploration phase
    double exploitationTemperature = 0.1; // Lower temp for execution phase
    bool enablePlanUpdates = true;      // Allow plan updates during execution
    bool narrativeProgress = true;      // Natural language progress updates

    // --- Retry Configuration -----------------------------------------------
    // Automatic retry for transient API errors (rate limits, server errors, network issues)
    bool enableAutoRetry = true;
    // Maximum retry attempts (total attempts = 1 initial + retries)
    int maxRetryAttempts = 4;
    // Base delay for exponential backoff in seconds
    int baseRetryDelaySeconds = 5;
    // Maximum single retry delay cap in seconds (0 = no cap)
    int maxRetryDelaySeconds = 300;
    // Retry strategy: "exponential" or "fixed"
    QString retryStrategy = u"exponential"_s;
};

QString providerId(Provider provider);
QString providerLabel(Provider provider);
Provider providerFromId(const QString &id);
QString providerBaseUrl(Provider provider);
QString providerBaseUrl(const Settings &settings);
QStringList defaultModels(Provider provider);

QString permissionModeId(PermissionMode mode);
QString permissionModeLabel(PermissionMode mode);
PermissionMode permissionModeFromId(const QString &id);

QString sandboxProfileId(SandboxProfile profile);
QString sandboxProfileLabel(SandboxProfile profile);
SandboxProfile sandboxProfileFromId(const QString &id);

QString apiKeyFor(const Settings &settings);
QString modelFor(const Settings &settings);

QJsonArray toolDefinitions(bool readOnlyOnly = false);
QString defaultSystemPrompt(const QString &workspace);
QString compressText(const QString &text, int maxLength, bool enabled);
// Smart context compression - preserves important parts while reducing size
QString smartCompressContext(const QString &text, int maxLength, bool enabled);
// Compress a list of messages intelligently
QList<ChatMessage> compressMessageHistory(const QList<ChatMessage> &messages,
                                           int maxMessages,
                                           int maxTotalChars,
                                           bool enabled);

// Structured planning helpers ------------------------------------------------
QJsonArray parsePlanFromText(const QString &text);
QJsonArray mergePlanIntoAssistantMessage(const QJsonArray &existingPlan,
                                         const QString &assistantText);
QJsonArray markPlanStepCompleted(const QJsonArray &plan, const QString &stepId);
bool planIsComplete(const QJsonArray &plan);

} // namespace KateAi
