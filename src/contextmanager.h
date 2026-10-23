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
    // When true (the default), compact as soon as the history would overflow
    // the window. When false, the history is sent unchanged; compaction then
    // only happens after the provider has rejected an oversized request (the
    // caller retries with compactionLevel > 0) or when summarizeEverything
    // asks for it.
    bool autoCompact = true;
    // Per-request cap on how much of an old user request survives into the
    // summary. 0 means a sensible default.
    int summaryRequestChars = 0;
    // When true, the entire conversation except the system prompt and the live
    // user turn is folded into the summary. This is what the user-facing
    // "reset context" action uses: it is how a full context window is reclaimed
    // without losing what the work so far was about.
    bool summarizeEverything = false;
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

/**
 * Folds the whole conversation into one summary, keeping only the system prompt
 * and the most recent user turn verbatim. This is the user-driven counterpart
 * to build(): when the context window has filled up, it reclaims all of it at
 * once instead of waiting for a request to fail, while retaining what the work
 * so far was about — the requests, the files touched, the tools used and where
 * the conversation had got to.
 */
QList<ChatMessage> resetContext(const QList<ChatMessage> &history, const Options &options);

QString summaryHeader();

} // namespace ContextManager

} // namespace KateAi
