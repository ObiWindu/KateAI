/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "settings.h"
#include "websearch.h"

#include <KConfigGroup>
#include <QFile>
#include <KSharedConfig>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

static KConfigGroup group()
{
    return KConfigGroup(KSharedConfig::openConfig(), u"KateAI"_s);
}

Settings SettingsStore::load()
{
    const KConfigGroup g = group();
    Settings s;
    s.provider = providerFromId(g.readEntry(u"Provider"_s, providerId(Provider::Grok)));
    s.grokApiKey = g.readEntry(u"GrokApiKey"_s, QString());
    s.openaiApiKey = g.readEntry(u"OpenAIApiKey"_s, QString());
    s.openrouterApiKey = g.readEntry(u"OpenRouterApiKey"_s, QString());
    s.deepseekApiKey = g.readEntry(u"DeepSeekApiKey"_s, QString());
    s.openaiCompatibleApiKey = g.readEntry(u"OpenAICompatibleApiKey"_s, QString());
    s.claudeCompatibleApiKey = g.readEntry(u"ClaudeCompatibleApiKey"_s, QString());
    s.kiloApiKey = g.readEntry(u"KiloApiKey"_s, QString());
    s.acpApiKey = g.readEntry(u"AcpApiKey"_s, QString());
    s.acpInstalledAgents = g.readEntry(u"AcpInstalledAgents"_s, QString());
    s.acpAgentModels = g.readEntry(u"AcpAgentModels"_s, QString());
    // Models are read from the provider, not hard-coded here; an empty entry
    // simply means "not chosen yet" and the fetched catalogue fills it in.
    s.grokModel = g.readEntry(u"GrokModel"_s, QString());
    s.openaiModel = g.readEntry(u"OpenAIModel"_s, QString());
    s.openrouterModel = g.readEntry(u"OpenRouterModel"_s, QString());
    s.deepseekModel = g.readEntry(u"DeepSeekModel"_s, QString());
    s.openaiCompatibleModel = g.readEntry(u"OpenAICompatibleModel"_s, QString());
    s.claudeCompatibleModel = g.readEntry(u"ClaudeCompatibleModel"_s, QString());
    s.kiloModel = g.readEntry(u"KiloModel"_s, QString());
    s.acpModel = g.readEntry(u"AcpModel"_s, QString());
    s.opencodeApiKey = g.readEntry(u"OpenCodeApiKey"_s, QString());
    s.opencodeModel = g.readEntry(u"OpenCodeModel"_s, QString());
    s.opencodeUrl = g.readEntry(u"OpenCodeUrl"_s, u"https://opencode.ai/zen/v1"_s);
    s.deepseekUrl = g.readEntry(u"DeepSeekUrl"_s, u"https://api.deepseek.com"_s);
    s.openaiCompatibleUrl = g.readEntry(u"OpenAICompatibleUrl"_s, u"http://localhost:11434/v1"_s);
    s.claudeCompatibleUrl = g.readEntry(u"ClaudeCompatibleUrl"_s, u"https://api.anthropic.com/v1"_s);
    s.acpUrl = g.readEntry(u"AcpUrl"_s, u"http://localhost:8080"_s);
    s.acpCommand = g.readEntry(u"AcpCommand"_s, u"grok"_s);
    s.acpArgs = g.readEntry(u"AcpArgs"_s, u"agent stdio"_s);
    s.acpAgentId = g.readEntry(u"AcpAgentId"_s, QString());
    if (s.acpAgentId.trimmed().isEmpty()) {
        s.acpAgentId = acpAgentIdMatching(s.acpCommand, s.acpArgs);
    }
    s.acpApiKeyEnv = g.readEntry(u"AcpApiKeyEnv"_s, QString());
    if (s.acpApiKeyEnv.trimmed().isEmpty()) {
        s.acpApiKeyEnv = acpAgentPreset(s.acpAgentId).apiKeyEnv;
    }
    s.apiFormat = apiFormatFromId(g.readEntry(u"ApiFormat"_s, apiFormatId(ApiFormat::AcpNative)));
    s.permissionMode = permissionModeFromId(g.readEntry(u"PermissionMode"_s, permissionModeId(PermissionMode::Ask)));
    s.sandbox = sandboxProfileFromId(g.readEntry(u"Sandbox"_s, sandboxProfileId(SandboxProfile::Workspace)));

    // Read new fields first, then fall back to legacy MaxIterations for maxToolCalls
    s.maxModelRequests = g.readEntry(u"MaxModelRequests"_s, 0);
    s.maxToolCalls = g.readEntry(u"MaxToolCalls"_s, 0);
    const int legacyMaxIterations = g.readEntry(u"MaxIterations"_s, 0);
    if (s.maxToolCalls == 0 && legacyMaxIterations > 0) {
        s.maxToolCalls = legacyMaxIterations;
    }
    s.requestsPerMinute = g.readEntry(u"RequestsPerMinute"_s, 15);
    // Keep legacy field in sync for older callers.
    s.maxIterations = s.maxToolCalls;
    s.bashTimeoutMs = g.readEntry(u"BashTimeoutMs"_s, 60000);
    s.planMode = g.readEntry(u"PlanMode"_s, false);
        s.agentMode = g.readEntry(u"AgentMode"_s, QStringLiteral("code"));
        s.autoApproveTools = g.readEntry(u"AutoApproveTools"_s, QStringList());
        s.loadAgentRules = g.readEntry(u"LoadAgentRules"_s, true);
        s.globalRules = g.readEntry(u"GlobalRules"_s, QString());
    s.loadProjectInstructions = g.readEntry(u"LoadProjectInstructions"_s, true);
    s.thinkingMode = g.readEntry(u"ThinkingMode"_s, true);
    s.extraSystemPrompt = g.readEntry(u"ExtraSystemPrompt"_s, QString());
    s.extraDenyGlobs = g.readEntry(u"ExtraDenyGlobs"_s, QStringList());
    s.allowReadsOutsideWorkspace = g.readEntry(u"AllowReadsOutsideWorkspace"_s, false);
    s.contextCompressionLevel = g.readEntry(u"ContextCompressionLevel"_s, 1);
    s.maxGraphNodes = g.readEntry(u"MaxGraphNodes"_s, 50);
    s.maxGraphEdges = g.readEntry(u"MaxGraphEdges"_s, 100);
    s.compressProjectGraph = g.readEntry(u"CompressProjectGraph"_s, true);
    s.includeFileContents = g.readEntry(u"IncludeFileContents"_s, true);
    s.maxFileContentLength = g.readEntry(u"MaxFileContentLength"_s, 500);
    s.compressEditorContext = g.readEntry(u"CompressEditorContext"_s, true);
    s.maxEditorContextLength = g.readEntry(u"MaxEditorContextLength"_s, 4000);
    s.compressProjectInstructions = g.readEntry(u"CompressProjectInstructions"_s, true);
    s.maxProjectInstructionsLength = g.readEntry(u"MaxProjectInstructionsLength"_s, 2048);
    s.compressSystemPrompt = g.readEntry(u"CompressSystemPrompt"_s, true);
    s.maxSystemPromptLength = g.readEntry(u"MaxSystemPromptLength"_s, 1024);
    // Optimal Intelligence Parameters
    s.temperature = g.readEntry(u"Temperature"_s, 0.2);
    s.topP = g.readEntry(u"TopP"_s, 0.95);
    s.maxTokens = g.readEntry(u"MaxTokens"_s, 0);
    s.reasoningEffort = g.readEntry(u"ReasoningEffort"_s, QString());
    s.selfCritique = g.readEntry(u"SelfCritique"_s, true);
    s.parallelToolCalls = g.readEntry(u"ParallelToolCalls"_s, true);
    s.verbosity = g.readEntry(u"Verbosity"_s, 1);
    s.autoCollapseThinking = g.readEntry(u"AutoCollapseThinking"_s, false);
    s.maxSavedConversations = g.readEntry(u"MaxSavedConversations"_s, 50);
    s.maxExpandedToolCards = g.readEntry(u"MaxExpandedToolCards"_s, 10);
    // MCP
    s.mcpEnabled = g.readEntry(u"McpEnabled"_s, true);
    s.mcpAutoConnect = g.readEntry(u"McpAutoConnect"_s, true);
    s.mcpTimeoutMs = g.readEntry(u"McpTimeoutMs"_s, 60000);
        // Web search
        s.webSearchProvider = g.readEntry(u"WebSearchProvider"_s, QStringLiteral("duckduckgo"));
    // An unknown value would silently disable the web search, because the
    // provider falls back to "disabled" for it.
    if (WebSearch::providerFromId(s.webSearchProvider) == WebSearch::Provider::Disabled) {
        s.webSearchProvider = QStringLiteral("duckduckgo");
    }
        s.webSearchApiKey = g.readEntry(u"WebSearchApiKey"_s);
        s.webSearchEndpoint = g.readEntry(u"WebSearchEndpoint"_s);
        s.webSearchMaxResults = g.readEntry(u"WebSearchMaxResults"_s, 5);
        s.webSearchTimeoutMs = g.readEntry(u"WebSearchTimeoutMs"_s, 20000);
    // Checkpoints
    s.checkpointsEnabled = g.readEntry(u"CheckpointsEnabled"_s, true);
    s.checkpointRetention = g.readEntry(u"CheckpointRetention"_s, 20);
    // Subtasks
    s.maxSubtaskDepth = g.readEntry(u"MaxSubtaskDepth"_s, 2);
    s.maxParallelSubtasks = g.readEntry(u"MaxParallelSubtasks"_s, 3);
    s.agentRoster = g.readEntry(u"AgentRoster"_s, QString());
    s.subtaskTimeoutMs = g.readEntry(u"SubtaskTimeoutMs"_s, 300000);
    // Appearance
    s.themeSource = g.readEntry(u"ThemeSource"_s, QStringLiteral("preset"));
    if (s.themeSource != u"editor"_s) {
        s.themeSource = u"preset"_s;
    }
    s.themePreset = g.readEntry(u"ThemePreset"_s, QStringLiteral("kateai-dark"));
    s.themeOverrides = g.readEntry(u"ThemeOverrides"_s, QString());
    // Behavioural toggles and budgets. These used to be dropped by save(), so
    // any value the UI wrote reverted to the struct default on the next start.
    s.toolCallTimeoutMs = g.readEntry(u"ToolCallTimeoutMs"_s, 120000);
    s.maxContextMessages = g.readEntry(u"MaxContextMessages"_s, 0);
    s.compactOnFailure = g.readEntry(u"CompactOnFailure"_s, true);
    s.structuredThinking = g.readEntry(u"StructuredThinking"_s, true);
    s.structuredPlanning = g.readEntry(u"StructuredPlanning"_s, true);
    s.showPlanAsChecklist = g.readEntry(u"ShowPlanAsChecklist"_s, true);
    s.maxThinkingTokens = g.readEntry(u"MaxThinkingTokens"_s, 4096);
    s.maxPlanSteps = g.readEntry(u"MaxPlanSteps"_s, 15);
    s.enablePlanUpdates = g.readEntry(u"EnablePlanUpdates"_s, true);
    s.narrativeProgress = g.readEntry(u"NarrativeProgress"_s, true);
    s.messageSpeed = g.readEntry(u"MessageSpeed"_s, 2);
    s.frequencyPenalty = g.readEntry(u"FrequencyPenalty"_s, 0.0);
    s.presencePenalty = g.readEntry(u"PresencePenalty"_s, 0.0);
    s.requireVerification = g.readEntry(u"RequireVerification"_s, true);
    s.maxVerificationAttempts = g.readEntry(u"MaxVerificationAttempts"_s, 2);
    s.adaptiveTemperature = g.readEntry(u"AdaptiveTemperature"_s, true);
    s.explorationTemperature = g.readEntry(u"ExplorationTemperature"_s, 0.4);
    s.exploitationTemperature = g.readEntry(u"ExploitationTemperature"_s, 0.1);
    s.enableAutoRetry = g.readEntry(u"EnableAutoRetry"_s, true);
    s.maxRetryAttempts = g.readEntry(u"MaxRetryAttempts"_s, 4);
    s.baseRetryDelaySeconds = g.readEntry(u"BaseRetryDelaySeconds"_s, 5);
    s.maxRetryDelaySeconds = g.readEntry(u"MaxRetryDelaySeconds"_s, 300);
    s.retryStrategy = g.readEntry(u"RetryStrategy"_s, u"exponential"_s);
    // Validate contextCompressionLevel (0=full, 1=summary, 2=minimal, 3=ultra-minimal)
    s.contextCompressionLevel = qBound(0, s.contextCompressionLevel, 3);
    if (s.maxModelRequests < 0) {
        s.maxModelRequests = 0;
    }
    if (s.maxToolCalls < 0) {
        s.maxToolCalls = 0;
    }
    if (s.requestsPerMinute < 1) {
        s.requestsPerMinute = 1;
    }
    if (s.requestsPerMinute > 60) {
        s.requestsPerMinute = 60;
    }
    // Keep the legacy field coherent for older callers.
    s.maxIterations = s.maxToolCalls;
    if (s.bashTimeoutMs < 1000) {
        s.bashTimeoutMs = 1000;
    }
    s.checkpointRetention = qBound(2, s.checkpointRetention, 500);
    s.maxSubtaskDepth = qBound(0, s.maxSubtaskDepth, 5);
    s.maxParallelSubtasks = qBound(1, s.maxParallelSubtasks, 12);
    s.subtaskTimeoutMs = qBound(10000, s.subtaskTimeoutMs, 30 * 60 * 1000);
    s.mcpTimeoutMs = qBound(1000, s.mcpTimeoutMs, 30 * 60 * 1000);
    s.webSearchMaxResults = qBound(1, s.webSearchMaxResults, 20);
    s.webSearchTimeoutMs = qBound(1000, s.webSearchTimeoutMs, 120000);
    // The fields above are bounded, and the behavioural ones must be too: a
    // hand-edited or migrated config must not be able to store a zero budget, a
    // plan with no steps, or a temperature no provider accepts.
    s.toolCallTimeoutMs = qBound(1000, s.toolCallTimeoutMs, 30 * 60 * 1000);
    s.maxContextMessages = qMax(0, s.maxContextMessages);
    s.maxThinkingTokens = qBound(0, s.maxThinkingTokens, 200000);
    s.maxPlanSteps = qBound(1, s.maxPlanSteps, 50);
    s.maxVerificationAttempts = qBound(0, s.maxVerificationAttempts, 10);
    s.messageSpeed = qBound(0, s.messageSpeed, 5);
    s.frequencyPenalty = qBound(-2.0, s.frequencyPenalty, 2.0);
    s.presencePenalty = qBound(-2.0, s.presencePenalty, 2.0);
    // Generation temperature, not the adaptive pair below.
    if (s.temperature < 0.0) {
        s.temperature = 0.0;
    }
    s.explorationTemperature = qBound(0.0, s.explorationTemperature, 2.0);
    s.exploitationTemperature = qBound(0.0, s.exploitationTemperature, 2.0);
    s.maxRetryAttempts = qBound(1, s.maxRetryAttempts, 10);
    s.baseRetryDelaySeconds = qBound(1, s.baseRetryDelaySeconds, 60);
    // The length budgets need a lower bound: a negative value would empty the
    // system prompt or the editor context without saying so.
    s.maxFileContentLength = qBound(1, s.maxFileContentLength, 1000000);
    s.maxEditorContextLength = qBound(1, s.maxEditorContextLength, 1000000);
    s.maxProjectInstructionsLength = qBound(1, s.maxProjectInstructionsLength, 1000000);
    s.maxSystemPromptLength = qBound(1, s.maxSystemPromptLength, 1000000);
    // maxThinkingTokens and maxPlanSteps are already bounded earlier; a
    // second pass used to override those bounds (a deliberate 0 became 1,
    // and the 200k thinking cap became 1M), so each field is bounded once.
    // 0 means "no limit" for the two budgets, so only the positive values
    // are bounded.
    if (s.maxToolCalls > 0) {
        s.maxToolCalls = qBound(1, s.maxToolCalls, 1000);
    }
    if (s.maxModelRequests > 0) {
        s.maxModelRequests = qBound(1, s.maxModelRequests, 1000);
    }
    // The sampling parameters have an upper bound: a temperature above 2 is
    // not a temperature.
    s.temperature = qBound(0.0, s.temperature, 2.0);
    s.topP = qBound(0.0, s.topP, 1.0);
    // 0 is the documented "no cap" value the retry maths keys off, so only the
    // positive entries are bounded.
    if (s.maxRetryDelaySeconds != 0) {
        s.maxRetryDelaySeconds = qBound(1, s.maxRetryDelaySeconds, 3600);
    }
    s.contextWindow = g.readEntry(u"ContextWindow"_s, 0);
    s.keepRecentTokens = g.readEntry(u"KeepRecentTokens"_s, 0);
    s.contextWindowReserve = g.readEntry(u"ContextWindowReserve"_s, 8192);
    s.compressOldMessages = g.readEntry(u"CompressOldMessages"_s, true);
    s.smartContextTruncation = g.readEntry(u"SmartContextTruncation"_s, true);
    s.compressionThreshold = g.readEntry(u"CompressionThreshold"_s, 2048);
    if (s.contextWindow < 0) {
        s.contextWindow = 0;
    }
    if (s.keepRecentTokens < 0) {
        s.keepRecentTokens = 0;
    }
    if (s.contextWindowReserve < 0) {
        s.contextWindowReserve = 0;
    }
    return s;
}

