/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "models.h"

#include <QJsonObject>
#include <QMap>
#include <QString>

namespace KateAi
{

/**
 * Provider-reported token usage for one or more model calls.
 *
 * `cachedInputTokens` are the tokens the provider served from a prompt cache;
 * `cacheWriteTokens` are the tokens written into a cache (Anthropic and
 * Gemini report these explicitly). Token counts are never inferred here.
 */
struct CostUsage {
    int inputTokens = 0;
    int outputTokens = 0;
    int cachedInputTokens = 0;
    int cacheWriteTokens = 0;

    bool isEmpty() const
    {
        return inputTokens == 0 && outputTokens == 0 && cachedInputTokens == 0
            && cacheWriteTokens == 0;
    }

    int totalInputTokens() const
    {
        return inputTokens + cachedInputTokens;
    }

    CostUsage &operator+=(const CostUsage &other);
    QJsonObject toJson() const;
};

/**
 * The priced view of one usage record.
 *
 * When the model profile has no prices, `priced` is false and every amount is
 * zero. The report then says the price is unknown instead of inventing one.
 */
struct CostBreakdown {
    CostUsage usage;
    double inputUsd = 0.0;
    double outputUsd = 0.0;
    double cacheReadUsd = 0.0;
    double cacheWriteUsd = 0.0;
    bool priced = false;

    double totalUsd() const
    {
        return inputUsd + outputUsd + cacheReadUsd + cacheWriteUsd;
    }

    void add(const CostBreakdown &other);
    QJsonObject toJson() const;

    static CostBreakdown compute(const ModelProfile &profile, const CostUsage &usage);
    // A short, locale-independent amount. Sub-cent values keep enough digits
    // to be useful in a trace without turning the report into noise.
    static QString formatUsd(double usd);
};

/**
 * Per-conversation cost accounting.
 *
 * Records are keyed by role/model so the planner and executor of an
 * orchestration turn are reported separately. The ledger is additive: it is
 * reset only with the conversation, never rewritten by compaction.
 */
class CostLedger
{
public:
    struct Entry {
        QString key;
        QString model;
        int calls = 0;
        CostBreakdown cost;
    };

    void reset();
    void record(const QString &key, const QString &model, const ModelProfile &profile,
                const CostUsage &usage);
    void recordSubagent(const QString &taskId, const QString &model, const ModelProfile &profile,
                        const CostUsage &usage);
    void setBudgetUsd(double budget);
    double budgetUsd() const
    {
        return m_budgetUsd;
    }
    bool overBudget() const;
    bool hasUsage() const;
    QList<Entry> entries() const;
    CostBreakdown total() const;

    // Stable JSON for the inspector and the headless artifact.
    QJsonObject toJson() const;
    // Plain-text lines, without a leading heading.
    QString summary() const;

private:
    QMap<QString, Entry> m_entries;
    QMap<QString, Entry> m_subagents;
    CostBreakdown m_total;
    double m_budgetUsd = 0.0;
};

} // namespace KateAi
