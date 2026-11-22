/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "../types.h"

#include <QString>

namespace KateAi
{

/**
 * Token estimation and per-model calibration.
 *
 * The base estimate is the cheap character heuristic used by the legacy
 * context manager. Calibration compares that estimate with the provider's
 * reported input tokens and stores a multiplicative factor, clamped to a safe
 * range so one bad report cannot make compaction wildly optimistic.
 */
namespace Tokens
{

constexpr double kMinFactor = 0.5;
constexpr double kMaxFactor = 2.0;

int estimate(const QString &text);
int estimate(const QList<ChatMessage> &messages);
// Applies a calibration factor, rounding up so a fractional token still costs.
int applyFactor(int tokens, double factor);

class Calibrator
{
public:
    Calibrator() = default;
    explicit Calibrator(double factor)
        : m_factor(qBound(kMinFactor, factor, kMaxFactor))
    {
    }

    double factor() const
    {
        return m_factor;
    }
    int samples() const
    {
        return m_samples;
    }

    // Records one observation. `estimated` is the pre-factor estimate and
    // `actual` the provider's input token count. A ratio is ignored when the
    // estimate is too small to be meaningful, or when the provider reports 0.
    // The factor moves by a bounded fraction of the error so one burst cannot
    // swing it.
    double observe(int estimated, int actual);

    // Parses/renders the factor for settings persistence.
    static QString encode(double factor);
    static double decode(const QString &encoded, double fallback = 1.0);

private:
    double m_factor = 1.0;
    int m_samples = 0;
};

} // namespace Tokens

} // namespace KateAi
