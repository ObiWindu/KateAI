/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "models.h"

#include <QJsonObject>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace
{

ModelProfile makeProfile(const QString &id, const QString &family, const QString &name,
                         int window, int output)
{
    ModelProfile profile;
    profile.id = id;
    profile.family = family;
    profile.displayName = name;
    profile.contextWindow = window;
    profile.maxOutputTokens = output;
    profile.known = true;
    return profile;
}

bool startsWithAny(const QString &name, const QStringList &prefixes)
{
    for (const QString &prefix : prefixes) {
        if (name.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

bool containsAny(const QString &name, const QStringList &needles)
{
    for (const QString &needle : needles) {
        if (name.contains(needle)) {
            return true;
        }
    }
    return false;
}

// The fallback is intentionally small and conservative. A model Kate does not
// recognise gets the legacy 32k budget and exact editing.
ModelProfile fallbackProfile(Provider provider, const QString &model)
{
    ModelProfile profile;
    profile.id = model;
    profile.family = QStringLiteral("unknown");
    profile.displayName = model.isEmpty() ? QStringLiteral("unknown") : model;
    profile.provider = provider;
    profile.contextWindow = 32768;
    profile.maxOutputTokens = 8192;
    profile.cacheStyle = CacheStyle::None;
    profile.promptVariant = PromptVariant::Local;
    profile.editDialect = EditDialect::Exact;
    profile.toolSchemaDialect = QStringLiteral("openai");
    profile.known = false;
    return profile;
}

// Resolve by the stable profile id, not by a hard-coded list index. Indexing
// made inserting a profile silently map a whole family to the wrong row.
ModelProfile profileById(const QList<ModelProfile> &all, const QString &id,
                         Provider provider, const QString &model)
{
    for (const ModelProfile &profile : all) {
        if (profile.id == id) {
            ModelProfile copy = profile;
            copy.id = model;
            copy.provider = provider;
            return copy;
        }
    }
    return fallbackProfile(provider, model);
}

} // namespace

QList<ModelProfile> ModelProfiles::catalog()
{
    // The catalogue is immutable. Building it once avoids re-allocating a
    // dozen profiles for every resolve() call (prompt, context, reports and
    // every edit-dialect lookup).
    static const QList<ModelProfile> cached = []() {
    QList<ModelProfile> profiles;

    {
        ModelProfile p = makeProfile(u"claude"_s, u"anthropic"_s, u"Claude"_s, 200000, 64000);
        p.inputPrice = 3.0;
        p.outputPrice = 15.0;
        p.cacheReadPrice = 0.3;
        p.cacheWritePrice = 3.75;
        p.cacheStyle = CacheStyle::Anthropic;
        p.promptVariant = PromptVariant::Anthropic;
        p.editDialect = EditDialect::Exact;
        p.toolSchemaDialect = u"anthropic"_s;
        p.supportsReasoning = true;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"gpt-4.1"_s, u"openai"_s, u"GPT-4.1"_s, 1047576, 32768);
        p.inputPrice = 2.0;
        p.outputPrice = 8.0;
        p.cacheReadPrice = 0.5;
        p.cacheStyle = CacheStyle::Automatic;
        p.promptVariant = PromptVariant::OpenAI;
        p.editDialect = EditDialect::PatchFirst;
        p.supportsReasoning = true;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"gpt-4o"_s, u"openai"_s, u"GPT-4o"_s, 128000, 16384);
        p.inputPrice = 2.5;
        p.outputPrice = 10.0;
        p.cacheReadPrice = 1.25;
        p.cacheStyle = CacheStyle::Automatic;
        p.promptVariant = PromptVariant::OpenAI;
        p.editDialect = EditDialect::PatchFirst;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"gpt-5"_s, u"openai"_s, u"GPT-5"_s, 400000, 128000);
        p.inputPrice = 1.25;
        p.outputPrice = 10.0;
        p.cacheReadPrice = 0.125;
        p.cacheStyle = CacheStyle::Automatic;
        p.promptVariant = PromptVariant::OpenAI;
        p.editDialect = EditDialect::PatchFirst;
        p.supportsReasoning = true;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"o3"_s, u"openai"_s, u"o3"_s, 200000, 100000);
        p.inputPrice = 2.0;
        p.outputPrice = 8.0;
        p.cacheReadPrice = 0.5;
        p.cacheStyle = CacheStyle::Automatic;
        p.promptVariant = PromptVariant::OpenAI;
        p.editDialect = EditDialect::PatchFirst;
        p.supportsReasoning = true;
        p.supportsParallelToolCalls = false;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"deepseek"_s, u"deepseek"_s, u"DeepSeek"_s, 128000, 8192);
        p.inputPrice = 0.28;
        p.outputPrice = 0.42;
        p.cacheReadPrice = 0.028;
        p.cacheStyle = CacheStyle::Automatic;
        p.promptVariant = PromptVariant::DeepSeek;
        p.editDialect = EditDialect::Exact;
        p.supportsReasoning = true;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"gemini"_s, u"gemini"_s, u"Gemini"_s, 1000000, 65536);
        p.inputPrice = 1.25;
        p.outputPrice = 10.0;
        p.cacheReadPrice = 0.31;
        p.cacheStyle = CacheStyle::Gemini;
        p.promptVariant = PromptVariant::Gemini;
        p.editDialect = EditDialect::Exact;
        p.supportsReasoning = true;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"grok"_s, u"grok"_s, u"Grok"_s, 256000, 16384);
        p.inputPrice = 3.0;
        p.outputPrice = 15.0;
        p.cacheReadPrice = 0.75;
        p.cacheStyle = CacheStyle::Automatic;
        p.promptVariant = PromptVariant::Grok;
        p.editDialect = EditDialect::Exact;
        p.supportsReasoning = true;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"qwen"_s, u"qwen"_s, u"Qwen"_s, 131072, 16384);
        p.promptVariant = PromptVariant::Local;
        p.editDialect = EditDialect::Exact;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"glm"_s, u"glm"_s, u"GLM"_s, 128000, 8192);
        p.promptVariant = PromptVariant::Local;
        p.editDialect = EditDialect::Exact;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"kimi"_s, u"kimi"_s, u"Kimi"_s, 256000, 16384);
        p.promptVariant = PromptVariant::Local;
        p.editDialect = EditDialect::Exact;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"mistral"_s, u"mistral"_s, u"Mistral"_s, 128000, 8192);
        p.promptVariant = PromptVariant::Local;
        p.editDialect = EditDialect::Exact;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"command-r"_s, u"cohere"_s, u"Command R"_s, 128000, 4096);
        p.promptVariant = PromptVariant::Local;
        p.editDialect = EditDialect::Exact;
        profiles.append(p);
    }
    {
        ModelProfile p = makeProfile(u"llama"_s, u"local"_s, u"Local model"_s, 32768, 4096);
        p.promptVariant = PromptVariant::Local;
        p.editDialect = EditDialect::Exact;
        profiles.append(p);
    }
    return profiles;
    }();
    return cached;
}

