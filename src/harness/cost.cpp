/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cost.h"

#include <QJsonArray>
#include <QJsonValue>

#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

CostUsage &CostUsage::operator+=(const CostUsage &other)
{
    inputTokens += other.inputTokens;
    outputTokens += other.outputTokens;
    cachedInputTokens += other.cachedInputTokens;
    cacheWriteTokens += other.cacheWriteTokens;
    return *this;
}

QJsonObject CostUsage::toJson() const
{
    QJsonObject object;
    object.insert(u"inputTokens"_s, inputTokens);
    object.insert(u"outputTokens"_s, outputTokens);
    object.insert(u"cachedInputTokens"_s, cachedInputTokens);
    object.insert(u"cacheWriteTokens"_s, cacheWriteTokens);
    return object;
}

void CostBreakdown::add(const CostBreakdown &other)
{
    usage += other.usage;
    inputUsd += other.inputUsd;
    outputUsd += other.outputUsd;
    cacheReadUsd += other.cacheReadUsd;
    cacheWriteUsd += other.cacheWriteUsd;
    priced = priced || other.priced;
}

QJsonObject CostBreakdown::toJson() const
{
    QJsonObject object;
    object.insert(u"priced"_s, priced);
    object.insert(u"inputUsd"_s, inputUsd);
    object.insert(u"outputUsd"_s, outputUsd);
    object.insert(u"cacheReadUsd"_s, cacheReadUsd);
    object.insert(u"cacheWriteUsd"_s, cacheWriteUsd);
    object.insert(u"totalUsd"_s, totalUsd());
    object.insert(u"usage"_s, usage.toJson());
    return object;
}

CostBreakdown CostBreakdown::compute(const ModelProfile &profile, const CostUsage &usage)
{
    CostBreakdown result;
    result.usage = usage;
    result.priced = profile.hasPricing();
    if (!result.priced) {
        return result;
    }
    constexpr double million = 1000000.0;
    result.inputUsd = usage.inputTokens * profile.inputPrice / million;
    result.outputUsd = usage.outputTokens * profile.outputPrice / million;
    result.cacheReadUsd = usage.cachedInputTokens * profile.cacheReadPrice / million;
    result.cacheWriteUsd = usage.cacheWriteTokens * profile.cacheWritePrice / million;
    return result;
}

QString CostBreakdown::formatUsd(double usd)
{
    if (usd == 0.0) {
        return u"$0.0000"_s;
    }
    if (std::abs(usd) < 0.0001) {
        return u"<$0.0001"_s;
    }
    return u"$%1"_s.arg(QString::number(usd, 'f', 4));
}

void CostLedger::reset()
{
    m_entries.clear();
    m_subagents.clear();
    m_total = CostBreakdown();
}

void CostLedger::record(const QString &key, const QString &model, const ModelProfile &profile,
                        const CostUsage &usage)
{
    if (usage.isEmpty()) {
        return;
    }
    const QString entryKey = key.isEmpty() ? model : key;
    if (entryKey.isEmpty()) {
        return;
    }
    Entry &entry = m_entries[entryKey];
    entry.key = entryKey;
    entry.model = model;
    ++entry.calls;
    const CostBreakdown priced = CostBreakdown::compute(profile, usage);
    entry.cost.add(priced);
    m_total.add(priced);
}

void CostLedger::recordSubagent(const QString &taskId, const QString &model,
                                const ModelProfile &profile, const CostUsage &usage)
{
    if (taskId.isEmpty() || usage.isEmpty()) {
        return;
    }
    // Cost attribution is useful for the current session, but a long-running
    // conversation must not accumulate one entry per sub-agent forever. The
    // per-model totals in m_total stay exact regardless of this cap.
    constexpr int kMaxSubagentEntries = 256;
    if (!m_subagents.contains(taskId) && m_subagents.size() >= kMaxSubagentEntries) {
        m_subagents.erase(m_subagents.begin());
    }
    Entry &entry = m_subagents[taskId];
    entry.key = taskId;
    entry.model = model;
    ++entry.calls;
    const CostBreakdown priced = CostBreakdown::compute(profile, usage);
    entry.cost.add(priced);
    m_total.add(priced);
}

