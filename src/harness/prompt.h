/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "../modes.h"
#include "../types.h"
#include "models.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace KateAi
{

// Static sections never change during a conversation. Session sections may
// change when the mode or the rules change. Turn sections are kept out of the
// system prompt entirely; they travel as a transient user message so the cache
// prefix stays byte-stable.
enum class PromptStability {
    Static,
    Session,
    Turn,
};

struct PromptSection {
    QString id;
    QString title;
    PromptStability stability = PromptStability::Static;
    // Lower orders come first. Ties are broken by id, never by insertion order,
    // so the rendered prompt is deterministic across runs.
    int order = 0;
    // Budget in tokens; 0 means the global section allowance applies.
    int budgetTokens = 0;
    QString content;

    bool isEmpty() const
    {
        return content.trimmed().isEmpty();
    }
};

struct PromptRequest {
    Settings settings;
    ModeDefinition mode;
    Provider provider = Provider::OpenAICompatible;
    QString modelName;
    QString workspace;
    // Pre-rendered fragments. Keeping IO out of the assembler makes golden
    // tests cheap and deterministic.
    QString rules;
    QString memory;
    QString skills;
    QString graph;
    QString plan;
    // Names the model may call, already filtered by the mode.
    QStringList toolNames;
    QString subagentRoster;
    // Tokens the transport will spend on tool schemas; counted against the
    // same 25% prompt budget so the manifest explains the whole request.
    int toolSchemaTokens = 0;
    // The verification gate is on. Its prompt section replaces the legacy
    // duplicate verification prose instead of adding to it.
    bool verificationGate = false;
};

struct PromptResult {
    QString text;
    QString staticPrefix;
    QString staticHash;
    QList<PromptSection> sections;
    QJsonObject manifest;
    int totalTokens = 0;
    int budgetTokens = 0;
    PromptVariant variant = PromptVariant::Local;
    bool truncated = false;
};

/**
 * Deterministic, budgeted system-prompt assembly.
 *
 * The legacy `systemPrompt()` assembled prose ad hoc, appended editor state,
 * and could change byte-for-byte every turn. This assembler keeps the system
 * prompt to stable sections, enforces per-section and total budgets, and
 * records a manifest so `/context` can explain where the tokens went.
 */
class PromptAssembler
{
public:
    // 25% of the model window, with a floor so a tiny local model still gets a
    // usable instruction set.
    static int totalBudgetFor(int contextWindow);

    static PromptResult assemble(const PromptRequest &request);

    // Renders already-prepared sections. Exposed for tests and for callers that
    // only need a sub-prompt.
    static QString render(const QList<PromptSection> &sections, int totalBudget,
                          QJsonObject *manifest, bool *truncated);

    static QString variantId(PromptVariant variant);
    static QString stabilityId(PromptStability stability);
    static QString hashOf(const QString &text);
};

} // namespace KateAi
