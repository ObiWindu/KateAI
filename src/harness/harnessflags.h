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

    // Substrate. Off until the implementation lands.
    bool eventLog = false;
    bool middleware = false;
    bool verificationGate = false;
    bool policyV2 = false;
    bool hooks = false;

    // Later releases.
    bool promptV2 = false;
    bool contextV2 = false;
    bool memoryV2 = false;
    bool toolSearch = false;
    bool skills = false;
    bool agentsV2 = false;
    bool inspector = false;
    bool telemetry = false;

    static HarnessFlags load();

    // Applies a comma-separated override list. Unknown names are ignored so a
    // typo in an environment variable cannot disable a safety feature.
    void applyOverrides(const QString &overrides);

    bool enabled(const QString &name) const;
    QStringList enabledNames() const;
};

} // namespace KateAi