ModelProfile ModelProfiles::resolve(Provider provider, const QString &model)
{
    const QString name = model.trimmed().toLower();
    if (name.isEmpty()) {
        return fallbackProfile(provider, model);
    }
    const QList<ModelProfile> all = catalog();

    // Provider-specific gateway prefixes still carry the family in the name;
    // matching on the id is more reliable than on the transport.
    if (containsAny(name, {u"claude"_s})) {
        return profileById(all, u"claude"_s, provider, model);
    }
    if (containsAny(name, {u"deepseek"_s})) {
        return profileById(all, u"deepseek"_s, provider, model);
    }
    if (containsAny(name, {u"gemini"_s})) {
        return profileById(all, u"gemini"_s, provider, model);
    }
    if (containsAny(name, {u"grok"_s})) {
        return profileById(all, u"grok"_s, provider, model);
    }
    if (containsAny(name, {u"qwen"_s, u"qwq"_s})) {
        return profileById(all, u"qwen"_s, provider, model);
    }
    if (containsAny(name, {u"glm"_s})) {
        return profileById(all, u"glm"_s, provider, model);
    }
    if (containsAny(name, {u"kimi"_s, u"moonshot"_s})) {
        return profileById(all, u"kimi"_s, provider, model);
    }
    if (containsAny(name, {u"mistral"_s, u"codestral"_s})) {
        return profileById(all, u"mistral"_s, provider, model);
    }
    if (containsAny(name, {u"command-r"_s, u"cohere"_s})) {
        return profileById(all, u"command-r"_s, provider, model);
    }
    if (containsAny(name, {u"llama"_s, u"phi"_s, u"gemma"_s, u"local"_s})) {
        return profileById(all, u"llama"_s, provider, model);
    }

    if (startsWithAny(name, {u"gpt-4.1"_s})) {
        return profileById(all, u"gpt-4.1"_s, provider, model);
    }
    if (startsWithAny(name, {u"gpt-4o"_s, u"gpt-4-turbo"_s})) {
        return profileById(all, u"gpt-4o"_s, provider, model);
    }
    if (startsWithAny(name, {u"gpt-5"_s, u"codex"_s})) {
        return profileById(all, u"gpt-5"_s, provider, model);
    }
    if (startsWithAny(name, {u"o1"_s, u"o3"_s, u"o4"_s})) {
        return profileById(all, u"o3"_s, provider, model);
    }
    if (name.contains(u"gpt-3.5"_s)) {
        ModelProfile p = fallbackProfile(provider, model);
        p.family = u"openai"_s;
        p.displayName = u"GPT-3.5"_s;
        p.contextWindow = 16384;
        p.maxOutputTokens = 4096;
        p.promptVariant = PromptVariant::OpenAI;
        p.known = true;
        return p;
    }
    if (name.contains(u"gpt"_s)) {
        ModelProfile p = fallbackProfile(provider, model);
        p.family = u"openai"_s;
        p.displayName = u"GPT"_s;
        p.contextWindow = 128000;
        p.maxOutputTokens = 16384;
        p.promptVariant = PromptVariant::OpenAI;
        p.editDialect = EditDialect::PatchFirst;
        p.cacheStyle = CacheStyle::Automatic;
        p.known = true;
        return p;
    }

    return fallbackProfile(provider, model);
}

