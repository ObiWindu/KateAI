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

// ACP/native tool name normalization. Maps known agent tool names (e.g. fs/read_text_file,
// terminal/exec) to canonical KateAI tool names used for policy, UI, and tracking.
QString canonicalToolName(const QString &name);
QStringList toolNameAliases(const QString &canonicalName);
// Resolve a path the agent reported, relative to the workspace when it is not
// absolute. Returns an empty string for an empty input.
QString resolvePath(const QString &path, const QString &workspace);
// Normalise the raw input of an ACP tool call into a JSON object, whatever
// shape the agent used (object, array, JSON string, plain text).
QJsonObject normalizeRawInput(const QJsonValue &rawInput);

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
    // Deny-first entries. A denied name wins over every allow rule, including
    // allowAll, so an AgentSpec can subtract a single MCP tool (or a prefix of
    // one) from a mode that otherwise allows the whole MCP namespace.
    QSet<QString> deniedExact;
    QStringList deniedPrefixes;
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
    void deny(const QString &toolName)
    {
        deniedExact.insert(toolName);
    }
    void denyPrefix(const QString &prefix)
    {
        if (!deniedPrefixes.contains(prefix)) {
            deniedPrefixes.append(prefix);
        }
    }
    bool isDenied(const QString &toolName) const
    {
        if (deniedExact.isEmpty() && deniedPrefixes.isEmpty()) {
            return false;
        }
        const QString canonical = canonicalToolName(toolName);
        if (deniedExact.contains(toolName) || deniedExact.contains(canonical)) {
            return true;
        }
        for (const QString &alias : toolNameAliases(canonical)) {
            if (deniedExact.contains(alias)) {
                return true;
            }
        }
        for (const QString &prefix : deniedPrefixes) {
            if (toolName.startsWith(prefix) || canonical.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }
    bool allows(const QString &toolName) const
    {
        if (isDenied(toolName)) {
            return false;
        }
        if (allowAll) {
            return true;
        }
        // Canonicalize so ACP aliases (fs/read_text_file, terminal/exec, etc.)
        // are checked against the canonical tool names used in mode definitions.
        const QString canonical = canonicalToolName(toolName);
        if (exact.contains(toolName) || exact.contains(canonical)) {
            return true;
        }
        for (const QString &prefix : prefixes) {
            if (toolName.startsWith(prefix) || canonical.startsWith(prefix)) {
                return true;
            }
        }
        // Also check aliases of canonical names in exact set
        const QStringList aliases = toolNameAliases(canonical);
        for (const QString &alias : aliases) {
            if (exact.contains(alias)) {
                return true;
            }
        }
        return false;
    }
    // Convenience: check the canonical form of a name directly.
    bool allowsCanonical(const QString &canonicalName) const
    {
        return allows(canonicalName);
    }
};

// Which model produced an assistant message in a two-model turn. Empty for
// single-model messages and for every non-assistant role. Persisted so the
// transcript can keep its planner/executor/reviewer badges after a restart.
struct ChatMessage {
    enum class Role { System, User, Assistant, Tool };
    Role role = Role::User;
    QString authorRole;
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
    // Ambient context injected into a request only (editor state, git status).
    // It is never stored in the conversation or the session database.
    bool transient = false;
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
    // Provider-reported usage when the stream carries it. 0 means "not
    // reported by this chunk/provider". Used to calibrate local estimates.
    int inputTokens = 0;
    int outputTokens = 0;
    int cachedInputTokens = 0;
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
    // Additional diffs for multi-file ACP tool calls (path -> diff).
    QStringList additionalPaths;
    QStringList additionalDiffs;
    // The original (un-normalized) tool name as reported by the agent, kept
    // for display/debugging.
    QString originalToolName;
    ToolRisk risk = ToolRisk::Write;

    // True when the call lands its result in a file. ToolRunner::describe sets
    // it for the mutating tools, and AcpClient::permissionFromToolCall sets it
    // for ACP tool calls of kind edit/write/delete, so the edit tracker can
    // review ACP edits whose tool names Kate does not know.
    bool mutatesFiles = false;
    // True when an ACP agent invoked one of Kate's client methods rather than
    // reporting one of the external agent's own tools.
    bool acpClientMethod = false;

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

// One hunk of a unified diff. `lines` keeps the leading marker character
// (' ', '+', '-'); the '\\ No newline at end of file' line is kept as-is.
struct PatchHunk {
    int oldStart = 0;
    int oldCount = 0;
    int newStart = 0;
    int newCount = 0;
    QStringList lines;
};

// One file section of a unified diff, as produced by apply_patch-style tools
// (Codex, aider, OpenCode, ...). Paths keep the a/ b/ prefixes stripped.
struct PatchFile {
    QString path; // target path (from the +++ line)
    QString oldPath; // from the --- line
    bool isNew = false; // /dev/null on the --- side
    bool isDelete = false; // /dev/null on the +++ side
    QList<PatchHunk> hunks;
};

// Parses a unified diff into per-file sections. Returns an empty list when the
// text is not a unified diff. Shared by the native apply_patch tool and the
// ACP ingest path, which turns agent patches into per-file card diffs.
QList<PatchFile> parseUnifiedPatch(const QString &patch);
// Applies hunks to `content` (LF-separated). Fails with an error when a hunk's
// context does not match at its recorded position; applies nothing on failure.
QString applyPatchToContent(const QString &content, const QList<PatchHunk> &hunks, QString *error);

// One resolved model in a two-model orchestration pair. The route is stored in
// settings instead of being derived from Settings::provider so the planner and
// executor can live on different API providers.
struct ModelRoute {
    Provider provider = Provider::Grok;
    QString model;
    QString effort;

    bool isValid() const
    {
        return provider != Provider::Acp && provider != Provider::Telegram
            && !model.trimmed().isEmpty();
    }
    QString key() const;
};

struct OrchestrationSettings {
    bool enabled = false;
    ModelRoute planner;
    ModelRoute executor;
    // Bounds for one user turn. Replans and reviews are deliberately separate:
    // a planner that keeps changing its mind must not consume the executor's
    // retry budget, and vice versa.
    int maxReplans = 3;
    int maxPlannerSteps = 12;
    int maxReviewRounds = 2;
    // "auto", "always", "off". Auto reviews a turn that changed files or has
    // at least three plan steps.
    QString reviewMode = QStringLiteral("auto");
    bool showPlannerActivity = true;
};

struct Settings {
    Provider provider = Provider::Grok;
    OrchestrationSettings orchestration;
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
    // Effort/thought level forced onto the live ACP agent session through
    // session/set_config_option. Empty means Auto: the agent keeps its own
    // default and Kate never pushes a value.
    QString acpEffort;
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
    // Slash commands last advertised by each ACP agent. A JSON object mapping
    // agent IDs to the advertised command arrays. Commands only arrive with a
    // live session, so the cache lets a freshly opened window show the list
    // before the agent process has started again.
    QString acpAgentCommands;
    ApiFormat apiFormat = ApiFormat::AcpNative;
    PermissionMode permissionMode = PermissionMode::Ask;
    // Accept ACP permission requests by default. Kate-hosted client methods
    // also override mode access and ordinary policy rules; immutable hard
    // denies and the handler's sandbox checks still apply.
    bool acceptAllAcpTools = true;
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
    int maxGraphNodes = 80; // Maximum number of file nodes to include in the graph prompt
    int maxGraphEdges = 160; // Maximum number of dependency edges to include
    bool compressProjectGraph = true; // Whether to include the graph summary in the prompt
    bool compressEditorContext = true; // Whether to compress editor context
    int maxEditorContextLength = 8000; // Maximum characters for editor context
    bool compressProjectInstructions = true; // Whether to compress project instructions
    int maxProjectInstructionsLength = 8192; // Maximum characters for project instructions
    bool compressSystemPrompt = true; // Whether to compress system prompt
    int maxSystemPromptLength = 4096; // Maximum characters for system prompt

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
    int contextWindowReserve = 8192;     // Reserve tokens for the model's reply
    bool compressOldMessages = true;     // Auto-compact older turns to fit the window
    
    // Agent behavior tuning
    bool requireVerification = true;    // Require verification after mutations
    int maxVerificationAttempts = 2;    // Max verification retries

    // --- Telemetry and cost ---------------------------------------------------
    // Opt-in local spans plus an optional OTLP/HTTP collector. The harness
    // event log is always local; this only controls export of derived spans.
    bool telemetryEnabled = false;
    QString telemetryEndpoint;          // e.g. http://127.0.0.1:4318 (empty = off)
    bool telemetryContentCapture = false; // never on by default; hashes/sizes only
    // Soft per-conversation budget. 0 = no warning. The turn is never failed
    // on cost; the user gets a notice when the budget is crossed.
    double costBudgetUsd = 0.0;

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
    // Opt-in: off by default. See the "Editor integration" section of README.md.
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

    // --- Voice prompting -----------------------------------------------------
    // Backend id: "auto" (OpenAI key -> custom endpoint -> local Whisper CLI),
    // "openai", "openai_compatible", or "local_cli".
    QString voiceProvider = QStringLiteral("auto");
    // Optional API key dedicated to speech-to-text (stored in CredentialStore).
    // Empty falls back to the provider's chat API key when applicable.
    QString voiceApiKey;
    // Custom Whisper-compatible base URL or full /audio/transcriptions URL.
    QString voiceEndpoint;
    // Transcription model sent to HTTP speech-to-text endpoints.
    QString voiceModel = QStringLiteral("whisper-1");
    // Optional ISO-639-1 language hint (e.g. "en", "fr"); empty = auto-detect.
    QString voiceLanguage;
    // Local CLI transcription command template (use {file} for the WAV path).
    QString voiceLocalCommand;
    // Optional microphone device name override; empty = system default mic.
    QString voiceInputDevice;
    // Run noise removal before transcription.
    bool voiceNoiseRemoval = true;
    // Automatically submit transcribed text to chat when idle.
    bool voiceAutoSend = true;
};

QString providerId(Provider provider);
QString providerLabel(Provider provider);
Provider providerFromId(const QString &id);
QString providerBaseUrl(Provider provider);
QString providerBaseUrl(const Settings &settings);
// Joins a provider base URL and an API path without doubling an existing
// "/v1" segment. The Anthropic base already ends in /v1, while the endpoints
// are written as /v1/messages, which used to produce /v1/v1/messages.
QString joinApiEndpoint(const QString &baseUrl, const QString &endpoint);
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
// Everything selectable in the ACP Agent combo: the built-in presets followed by
// agents installed from the ACP registry, then Custom last. An installed agent
// whose id also exists as a preset keeps the preset entry.
QList<AcpAgentPreset> acpAgentChoices(const Settings &settings);
// Launch line for an agent: the registry entry when it is installed, otherwise
// the matching preset. Agents known to neither return an empty command/args so
// the user's typed values are left alone.
AcpAgentPreset acpAgentLaunchLine(const Settings &settings, const QString &agentId);
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
// True when the agent's registry entry is a binary distribution. Such a
// command is staged by the installer, so an unresolved relative path means the
// install is missing rather than that the user meant a workspace file.
bool acpAgentHasBinaryInstall(const Settings &settings);
// True for the MiniMax Code ACP agent (`mcode`, registry id `minimax-code`).
// Its standalone CLI (`mcode acp`) is preferred over the npm distribution the
// ACP registry records, because the distribution's bundled native modules can
// fail to load on current Node runtimes and the process exits before the ACP
// handshake completes.
bool acpAgentIsMinimaxCode(const Settings &settings);
// True for Google Antigravity ACP agent (`agy acp` CLI or legacy
// `agy_acp_server` binary, registry id `antigravity-acp`).
bool acpAgentIsAntigravity(const Settings &settings);
// True only for the legacy standalone gflags/C++ `agy_acp_server` binary
// (`.par` archive), which needs sanitizer-disabling gflags and the 4KB page
// shim on >4KB hosts. The modern Go `agy` CLI (`agy acp`) rejects those flags
// and runs natively without LD_PRELOAD.
bool acpAgentIsLegacyAntigravityServer(const Settings &settings);
// Launch-line quirks of one ACP agent, keyed by registry id so newly installed
// registry agents get the same treatment as the built-in presets. Zed keeps the
// equivalent knowledge in crates/agent_servers/<name>.rs.
struct AcpAgentQuirks {
    // Append an "acp" subcommand when the recorded args have none (opencode and
    // friends start their TUI instead of the ACP server otherwise).
    bool needsAcpSubcommand = false;
    // Drop npm-only tokens ("--yes", "@scope/pkg") from recorded args.
    bool stripNpmTokens = false;
    // The agent accepts "--model <id>".
    bool modelFlag = false;
    // Always appended (e.g. antigravity's sanitizer-disabling flags).
    QStringList extraArgs;
    // Removed from the recorded args when present.
    QStringList stripArgs;
    // Present clientInfo as Zed at initialize (agents that gate models by client).
    bool spoofZedClientInfo = false;
};
AcpAgentQuirks acpAgentQuirks(const QString &agentId, const QString &command);
// True for the Mistral Vibe ACP agent (registry id `mistral-vibe`, binary
// `vibe-acp`). It persists its selected model in its own global state and
// reports it as current on every new session, so the model configured in
// Kate is pinned onto the session instead of following the agent.
bool acpAgentIsMistralVibe(const Settings &settings);
QString acpAgentIdMatching(const QString &command, const QString &args);
// Models last reported by this agent (session config or the legacy models
// field), plus the configured id when it is not already in that list.
// Agents that have not been queried yet contribute no names.
QStringList acpAgentAvailableModels(const Settings &settings, const QString &agentId = QString());

// Tolerant readers for the two UI-only tools. Agents commonly spell the same
// payload differently (`question`/`prompt`/`message`, `suggestion`/`text`/
// `advice`, nested `questions[]`, option objects instead of strings). Card
// construction and tool validation share these so a synonym cannot leave a
// question or suggestion card blank.
QString firstStringToolArgument(const QJsonObject &args, const QStringList &keys);
QString questionTextFromToolArguments(const QJsonObject &args);
QStringList questionOptionsFromToolArguments(const QJsonObject &args);
bool questionAllowFreeformFromToolArguments(const QJsonObject &args, bool fallback = true);
QString suggestionTextFromToolArguments(const QJsonObject &args);

QString permissionModeId(PermissionMode mode);QString permissionModeLabel(PermissionMode mode);
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
