/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QList>
#include <QString>

namespace KateAi
{

// Loads the layered project instructions that get folded into the system
// prompt, following the same discovery order Kilo Code uses for its rules
// files.
class RulesLoader
{
public:
    struct Block {
        // Human-readable origin, e.g. "AGENTS.md" or ".clinerules/style.md".
        QString source;
        QString content;
    };

    // Upper bound per file, so a huge rules file cannot crowd out the
    // conversation.
    static constexpr int MaxFileBytes = 64 * 1024;
    // Upper bound on the number of rule files that are read.
    static constexpr int MaxFiles = 32;

    // Directories scanned for rule files, in the order they are applied.
    static QStringList ruleDirectories(const QString &workspace);
    // Individual rule files at the workspace root.
    static QStringList rootRuleFiles(const QString &workspace);

    // Reads every rule file that applies to `modeId`. Mode-named files
    // (e.g. .kateai/rules/debug.md) apply to every mode for portability, and
    // the mode-specific variant is applied last.
    static QList<Block> load(const QString &workspace, const QString &modeId);
    // Renders the blocks as a <rules> section for the system prompt.
    static QString render(const QList<Block> &blocks, const QString &globalRules);
};

} // namespace KateAi