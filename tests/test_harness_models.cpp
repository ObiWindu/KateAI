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
};

QTEST_GUILESS_MAIN(TestHarnessModels)
#include "test_harness_models.moc"