int ModelProfiles::contextWindow(Provider provider, const QString &model)
{
    return resolve(provider, model).contextWindow;
}

PromptVariant ModelProfiles::variantFor(Provider provider, const QString &model)
{
    return resolve(provider, model).promptVariant;
}

EditDialect ModelProfiles::editDialectFor(Provider provider, const QString &model)
{
    return resolve(provider, model).editDialect;
}

CacheStyle ModelProfiles::cacheStyleFor(Provider provider, const QString &model)
{
    return resolve(provider, model).cacheStyle;
}

QString ModelProfiles::familyId(Provider provider, const QString &model)
{
    return resolve(provider, model).family;
}

bool ModelProfiles::isKnown(Provider provider, const QString &model)
{
    return resolve(provider, model).known;
}

QJsonObject ModelProfiles::toJson(const ModelProfile &profile)
{
    QJsonObject obj;
    obj.insert(u"id"_s, profile.id);
    obj.insert(u"family"_s, profile.family);
    obj.insert(u"displayName"_s, profile.displayName);
    obj.insert(u"provider"_s, providerId(profile.provider));
    obj.insert(u"contextWindow"_s, profile.contextWindow);
    obj.insert(u"maxOutputTokens"_s, profile.maxOutputTokens);
    obj.insert(u"inputPrice"_s, profile.inputPrice);
    obj.insert(u"outputPrice"_s, profile.outputPrice);
    obj.insert(u"cacheReadPrice"_s, profile.cacheReadPrice);
    obj.insert(u"cacheWritePrice"_s, profile.cacheWritePrice);
    QString cache;
    switch (profile.cacheStyle) {
    case CacheStyle::None:
        cache = u"none"_s;
        break;
    case CacheStyle::Automatic:
        cache = u"automatic"_s;
        break;
    case CacheStyle::Anthropic:
        cache = u"anthropic"_s;
        break;
    case CacheStyle::Gemini:
        cache = u"gemini"_s;
        break;
    }
    obj.insert(u"cacheStyle"_s, cache);
    QString variant;
    switch (profile.promptVariant) {
    case PromptVariant::Local:
        variant = u"local"_s;
        break;
    case PromptVariant::Anthropic:
        variant = u"anthropic"_s;
        break;
    case PromptVariant::OpenAI:
        variant = u"openai"_s;
        break;
    case PromptVariant::DeepSeek:
        variant = u"deepseek"_s;
        break;
    case PromptVariant::Gemini:
        variant = u"gemini"_s;
        break;
    case PromptVariant::Grok:
        variant = u"grok"_s;
        break;
    }
    obj.insert(u"promptVariant"_s, variant);
    QString dialect;
    switch (profile.editDialect) {
    case EditDialect::Exact:
        dialect = u"exact"_s;
        break;
    case EditDialect::PatchFirst:
        dialect = u"patch-first"_s;
        break;
    case EditDialect::Fuzzy:
        dialect = u"fuzzy"_s;
        break;
    }
    obj.insert(u"editDialect"_s, dialect);
    obj.insert(u"toolSchemaDialect"_s, profile.toolSchemaDialect);
    obj.insert(u"supportsParallelToolCalls"_s, profile.supportsParallelToolCalls);
    obj.insert(u"supportsReasoning"_s, profile.supportsReasoning);
    obj.insert(u"tokenFactor"_s, profile.tokenFactor);
    obj.insert(u"known"_s, profile.known);
    return obj;
}

} // namespace KateAi
