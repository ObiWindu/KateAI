/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "contextmanager.h"
#include "harness/models.h"

#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestHarnessModels : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void resolvesKnownFamilies()
    {
        const ModelProfile claude = ModelProfiles::resolve(Provider::ClaudeCompatible, u"claude-sonnet-4"_s);
        QVERIFY(claude.known);
        QCOMPARE(claude.contextWindow, 200000);
        QCOMPARE(claude.cacheStyle, CacheStyle::Anthropic);
        QCOMPARE(claude.promptVariant, PromptVariant::Anthropic);
        QCOMPARE(claude.toolSchemaDialect, u"anthropic"_s);

        const ModelProfile deepseek = ModelProfiles::resolve(Provider::DeepSeek, u"deepseek-chat"_s);
        QCOMPARE(deepseek.contextWindow, 128000);
        QCOMPARE(deepseek.promptVariant, PromptVariant::DeepSeek);

        const ModelProfile gemini = ModelProfiles::resolve(Provider::OpenAICompatible, u"gemini-2.5-pro"_s);
        QCOMPARE(gemini.contextWindow, 1000000);
        QCOMPARE(gemini.cacheStyle, CacheStyle::Gemini);

        const ModelProfile grok = ModelProfiles::resolve(Provider::Grok, u"grok-4"_s);
        QCOMPARE(grok.contextWindow, 256000);

        const ModelProfile gpt = ModelProfiles::resolve(Provider::OpenAI, u"gpt-5-codex"_s);
        QCOMPARE(gpt.editDialect, EditDialect::PatchFirst);
        QVERIFY(gpt.supportsReasoning);
    }

    void legacyWindowFamiliesStillResolve()
    {
        const ModelProfile cohere =
            ModelProfiles::resolve(Provider::OpenAICompatible, u"command-r-plus"_s);
        QVERIFY(cohere.known);
        QCOMPARE(cohere.contextWindow, 128000);
    }

    void unknownModelGetsConservativeFallback()
    {
        const ModelProfile unknown = ModelProfiles::resolve(Provider::OpenAICompatible, u"mystery-1"_s);
        QVERIFY(!unknown.known);
        QCOMPARE(unknown.contextWindow, 32768);
        QCOMPARE(unknown.editDialect, EditDialect::Exact);
        QCOMPARE(unknown.cacheStyle, CacheStyle::None);
        QCOMPARE(unknown.family, u"unknown"_s);
    }

    void contextManagerUsesTheSameRegistry()
    {
        QCOMPARE(ContextManager::contextWindowFor(u"gemini-2.5-pro"_s), 1000000);
        QCOMPARE(ContextManager::contextWindowFor(u"deepseek-chat"_s), 128000);
        QCOMPARE(ContextManager::contextWindowFor(u"totally-unknown"_s), 32768);
    }

    void jsonIsStable()
    {
        const ModelProfile profile = ModelProfiles::resolve(Provider::OpenAI, u"gpt-4.1"_s);
        const QJsonObject json = ModelProfiles::toJson(profile);
        QCOMPARE(json.value(u"family"_s).toString(), u"openai"_s);
        QCOMPARE(json.value(u"cacheStyle"_s).toString(), u"automatic"_s);
        QCOMPARE(json.value(u"editDialect"_s).toString(), u"patch-first"_s);
        QVERIFY(json.value(u"known"_s).toBool());
    }

    void catalogueMetadataOverridesFamilyDefaults()
    {
        // OpenCode Console publishes models.dev v2 metadata. The resolver must
        // use the live window, prices, cache support and thinking levels
        // instead of the bundled Claude family defaults.
        QJsonObject model;
        model.insert(u"id"_s, u"claude-sonnet-5-5"_s);
        model.insert(u"name"_s, u"Claude Sonnet 5.5"_s);
        model.insert(u"family"_s, u"claude-sonnet"_s);
        model.insert(u"limit"_s, QJsonObject{{u"context"_s, 1000000},
                                             {u"output"_s, 128000}});
        model.insert(u"cost"_s, QJsonObject{
            {u"text"_s, QJsonObject{
                {u"input"_s, QJsonObject{
                    {u"price"_s, 2.0},
                    {u"cache"_s, QJsonObject{{u"read"_s, 0.1}, {u"write"_s, 2.5}}}}},
                {u"output"_s, QJsonObject{{u"price"_s, 10.0}}}}}});
        model.insert(u"capabilities"_s, QJsonObject{
            {u"reasoning"_s, QJsonObject{
                {u"supported"_s, true},
                {u"effort"_s, QJsonArray{u"low"_s, u"medium"_s, u"high"_s, u"xhigh"_s, u"max"_s}}}},
            {u"tools"_s, QJsonObject{{u"supported"_s, true}}}});

        ModelProfiles::registerModels(Provider::OpenCode, QJsonArray{model});
        const ModelProfile profile =
            ModelProfiles::resolve(Provider::OpenCode, u"claude-sonnet-5-5"_s);
        QVERIFY(profile.known);
        QCOMPARE(profile.provider, Provider::OpenCode);
        QCOMPARE(profile.displayName, u"Claude Sonnet 5.5"_s);
        QCOMPARE(profile.contextWindow, 1000000);
        QCOMPARE(profile.maxOutputTokens, 128000);
        QCOMPARE(profile.inputPrice, 2.0);
        QCOMPARE(profile.outputPrice, 10.0);
        QCOMPARE(profile.cacheReadPrice, 0.1);
        QCOMPARE(profile.cacheWritePrice, 2.5);
        QCOMPARE(profile.cacheStyle, CacheStyle::Automatic);
        QVERIFY(profile.supportsReasoning);
        QCOMPARE(profile.reasoningEfforts,
                 (QStringList{u"low"_s, u"medium"_s, u"high"_s, u"xhigh"_s, u"max"_s}));
        QCOMPARE(ModelProfiles::reasoningEfforts(Provider::OpenCode, u"claude-sonnet-5-5"_s),
                 profile.reasoningEfforts);

        // The learned entry belongs to OpenCode only; another gateway still
        // gets the bundled family profile.
        const ModelProfile elsewhere =
            ModelProfiles::resolve(Provider::ClaudeCompatible, u"claude-sonnet-5-5"_s);
        QCOMPARE(elsewhere.cacheStyle, CacheStyle::Anthropic);
        QCOMPARE(elsewhere.contextWindow, 200000);

        ModelProfiles::clearRegisteredModels(Provider::OpenCode);
        const ModelProfile cleared =
            ModelProfiles::resolve(Provider::OpenCode, u"claude-sonnet-5-5"_s);
        QCOMPARE(cleared.contextWindow, 200000);
        QCOMPARE(cleared.cacheStyle, CacheStyle::Anthropic);
        QVERIFY(cleared.reasoningEfforts.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestHarnessModels)
#include "test_harness_models.moc"
