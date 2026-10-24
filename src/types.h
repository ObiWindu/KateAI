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
    Telegram,
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
    // The absolute path the tool actually writes to. "path" is what the model
    // asked for, which can be relative; the edit tracker has to use the
    // resolved path, otherwise it reads, restores and deletes a file next to
    // the process instead of the edited one.
    QString resolvedPath;
    // Pre-formatted unified diff for write_file, or the old/new strings for
    // edit_file. Rendered inline by the tool-call card in the chat transcript.
    QString describeDiff;
    ToolRisk risk = ToolRisk::Write;

    // True when the call lands its result in a file. ToolRunner::describe sets
    // it for the mutating tools, and AcpClient::permissionFromToolCall sets it
    // for ACP tool calls of kind edit/write/delete, so the edit tracker can
    // review ACP edits whose tool names Kate does not know.
    bool mutatesFiles = false;

    // Filled only for the `question` tool: what is being asked, the answers
    // offered as one-tap chips, and whether the user may type their own. The
    // agent loop stops the turn until an answer arrives.
    QString questionText;
    QStringList questionOptions;
    bool allowFreeform = true;
};

struct ToolResult {
    QString toolCallId;
    QString name;
    QString output;          // Formatted output for conversation history (with metadata)
    QString rawOutput;       // Raw output from tool execution (for UI display)
    bool ok = true;
    // True when the operation was stopped deliberately rather than failing.
    bool cancelled = false;
};

// One suggestion the agent wants the user to see, optionally anchored to a
// file and line so Accept can take the user straight to the code it is about.
// Reachable from the native suggest tool and from the ACP client method of
// the same name.
struct Suggestion {
    QString text;
    QString path;
    // 1-based line number; 0 means no anchor.
    int line = 0;
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
    // Installed ACP registry entries as compact JSON objects (id, name,
    // command, args, package, distribution). Kept in user settings so the
    // registry selection is available in every chat.
    QString acpInstalledAgents;
    // ACP agent models broadcasted as available by running agents.
    // Stores a JSON object mapping agent IDs to arrays of model names.
    QString acpAgentModels;
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
    // Lets the read tools (read_file, list_dir, grep, glob) reach files
    // outside the workspace. Off by default: those tools are approved without
    // a prompt, so a confined read is what keeps the agent in the project.
    bool allowReadsOutsideWorkspace = false;
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

    // --- Editor integration --------------------------------------------------
    // AI completions at the cursor, rendered inline and accepted with Tab.
    // Opt-in: off by default. See docs/ai-completion-plan.md.
    bool completionEnabled = false;
    // "manual" (the Alt+\ action) or "auto" (idle timer after typing).
    QString completionTrigger = QStringLiteral("manual");
    // "inline" (ghost note at the cursor) or "popup" (native completion list).
    QString completionRenderMode = QStringLiteral("inline");
    // A small fast model for completions; empty means "use the chat model".
    QString completionModel;
    // Separate budget for completions, enforced in CompletionEngine.
    int completionRequestsPerMinute = 20;
    // FIM-style context window around the cursor, in characters.
    int completionContextPrefixChars = 4000;
    int completionContextSuffixChars = 1500;
    // Idle time after the last keystroke before an auto trigger fires.
    int completionAutoTriggerDelayMs = 800;
    // Apply the agent's file edits to documents that are open in the editor
    // in the buffer, leaving them unsaved for review, instead of writing them
    // to disk at once. Off by default: while a change is unsaved, commands
    // and tools that read the file from disk (bash, tests) still see the
    // previous state.
    bool editInBuffer = false;
    // Show an in-editor banner on documents the agent edited, with actions to
    // review the change in the Kate AI panel or reject it. Only open documents
    // can carry a banner, so this does nothing for files Kate has not opened.
    bool editBannersEnabled = true;

    // --- Appearance ----------------------------------------------------------
    // "preset" paints the chat panel from a built-in palette (themePreset);
    // "editor" derives both the chat colours and the window chrome from the
    // Kate editor's current colour scheme, so the whole panel -- the KateAI
    // plugin window and the chat inside it -- follows the editor theme
    // (including code blocks).
    QString themeSource = QStringLiteral("preset");
    // Built-in palette id, used when themeSource is "preset".
    QString themePreset = QStringLiteral("kateai-dark");
    // JSON object mapping ChatTheme colour keys (panelBg, accent, ...) to hex
    // values, applied on top of the selected source. Empty means no overrides.
    QString themeOverrides;
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
// Directory where a binary-distributed ACP registry agent is unpacked.
QString acpBinaryInstallDir(const QString &agentId);
// Absolute path of the executable for an installed binary ACP agent, or an
// empty string when it is not installed. Registry entries record commands such
// as "./agy_acp_server.par", which are relative to the unpacked archive rather
// than to Kate's working directory.
QString acpInstalledBinaryCommand(const Settings &settings);
// True for the MiniMax Code ACP agent (`mcode`, registry id `minimax-code`).
// Its standalone CLI (`mcode acp`) is preferred over the npm distribution the
// ACP registry records, because the distribution's bundled native modules can
// fail to load on current Node runtimes and the process exits before the ACP
// handshake completes.
bool acpAgentIsMinimaxCode(const Settings &settings);
// True for Google Antigravity ACP agent (registry id `antigravity-acp`).
bool acpAgentIsAntigravity(const Settings &settings);
QString acpAgentIdMatching(const QString &command, const QString &args);
// Models last reported by this agent (session config or the legacy models
// field), plus the configured id when it is not already in that list.
// Agents that have not been queried yet contribute no names.
QStringList acpAgentAvailableModels(const Settings &settings, const QString &agentId = QString());

QString permissionModeId(PermissionMode mode);
QString permissionModeLabel(PermissionMode mode);
PermissionMode permissionModeFromId(const QString &id);

QString sandboxProfileId(SandboxProfile profile);
QString sandboxProfileLabel(SandboxProfile profile);
SandboxProfile sandboxProfileFromId(const QString &id);

QString apiKeyFor(const Settings &settings);
QString modelFor(const Settings &settings);
// True when the provider exposes a fill-in-the-middle completion endpoint
// (/completions with prompt markers), used by the editor completion engine.
bool providerSupportsFim(Provider provider);
// Models the picker should offer for a provider: the live catalogue, plus the
// configured model when it is missing from that list. Providers with no
// catalogue (ACP) still surface the configured model so they can be selected.
QStringList pickerModelsFor(const Settings &settings, Provider provider, const QStringList &catalog);

// Tool definitions advertised to the model. `access` restricts the set; an
// empty access advertises every built-in tool.
QJsonArray toolDefinitions(const ToolAccess &access);
QString defaultSystemPrompt(const QString &workspace);
QString compressText(const QString &text, int maxLength, bool enabled);
// Cuts a text to at most maxLength UTF-16 units without splitting a
// surrogate pair. A cut in the middle of an astral character leaves a lone
// surrogate behind, which is serialised as U+FFFD.
QString truncateSafe(const QString &text, int maxLength);
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
