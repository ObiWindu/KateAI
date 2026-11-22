/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "tokens.h"

#include "../contextmanager.h"

#include <QtMath>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace Tokens
{

int estimate(const QString &text)
{
    return ContextManager::estimateTokens(text);
}

int estimate(const QList<ChatMessage> &messages)
{
    return ContextManager::estimateTokens(messages);
}

int applyFactor(int tokens, double factor)
{
    if (tokens <= 0) {
        return 0;
    }
    const double scaled = tokens * qBound(kMinFactor, factor, kMaxFactor);
    return qMax(1, qCeil(scaled));
}

QString truncateToTokenBudget(const QString &text, int budgetTokens, bool *truncated)
{
    if (truncated) {
        *truncated = false;
    }
    if (text.isEmpty()) {
        return text;
    }
    if (budgetTokens <= 0) {
        if (truncated) {
            *truncated = true;
        }
        return QString();
    }
    if (estimate(text) <= budgetTokens) {
        return text;
    }
    if (truncated) {
        *truncated = true;
    }

    // The estimate is monotonic as a prefix grows, so binary search the
    // largest UTF-16 prefix that fits. This is correct for both the ASCII
    // fast path and the UTF-8 byte heuristic used for non-ASCII text.
    int low = 0;
    int high = text.size();
    while (low < high) {
        const int mid = low + (high - low + 1) / 2;
        if (estimate(text.left(mid)) <= budgetTokens) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    while (low > 0 && low < text.size() && text.at(low - 1).isHighSurrogate()
           && text.at(low).isLowSurrogate()) {
        --low;
    }
    return text.left(low);
}

double Calibrator::observe(int estimated, int actual)
{
    // Below a few hundred tokens the message overhead dominates and the ratio
    // says more about the fixture than about the tokenizer.
    if (estimated < 256 || actual <= 0) {
        return m_factor;
    }
    const double ratio = static_cast<double>(actual) / static_cast<double>(estimated);
    if (!qIsFinite(ratio) || ratio <= 0.0) {
        return m_factor;
    }
    // Move a quarter of the way towards the observation, then clamp. The
    // clamp is the contract: an adversarial report cannot set a 0.01 factor.
    const double observed = m_factor * 0.75 + ratio * 0.25;
    m_factor = qBound(kMinFactor, observed, kMaxFactor);
    ++m_samples;
    return m_factor;
}

QString Calibrator::encode(double factor)
{
    return QString::number(qBound(kMinFactor, factor, kMaxFactor), 'f', 3);
}

double Calibrator::decode(const QString &encoded, double fallback)
{
    bool ok = false;
    const double value = encoded.toDouble(&ok);
    if (!ok || !qIsFinite(value) || value <= 0.0) {
        return qBound(kMinFactor, fallback, kMaxFactor);
    }
    return qBound(kMinFactor, value, kMaxFactor);
}

} // namespace Tokens
} // namespace KateAi
