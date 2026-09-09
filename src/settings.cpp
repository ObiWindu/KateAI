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
    s.grokModel = g.readEntry(u"GrokModel"_s, u"grok-4.5"_s);
    s.openaiModel = g.readEntry(u"OpenAIModel"_s, u"gpt-4.1"_s);
    s.openrouterModel = g.readEntry(u"OpenRouterModel"_s, u"x-ai/grok-4"_s);
    s.permissionMode = permissionModeFromId(g.readEntry(u"PermissionMode"_s, permissionModeId(PermissionMode::Ask)));
    s.sandbox = sandboxProfileFromId(g.readEntry(u"Sandbox"_s, sandboxProfileId(SandboxProfile::Workspace)));
    const int legacyMaxIterations = g.readEntry(u"MaxIterations"_s, 20);
    s.maxModelRequests = g.readEntry(u"MaxModelRequests"_s, 40);
    s.maxToolCalls = g.readEntry(u"MaxToolCalls"_s, legacyMaxIterations);
    s.requestsPerMinute = g.readEntry(u"RequestsPerMinute"_s, 15);
    s.maxIterations = legacyMaxIterations;
    s.bashTimeoutMs = g.readEntry(u"BashTimeoutMs"_s, 60000);
    s.planMode = g.readEntry(u"PlanMode"_s, false);
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
    s.maxEditorContextLength = g.readEntry(u"MaxEditorContextLength"_s, 200);
    s.compressProjectInstructions = g.readEntry(u"CompressProjectInstructions"_s, true);
    s.maxProjectInstructionsLength = g.readEntry(u"MaxProjectInstructionsLength"_s, 2048);
    s.compressSystemPrompt = g.readEntry(u"CompressSystemPrompt"_s, true);
    s.maxSystemPromptLength = g.readEntry(u"MaxSystemPromptLength"_s, 1024);
    if (s.maxModelRequests < 1) {
        s.maxModelRequests = 1;
    }
    if (s.maxToolCalls < 1) {
        s.maxToolCalls = 1;
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
    return s;
}

void SettingsStore::save(const Settings &settings)
{
    KConfigGroup g = group();
    g.writeEntry(u"Provider"_s, providerId(settings.provider));
    g.writeEntry(u"GrokApiKey"_s, settings.grokApiKey);
    g.writeEntry(u"OpenAIApiKey"_s, settings.openaiApiKey);
    g.writeEntry(u"OpenRouterApiKey"_s, settings.openrouterApiKey);
    g.writeEntry(u"GrokModel"_s, settings.grokModel);
    g.writeEntry(u"OpenAIModel"_s, settings.openaiModel);
    g.writeEntry(u"OpenRouterModel"_s, settings.openrouterModel);
    g.writeEntry(u"PermissionMode"_s, permissionModeId(settings.permissionMode));
    g.writeEntry(u"Sandbox"_s, sandboxProfileId(settings.sandbox));
    g.writeEntry(u"MaxModelRequests"_s, settings.maxModelRequests);
    g.writeEntry(u"MaxToolCalls"_s, settings.maxToolCalls);
    g.writeEntry(u"RequestsPerMinute"_s, settings.requestsPerMinute);
    // Preserve the old key for existing versions/UI.
    g.writeEntry(u"MaxIterations"_s, settings.maxToolCalls);
    g.writeEntry(u"BashTimeoutMs"_s, settings.bashTimeoutMs);
    g.writeEntry(u"PlanMode"_s, settings.planMode);
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
    
    g.sync();
}

} // namespace KateAi
