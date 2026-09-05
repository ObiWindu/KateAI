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
    s.maxIterations = g.readEntry(u"MaxIterations"_s, 20);
    s.bashTimeoutMs = g.readEntry(u"BashTimeoutMs"_s, 60000);
    s.extraSystemPrompt = g.readEntry(u"ExtraSystemPrompt"_s, QString());
    s.extraDenyGlobs = g.readEntry(u"ExtraDenyGlobs"_s, QStringList());
    if (s.maxIterations < 1) {
        s.maxIterations = 1;
    }
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
    g.writeEntry(u"MaxIterations"_s, settings.maxIterations);
    g.writeEntry(u"BashTimeoutMs"_s, settings.bashTimeoutMs);
    g.writeEntry(u"ExtraSystemPrompt"_s, settings.extraSystemPrompt);
    g.writeEntry(u"ExtraDenyGlobs"_s, settings.extraDenyGlobs);
    g.sync();
}

} // namespace KateAi
