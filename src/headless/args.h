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
 * Command-line options for kateai-headless.
 *
 * Parsing is pure and lives in args.cpp so the CLI contract can be tested
 * without starting a model. Unknown options are an error; a typo must not
 * silently change a permission mode.
 */
struct HeadlessOptions {
    QString mode = QStringLiteral("run"); // run | replay | acp
    QString prompt;
    QString workspace;
    QString outputDir;
    QString provider;
    QString baseUrl;
    QString model;
    QString apiKey;
    QString permission = QStringLiteral("auto"); // ask | accept-edits | auto | bypass
    QString sandbox = QStringLiteral("workspace");
    QString modeId;                              // agent mode
    QString conversationId;
    QString answer;
    int maxTurns = 0;
    double maxCostUsd = 0.0;
    bool verify = true;
    bool quiet = false;
    bool jsonl = false;
    bool persist = true;
    bool stdio = false;
    bool help = false;
    bool version = false;

    static QString usage();
};

class HeadlessArgs
{
public:
    static bool parse(const QStringList &arguments, HeadlessOptions *options, QString *error);
};

} // namespace KateAi
