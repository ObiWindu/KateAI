/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "../types.h"

#include <QList>
#include <QString>

namespace KateAi
{

// How a provider accepts prompt-cache annotations. The distinction matters:
// Anthropic and Gemini need explicit breakpoints, OpenAI-compatible providers
// cache stable prefixes automatically, and local servers usually do not cache
// at all.
enum class CacheStyle {
    None,
    Automatic,
    Anthropic,
    Gemini,
};

// The edit tool dialect a family handles best. Kate keeps the exact-match
// contract for every editor; this only changes the guidance, not the safety.
enum class EditDialect {
    Exact,
    PatchFirst,
    Fuzzy,
};

// A prompt family. The profile chooses which optional prose is worth its
// tokens and which tool dialect the model was trained on.
enum class PromptVariant {
    Local,
    Anthropic,
    OpenAI,
    DeepSeek,
    Gemini,
    Grok,
};

/**
 * Capabilities and request-shaping facts for one model family.
 *
 * The catalogue is deliberately family-based, not a list of every model id a
 * provider may launch. A match on the family prefix gives a safe window and
 * the right request shape; a user override of the context window still wins.
 */
struct ModelProfile {
    QString id;
    QString family;
    QString displayName;
    Provider provider = Provider::OpenAICompatible;

    int contextWindow = 32768;
    int maxOutputTokens = 8192;
    // USD per million tokens. 0 means "not known"; cost reports then say so
    // instead of inventing a price.
    double inputPrice = 0.0;
    double outputPrice = 0.0;
    double cacheReadPrice = 0.0;
    double cacheWritePrice = 0.0;

    CacheStyle cacheStyle = CacheStyle::None;
    PromptVariant promptVariant = PromptVariant::Local;
    EditDialect editDialect = EditDialect::Exact;
    // "openai" or "anthropic": the schema dialect the transport should use.
    QString toolSchemaDialect = QStringLiteral("openai");
    bool supportsParallelToolCalls = true;
    bool supportsReasoning = false;
    // Levels the model advertises, e.g. {"low", "medium", "high", "xhigh"}.
    // Empty means "reasoning is supported, but the catalogue did not list
    // levels"; the UI then offers the generic set.
    QStringList reasoningEfforts;
    bool supportsToolChoice = true;

    // Multiplier applied to the character/4 estimate. Learned values come from
    // TokenCalibrator; this is the conservative seed.
    double tokenFactor = 1.0;
    // True when the profile came from a known family rather than the fallback.
    bool known = false;

    bool supportsCache() const
    {
        return cacheStyle != CacheStyle::None;
    }
    bool hasPricing() const
    {
        return inputPrice > 0.0 || outputPrice > 0.0;
    }
};

/**
 * The bundled model registry.
 *
 * Resolution order: exact id, family prefix, then a conservative fallback.
 * The fallback intentionally keeps the 32k window and exact editing: an
 * unknown model must compact early rather than overflow, and must never be
 * pushed into a fuzzy edit path it was not trained for.
 */
class ModelProfiles
{
public:
    static QList<ModelProfile> catalog();

    // Resolves a model for a provider. The provider is used to fill prices and
    // cache semantics when the family is shared across gateways.
    static ModelProfile resolve(Provider provider, const QString &model);

    // Replaces the dynamically learned entries for one provider with the
    // metadata from a model-list response. Models.dev-style fields (limit,
    // cost, capabilities, family, display name) override the bundled family
    // defaults; absent fields keep them. This is what lets OpenCode's new
    // subscription models report the right window, cache prices and thinking
    // levels without shipping a hard-coded catalogue.
    static void registerModels(Provider provider, const QJsonArray &models);
    static void clearRegisteredModels(Provider provider);
    // The levels a model advertises. Falls back to the generic set when the
    // model supports reasoning but no explicit list was published.
    static QStringList reasoningEfforts(Provider provider, const QString &model);

    // Convenience for callers that only need the window.
    static int contextWindow(Provider provider, const QString &model);

    static PromptVariant variantFor(Provider provider, const QString &model);
    static EditDialect editDialectFor(Provider provider, const QString &model);
    static CacheStyle cacheStyleFor(Provider provider, const QString &model);
    static QString familyId(Provider provider, const QString &model);
    static bool isKnown(Provider provider, const QString &model);

    // JSON for the inspector and settings diagnostics. Stable key order.
    static QJsonObject toJson(const ModelProfile &profile);
};

} // namespace KateAi
