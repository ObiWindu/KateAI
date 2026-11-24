/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>
#include <QStringList>

namespace KateAi
{

/**
 * Rollout switches for the harness work.
 *
 * The defaults are the safe, already-shipped behaviour. A feature that changes
 * behaviour is added here, developed behind its flag, and removed one release
 * after it becomes the default.
 *
 * Precedence: built-in defaults, then the [Harness] KConfig group, then the
 * KATEAI_HARNESS_FLAGS environment variable (comma-separated; "+name" enables,
 * "-name" disables). The environment form is for developers and CI; the config
 * form is written by the settings page in developer mode.
 */
struct HarnessFlags {
    // Context and prompt.
    bool stablePromptPrefix = true;   // editor/git context moves to a turn message
    bool resultBudget = true;         // large tool results go to the content store
    bool contextReport = true;        // :kateai context / :kateai usage

    // Substrate: on by default in 0.7. Each one degrades to the legacy path
    // when its configuration is absent or its flag is turned off.
    bool eventLog = true;
    bool middleware = true;
    bool verificationGate = true;
    bool policyV2 = true;
    bool hooks = true;

    // 0.8: on by default at the release gate; each still degrades to the
    // legacy path when turned off.
    bool promptV2 = true;
    bool contextV2 = true;
    bool memoryV2 = true;
    bool toolSearch = true;
    bool skills = true;
    bool agentsV2 = true;
    bool inspector = true;
    bool orchestration = true;
    bool telemetry = false;

    static HarnessFlags load();

    // Applies a comma-separated override list. Unknown names are ignored so a
    // typo in an environment variable cannot disable a safety feature.
    void applyOverrides(const QString &overrides);

    bool enabled(const QString &name) const;
    QStringList enabledNames() const;
};

} // namespace KateAi