void SettingsStore::save(const Settings &settings)
{
    KConfigGroup g = group();
    g.writeEntry(u"Provider"_s, providerId(settings.provider));
    g.writeEntry(u"GrokApiKey"_s, settings.grokApiKey);
    g.writeEntry(u"OpenAIApiKey"_s, settings.openaiApiKey);
    g.writeEntry(u"OpenRouterApiKey"_s, settings.openrouterApiKey);
    g.writeEntry(u"DeepSeekApiKey"_s, settings.deepseekApiKey);
    g.writeEntry(u"OpenAICompatibleApiKey"_s, settings.openaiCompatibleApiKey);
    g.writeEntry(u"ClaudeCompatibleApiKey"_s, settings.claudeCompatibleApiKey);
    g.writeEntry(u"KiloApiKey"_s, settings.kiloApiKey);
    g.writeEntry(u"AcpApiKey"_s, settings.acpApiKey);
    g.writeEntry(u"AcpInstalledAgents"_s, settings.acpInstalledAgents);
    g.writeEntry(u"AcpAgentModels"_s, settings.acpAgentModels);
    g.writeEntry(u"OpenCodeApiKey"_s, settings.opencodeApiKey);
    g.writeEntry(u"OpenCodeModel"_s, settings.opencodeModel);
    g.writeEntry(u"OpenCodeUrl"_s, settings.opencodeUrl);
    g.writeEntry(u"GrokModel"_s, settings.grokModel);
    g.writeEntry(u"OpenAIModel"_s, settings.openaiModel);
    g.writeEntry(u"OpenRouterModel"_s, settings.openrouterModel);
    g.writeEntry(u"DeepSeekModel"_s, settings.deepseekModel);
    g.writeEntry(u"OpenAICompatibleModel"_s, settings.openaiCompatibleModel);
    g.writeEntry(u"ClaudeCompatibleModel"_s, settings.claudeCompatibleModel);
    g.writeEntry(u"KiloModel"_s, settings.kiloModel);
    g.writeEntry(u"AcpModel"_s, settings.acpModel);
    g.writeEntry(u"DeepSeekUrl"_s, settings.deepseekUrl);
    g.writeEntry(u"OpenAICompatibleUrl"_s, settings.openaiCompatibleUrl);
    g.writeEntry(u"ClaudeCompatibleUrl"_s, settings.claudeCompatibleUrl);
    g.writeEntry(u"AcpUrl"_s, settings.acpUrl);
    g.writeEntry(u"AcpCommand"_s, settings.acpCommand);
    g.writeEntry(u"AcpArgs"_s, settings.acpArgs);
    g.writeEntry(u"AcpAgentId"_s, settings.acpAgentId);
    g.writeEntry(u"AcpApiKeyEnv"_s, settings.acpApiKeyEnv);
    g.writeEntry(u"ApiFormat"_s, apiFormatId(settings.apiFormat));
    g.writeEntry(u"PermissionMode"_s, permissionModeId(settings.permissionMode));
    g.writeEntry(u"Sandbox"_s, sandboxProfileId(settings.sandbox));
    g.writeEntry(u"MaxModelRequests"_s, settings.maxModelRequests);
    g.writeEntry(u"MaxToolCalls"_s, settings.maxToolCalls);
    g.writeEntry(u"RequestsPerMinute"_s, settings.requestsPerMinute);
    // Preserve the old key for existing versions/UI.
    g.writeEntry(u"MaxIterations"_s, settings.maxToolCalls);
    g.writeEntry(u"BashTimeoutMs"_s, settings.bashTimeoutMs);
    g.writeEntry(u"PlanMode"_s, settings.planMode);
        g.writeEntry(u"AgentMode"_s, settings.agentMode);
        g.writeEntry(u"AutoApproveTools"_s, settings.autoApproveTools);
        g.writeEntry(u"LoadAgentRules"_s, settings.loadAgentRules);
        g.writeEntry(u"GlobalRules"_s, settings.globalRules);
    g.writeEntry(u"LoadProjectInstructions"_s, settings.loadProjectInstructions);
    g.writeEntry(u"ThinkingMode"_s, settings.thinkingMode);
    g.writeEntry(u"ExtraSystemPrompt"_s, settings.extraSystemPrompt);
    g.writeEntry(u"ExtraDenyGlobs"_s, settings.extraDenyGlobs);
    g.writeEntry(u"AllowReadsOutsideWorkspace"_s, settings.allowReadsOutsideWorkspace);
    
    // Save context compression settings
    g.writeEntry(u"ContextCompressionLevel"_s, settings.contextCompressionLevel);
    g.writeEntry(u"MaxGraphNodes"_s, settings.maxGraphNodes);
    g.writeEntry(u"MaxGraphEdges"_s, settings.maxGraphEdges);
    g.writeEntry(u"CompressProjectGraph"_s, settings.compressProjectGraph);
    g.writeEntry(u"IncludeFileContents"_s, settings.includeFileContents);
    g.writeEntry(u"MaxFileContentLength"_s, settings.maxFileContentLength);
    g.writeEntry(u"CompressEditorContext"_s, settings.compressEditorContext);
    g.writeEntry(u"MaxEditorContextLength"_s, settings.maxEditorContextLength);
    g.writeEntry(u"CompressProjectInstructions"_s, settings.compressProjectInstructions);
    g.writeEntry(u"MaxProjectInstructionsLength"_s, settings.maxProjectInstructionsLength);
    g.writeEntry(u"CompressSystemPrompt"_s, settings.compressSystemPrompt);
    g.writeEntry(u"MaxSystemPromptLength"_s, settings.maxSystemPromptLength);
    // Save optimal intelligence parameters
    g.writeEntry(u"Temperature"_s, settings.temperature);
    g.writeEntry(u"TopP"_s, settings.topP);
    g.writeEntry(u"MaxTokens"_s, settings.maxTokens);
    g.writeEntry(u"ReasoningEffort"_s, settings.reasoningEffort);
    g.writeEntry(u"SelfCritique"_s, settings.selfCritique);
    g.writeEntry(u"ParallelToolCalls"_s, settings.parallelToolCalls);
    g.writeEntry(u"Verbosity"_s, settings.verbosity);
    g.writeEntry(u"AutoCollapseThinking"_s, settings.autoCollapseThinking);
    g.writeEntry(u"MaxSavedConversations"_s, settings.maxSavedConversations);
    g.writeEntry(u"MaxExpandedToolCards"_s, settings.maxExpandedToolCards);
    // MCP
    g.writeEntry(u"McpEnabled"_s, settings.mcpEnabled);
    g.writeEntry(u"McpAutoConnect"_s, settings.mcpAutoConnect);
    g.writeEntry(u"McpTimeoutMs"_s, settings.mcpTimeoutMs);
        g.writeEntry(u"WebSearchProvider"_s, settings.webSearchProvider);
        g.writeEntry(u"WebSearchApiKey"_s, settings.webSearchApiKey);
        g.writeEntry(u"WebSearchEndpoint"_s, settings.webSearchEndpoint);
        g.writeEntry(u"WebSearchMaxResults"_s, settings.webSearchMaxResults);
        g.writeEntry(u"WebSearchTimeoutMs"_s, settings.webSearchTimeoutMs);
    // Checkpoints
    g.writeEntry(u"CheckpointsEnabled"_s, settings.checkpointsEnabled);
    g.writeEntry(u"CheckpointRetention"_s, settings.checkpointRetention);
    // Subtasks
    g.writeEntry(u"MaxSubtaskDepth"_s, settings.maxSubtaskDepth);
    g.writeEntry(u"MaxParallelSubtasks"_s, settings.maxParallelSubtasks);
    g.writeEntry(u"AgentRoster"_s, settings.agentRoster);
    g.writeEntry(u"SubtaskTimeoutMs"_s, settings.subtaskTimeoutMs);
    // Appearance
    g.writeEntry(u"ThemeSource"_s, settings.themeSource);
    g.writeEntry(u"ThemePreset"_s, settings.themePreset);
    g.writeEntry(u"ThemeOverrides"_s, settings.themeOverrides);
    // Keep every field the UI can write, so a restart restores the same
    // configuration instead of silently falling back to struct defaults.
    g.writeEntry(u"ToolCallTimeoutMs"_s, settings.toolCallTimeoutMs);
    g.writeEntry(u"MaxContextMessages"_s, settings.maxContextMessages);
    g.writeEntry(u"CompactOnFailure"_s, settings.compactOnFailure);
    g.writeEntry(u"StructuredThinking"_s, settings.structuredThinking);
    g.writeEntry(u"StructuredPlanning"_s, settings.structuredPlanning);
    g.writeEntry(u"ShowPlanAsChecklist"_s, settings.showPlanAsChecklist);
    g.writeEntry(u"MaxThinkingTokens"_s, settings.maxThinkingTokens);
    g.writeEntry(u"MaxPlanSteps"_s, settings.maxPlanSteps);
    g.writeEntry(u"EnablePlanUpdates"_s, settings.enablePlanUpdates);
    g.writeEntry(u"NarrativeProgress"_s, settings.narrativeProgress);
    g.writeEntry(u"MessageSpeed"_s, settings.messageSpeed);
    g.writeEntry(u"FrequencyPenalty"_s, settings.frequencyPenalty);
    g.writeEntry(u"PresencePenalty"_s, settings.presencePenalty);
    g.writeEntry(u"RequireVerification"_s, settings.requireVerification);
    g.writeEntry(u"MaxVerificationAttempts"_s, settings.maxVerificationAttempts);
    g.writeEntry(u"AdaptiveTemperature"_s, settings.adaptiveTemperature);
    g.writeEntry(u"ExplorationTemperature"_s, settings.explorationTemperature);
    g.writeEntry(u"ExploitationTemperature"_s, settings.exploitationTemperature);
    g.writeEntry(u"EnableAutoRetry"_s, settings.enableAutoRetry);
    g.writeEntry(u"MaxRetryAttempts"_s, settings.maxRetryAttempts);
    g.writeEntry(u"BaseRetryDelaySeconds"_s, settings.baseRetryDelaySeconds);
    g.writeEntry(u"MaxRetryDelaySeconds"_s, settings.maxRetryDelaySeconds);
    g.writeEntry(u"RetryStrategy"_s, settings.retryStrategy);
    g.writeEntry(u"ContextWindow"_s, settings.contextWindow);
    g.writeEntry(u"KeepRecentTokens"_s, settings.keepRecentTokens);
    g.writeEntry(u"ContextWindowReserve"_s, settings.contextWindowReserve);
    g.writeEntry(u"CompressOldMessages"_s, settings.compressOldMessages);
    g.writeEntry(u"SmartContextTruncation"_s, settings.smartContextTruncation);
    g.writeEntry(u"CompressionThreshold"_s, settings.compressionThreshold);
    
    g.sync();

    // The API keys live in this file in plain text. The file is created with
    // the permissions of the user's umask, which usually leaves it readable
    // by every account on the machine. It is tightened to the owner only.
    const QString configFile = g.config()->name();
    if (!configFile.isEmpty()) {
        QFile file(configFile);
        if (file.exists()) {
            const QFileDevice::Permissions wanted =
                QFileDevice::ReadOwner | QFileDevice::WriteOwner;
            if ((file.permissions() & QFileDevice::ReadGroup) || (file.permissions() & QFileDevice::ReadOther)) {
                file.setPermissions(wanted);
            }
        }
    }
}

} // namespace KateAi