void CostLedger::setBudgetUsd(double budget)
{
    m_budgetUsd = qMax(0.0, budget);
}

bool CostLedger::overBudget() const
{
    return m_budgetUsd > 0.0 && m_total.priced && m_total.totalUsd() > m_budgetUsd;
}

bool CostLedger::hasUsage() const
{
    return !m_total.usage.isEmpty();
}

QList<CostLedger::Entry> CostLedger::entries() const
{
    QList<Entry> result;
    result.reserve(m_entries.size());
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        result.append(it.value());
    }
    return result;
}

CostBreakdown CostLedger::total() const
{
    return m_total;
}

QJsonObject CostLedger::toJson() const
{
    QJsonArray entries;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        QJsonObject object = it.value().cost.toJson();
        object.insert(u"key"_s, it.value().key);
        object.insert(u"model"_s, it.value().model);
        object.insert(u"calls"_s, it.value().calls);
        entries.append(object);
    }
    QJsonArray subagents;
    for (auto it = m_subagents.constBegin(); it != m_subagents.constEnd(); ++it) {
        QJsonObject object = it.value().cost.toJson();
        object.insert(u"taskId"_s, it.value().key);
        object.insert(u"model"_s, it.value().model);
        object.insert(u"calls"_s, it.value().calls);
        subagents.append(object);
    }
    QJsonObject total = m_total.toJson();
    total.insert(u"budgetUsd"_s, m_budgetUsd);
    total.insert(u"overBudget"_s, overBudget());
    QJsonObject object;
    object.insert(u"total"_s, total);
    object.insert(u"entries"_s, entries);
    object.insert(u"subagents"_s, subagents);
    return object;
}

QString CostLedger::summary() const
{
    if (!hasUsage()) {
        return u"No provider usage recorded yet.\n"_s;
    }
    QString text;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        const Entry &entry = it.value();
        text += u"  %1%2: %3 calls, %4 input, %5 output, %6 cached tokens; %7\n"_s
                    .arg(entry.key,
                         entry.model.isEmpty() ? QString()
                                               : QStringLiteral(" (") + entry.model
                                                   + QStringLiteral(")"))
                    .arg(entry.calls)
                    .arg(entry.cost.usage.inputTokens)
                    .arg(entry.cost.usage.outputTokens)
                    .arg(entry.cost.usage.cachedInputTokens)
                    .arg(entry.cost.priced ? CostBreakdown::formatUsd(entry.cost.totalUsd())
                                           : u"price unknown"_s);
    }
    const CostBreakdown total = m_total;
    if (total.priced) {
        text += u"  Total: %1 (input %2, output %3, cache read %4, cache write %5)\n"_s
                    .arg(CostBreakdown::formatUsd(total.totalUsd()),
                         CostBreakdown::formatUsd(total.inputUsd),
                         CostBreakdown::formatUsd(total.outputUsd),
                         CostBreakdown::formatUsd(total.cacheReadUsd),
                         CostBreakdown::formatUsd(total.cacheWriteUsd));
        if (m_budgetUsd > 0.0) {
            text += u"  Budget: %1 of %2%3\n"_s
                        .arg(CostBreakdown::formatUsd(total.totalUsd()),
                             CostBreakdown::formatUsd(m_budgetUsd),
                             overBudget() ? u" (exceeded)"_s : QString());
        }
    } else {
        text += u"  Total: price unknown for this model; token counts are still recorded.\n"_s;
    }
    if (!m_subagents.isEmpty()) {
        text += u"  Sub-agents:\n"_s;
        for (auto it = m_subagents.constBegin(); it != m_subagents.constEnd(); ++it) {
            text += u"    %1: %2 input, %3 output, %4\n"_s
                        .arg(it.value().key)
                        .arg(it.value().cost.usage.inputTokens)
                        .arg(it.value().cost.usage.outputTokens)
                        .arg(it.value().cost.priced
                                 ? CostBreakdown::formatUsd(it.value().cost.totalUsd())
                                 : u"price unknown"_s);
        }
    }
    return text;
}

} // namespace KateAi
