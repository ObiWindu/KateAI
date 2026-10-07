/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QList>
#include <QString>

namespace KateAi
{

/**
 * Builds the message list actually sent to the model.
 *
 * A long conversation is compacted to fit the model's window so prompting never
 * stops on a hard message cap. The tail is kept verbatim; older turns become a
 * single summary of what was asked, which files were touched, and which tools
 * ran. Tool calls stay glued to their results, because providers reject an
 * orphaned tool message.
 *
 * If the provider still rejects the request as too large, raise
 * `compactionLevel` and call again: each step keeps less of the tail verbatim
 * and shrinks bulky tool output until the payload fits.
 */
namespace ContextManager
{

struct Options {
    // The model's context window. 0 means "unknown"; a conservative default is
    // used so we compact a little early rather than overflow.
    int contextWindow = 0;
    // Tokens held back for the response. 0 means a modest default fraction of
    // the window, so a long answer cannot push the request over the limit.
    int reserveForResponse = 0;
    // How much of the recent tail is kept verbatim. 0 means half the budget.
    int keepRecentTokens = 0;
    // Extra compaction pressure after a context-window rejection (0 = none).
    int compactionLevel = 0;
    // When true (the default), compact as soon as the history would overflow.
    // When false, still compact rather than send an oversized payload: an
    // overflow fails the request outright.
    bool autoCompact = true;
    // Per-request cap on how much of an old user request survives into the
    // summary. 0 means a sensible default.
    int summaryRequestChars = 0;
};

int estimateTokens(const QString &text);
int estimateTokens(const QList<ChatMessage> &messages);

// A conservative context window for a model name, used when the provider does
// not report one.
int contextWindowFor(const QString &model);

/**
 * Returns a list that fits the budget. Never drops the leading system prompt
 * or the latest user turn. There is no message-count cap: fitting the token
 * window is the only constraint, so prompting cannot be stopped by a hard
 * "max messages" cutoff.
 */
QList<ChatMessage> build(const QList<ChatMessage> &history, const Options &options);

QString summaryHeader();

} // namespace ContextManager

} // namespace KateAi
