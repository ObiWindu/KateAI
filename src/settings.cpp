/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "settings.h"

#include <KConfigGroup>
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
    s.acpApiKey = g.readEntry(u"AcpApiKey"_s, QString());
    // Models are read from the provider, not hard-coded here; an empty entry
    // simply means "not chosen yet" and the fetched catalogue fills it in.
    s.grokModel = g.readEntry(u"GrokModel"_s, QString());
    s.openaiModel = g.readEntry(u"OpenAIModel"_s, QString());
    s.openrouterModel = g.readEntry(u"OpenRouterModel"_s, QString());
    s.deepseekModel = g.readEntry(u"DeepSeekModel"_s, QString());
    s.acpModel = g.readEntry(u"AcpModel"_s, QString());
    s.opencodeApiKey = g.readEntry(u"OpenCodeApiKey"_s, QString());
    s.opencodeModel = g.readEntry(u"OpenCodeModel"_s, QString());
    s.opencodeUrl = g.readEntry(u"OpenCodeUrl"_s, u"https://opencode.ai/zen/v1"_s);
    s.deepseekUrl = g.readEntry(u"DeepSeekUrl"_s, u"https://api.deepseek.com"_s);
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
    const int legacyMaxIterations = g.readEntry(u"MaxIterations"_s, 0);
    s.maxModelRequests = g.readEntry(u"MaxModelRequests"_s, 0);
    s.maxToolCalls = g.readEntry(u"MaxToolCalls"_s, legacyMaxIterations);
    s.requestsPerMinute = g.readEntry(u"RequestsPerMinute"_s, 15);
    s.maxIterations = legacyMaxIterations;
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
    g.writeEntry(u"AcpApiKey"_s, settings.acpApiKey);
        g.writeEntry(u"OpenCodeApiKey"_s, settings.opencodeApiKey);
        g.writeEntry(u"OpenCodeModel"_s, settings.opencodeModel);
        g.writeEntry(u"OpenCodeUrl"_s, settings.opencodeUrl);
    g.writeEntry(u"GrokModel"_s, settings.grokModel);
    g.writeEntry(u"OpenAIModel"_s, settings.openaiModel);
    g.writeEntry(u"OpenRouterModel"_s, settings.openrouterModel);
    g.writeEntry(u"DeepSeekModel"_s, settings.deepseekModel);
    g.writeEntry(u"AcpModel"_s, settings.acpModel);
    g.writeEntry(u"DeepSeekUrl"_s, settings.deepseekUrl);
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
    g.writeEntry(u"ContextWindow"_s, settings.contextWindow);
    g.writeEntry(u"KeepRecentTokens"_s, settings.keepRecentTokens);
    g.writeEntry(u"ContextWindowReserve"_s, settings.contextWindowReserve);
    g.writeEntry(u"CompressOldMessages"_s, settings.compressOldMessages);
    g.writeEntry(u"SmartContextTruncation"_s, settings.smartContextTruncation);
    g.writeEntry(u"CompressionThreshold"_s, settings.compressionThreshold);
    
    g.sync();
}

} // namespace KateAi
