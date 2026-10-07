/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

enum class Provider {
    Grok,
    OpenAI,
    OpenRouter,
    DeepSeek,
    OpenAICompatible,
    ClaudeCompatible,
    Kilo,
    Acp,
    // OpenCode Zen, the gateway at opencode.ai that curates models for coding
    // agents. OpenAI-compatible, with the catalogue served from /zen/v1/models.
    OpenCode,
};

enum class ApiFormat {
    OpenAICompatible,
    AnthropicCompatible,
    AcpNative,
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

// Which tools the model may call. MCP tools are discovered at runtime, so they
// are matched by name prefix rather than by exact name.
struct ToolAccess {
    QSet<QString> exact;
    QStringList prefixes;
    bool allowAll = false;

    // Every tool, current and future.
    static ToolAccess unrestricted()
    {
        ToolAccess access;
        access.allowAll = true;
        return access;
    }
    bool isEmpty() const
    {
        return !allowAll && exact.isEmpty() && prefixes.isEmpty();
    }
    void allow(const QString &toolName)
    {
        exact.insert(toolName);
    }
    void allowPrefix(const QString &prefix)
    {
        if (!prefixes.contains(prefix)) {
            prefixes.append(prefix);
        }
    }
    bool allows(const QString &toolName) const
    {
        if (allowAll) {
            return true;
        }
        if (exact.contains(toolName)) {
            return true;
        }
        for (const QString &prefix : prefixes) {
            if (toolName.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }
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
    // True when the operation was stopped deliberately rather than failing.
    bool cancelled = false;
};

struct Settings {
    Provider provider = Provider::Grok;
    QString grokApiKey;
    QString openaiApiKey;
    QString openrouterApiKey;
    QString deepseekApiKey;
    QString openaiCompatibleApiKey;
    QString claudeCompatibleApiKey;
    QString kiloApiKey;
    QString acpApiKey;
    QString grokModel;
    QString openaiModel;
    QString openrouterModel;
    QString deepseekModel;
    QString openaiCompatibleModel;
    QString claudeCompatibleModel;
    QString kiloModel;
    QString acpModel;
    QString opencodeModel;
    // Model names are no longer hard-coded: they are fetched from whichever
    // providers have a valid key. These stay empty until then.
    QString opencodeApiKey;
    QString opencodeUrl = QStringLiteral("https://opencode.ai/zen/v1");
    QString deepseekUrl = QStringLiteral("https://api.deepseek.com");
    QString openaiCompatibleUrl = QStringLiteral("http://localhost:11434/v1");
    QString claudeCompatibleUrl = QStringLiteral("https://api.anthropic.com/v1");
    QString acpUrl = QStringLiteral("http://localhost:8080");
    // Native ACP: spawn a stdio JSON-RPC agent. HTTP formats still use acpUrl.
    // acpAgentId selects a known command/args/env preset; Custom leaves them as-is.
    QString acpAgentId = QStringLiteral("grok-build");
    QString acpCommand = QStringLiteral("grok");
    QString acpArgs = QStringLiteral("agent stdio");
    QString acpApiKeyEnv = QStringLiteral("XAI_API_KEY");
    ApiFormat apiFormat = ApiFormat::AcpNative;
    PermissionMode permissionMode = PermissionMode::Ask;
    SandboxProfile sandbox = SandboxProfile::Workspace;
    // Agent budgets are intentionally separate: API model turns, tool calls, and provider rate.
    // 0 = unlimited. A positive value is a safety ceiling, not a required cap.
    int maxModelRequests = 0;
    int maxToolCalls = 0;
    int requestsPerMinute = 15;
    // Number of recent Kate AI chats (thinking blocks and tool cards) to keep
    // expanded in the transcript. Older cards collapse; file-edit diffs stay open.
    int maxExpandedToolCards = 10;
    // Legacy compatibility with older KateAI settings/UI. Internally maxToolCalls is used.
    int maxIterations = 20;
    int bashTimeoutMs = 60000;
    bool planMode = false;
    // Active agent mode: a built-in id ("code", "ask", "architect", "debug",
    // "orchestrator") or the slug of a custom mode from .kateai/modes/.
    QString agentMode = QStringLiteral("code");
    // Tools that never prompt, regardless of permission mode (Kilo Code's
    // auto-approve checkboxes). MCP tools can be auto-approved per server too.
    QStringList autoApproveTools;
    bool loadAgentRules = true;
    // User-wide rules applied to every workspace before the project rules.
    QString globalRules;
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
    int maxEditorContextLength = 4000; // Maximum characters for editor context
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
    int contextWindow = 0; // 0 = auto from a conservative default
    int keepRecentTokens = 0; // 0 = auto (half the remaining window)
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
    bool autoCollapseThinking = false; // Auto-collapse thinking once answer starts (default false: stay visible)
    bool showPlanAsChecklist = true;  // Render plan as interactive checklist
    int maxThinkingTokens = 4096;    // Max tokens for thinking block
    int maxPlanSteps = 15;           // Max steps in structured plan
    
    // Context management for performance
    bool smartContextTruncation = true;  // Keep a recent tail verbatim when compacting
    int contextWindowReserve = 8192;     // Reserve tokens for the model's reply
    bool compressOldMessages = true;     // Auto-compact older turns to fit the window
    int compressionThreshold = 2048;     // Soft hint for how much recent text to keep
    
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

    // --- Conversation History ----------------------------------------------
    // Maximum number of conversations to keep in history (0 = unlimited)
    int maxSavedConversations = 50;

    // --- MCP (Model Context Protocol) servers --------------------------------
    bool mcpEnabled = true;
    // Connect configured servers automatically when a workspace opens.
    bool mcpAutoConnect = true;
    int mcpTimeoutMs = 60000;

    // --- Web search -----------------------------------------------------------
    // Provider id: "duckduckgo" (no key), "tavily", "brave", "searxng",
    // or "disabled". DuckDuckGo is the default so the feature works with no
    // setup at all; the others are better or needed on an isolated network.
    QString webSearchProvider = QStringLiteral("duckduckgo");
    // API key for providers that need one (Tavily, Brave). Only ever used to
    // build a request header or body, never logged.
    QString webSearchApiKey;
    // Base URL for SearXNG, e.g. http://localhost:8888.
    QString webSearchEndpoint;
    // Results per search, clamped to 1..20.
    int webSearchMaxResults = 5;
    int webSearchTimeoutMs = 20000;

    // --- Checkpoints ---------------------------------------------------------
    // Snapshot the workspace into a shadow git repository before the agent
    // changes anything, so a turn can be rolled back.
    bool checkpointsEnabled = true;
    // How many snapshots to keep in the shadow repository.
    int checkpointRetention = 20;

    // --- Subtasks ------------------------------------------------------------
    // How deep new_task may nest before it is refused.
    int maxSubtaskDepth = 2;
    // How many sub-agents may run at the same time.
    int maxParallelSubtasks = 3;
    // User-defined agents as a JSON array; merged with the built-in roster.
    QString agentRoster;
    // Wall-clock budget for a single subtask.
    int subtaskTimeoutMs = 300000;
};

QString providerId(Provider provider);
QString providerLabel(Provider provider);
Provider providerFromId(const QString &id);
QString providerBaseUrl(Provider provider);
QString providerBaseUrl(const Settings &settings);
QStringList defaultModels(Provider provider);
// True when the provider can be listed: it has a key and speaks an API we can
// query a model catalogue on. Used to decide which providers to fetch from.
bool providerSupportsModelListing(Provider provider);
// True when Kate talks to an ACP agent over stdio JSON-RPC.
bool usesAcpNative(const Settings &settings);
// False for ACP native: agents typically authenticate through their own CLI login.
bool providerRequiresApiKey(const Settings &settings, Provider provider);
bool providerIsSelectable(const Settings &settings, Provider provider);

// Known stdio ACP agents. Command/args match the official ACP registry launch
// lines; users can still override them, or pick Custom and type anything.
struct AcpAgentPreset {
    QString id;
    QString label;
    QString command;
    QString args;
    QString apiKeyEnv;
};

QList<AcpAgentPreset> acpAgentPresets();
AcpAgentPreset acpAgentPreset(const QString &id);
QString acpAgentDisplayName(const Settings &settings);
QString acpEffectiveCommand(const Settings &settings);
QString acpEffectiveArgs(const Settings &settings);
QString acpEffectiveApiKeyEnv(const Settings &settings);
// True when the resolved launch command is Grok Build (`grok`). Only then do
// we inject `--model` / `--always-approve` and Grok `_meta`.
bool acpAgentIsGrok(const Settings &settings);
QString acpAgentIdMatching(const QString &command, const QString &args);

QString permissionModeId(PermissionMode mode);
QString permissionModeLabel(PermissionMode mode);
PermissionMode permissionModeFromId(const QString &id);

QString sandboxProfileId(SandboxProfile profile);
QString sandboxProfileLabel(SandboxProfile profile);
SandboxProfile sandboxProfileFromId(const QString &id);

QString apiKeyFor(const Settings &settings);
QString modelFor(const Settings &settings);
// Models the picker should offer for a provider: the live catalogue, plus the
// configured model when it is missing from that list. Providers with no
// catalogue (ACP) still surface the configured model so they can be selected.
QStringList pickerModelsFor(const Settings &settings, Provider provider, const QStringList &catalog);

// Tool definitions advertised to the model. `access` restricts the set; an
// empty access advertises every built-in tool.
QJsonArray toolDefinitions(const ToolAccess &access);
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

// API Format helpers --------------------------------------------------------
QString apiFormatId(ApiFormat format);
QString apiFormatLabel(ApiFormat format);
ApiFormat apiFormatFromId(const QString &id);

} // namespace KateAi
