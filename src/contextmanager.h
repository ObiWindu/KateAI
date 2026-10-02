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
 * The agent used to hand its entire history to the provider on every request.
 * Nothing measured it, nothing trimmed it, and nothing compacted it, so a long
 * conversation kept growing until the request either failed for exceeding the
 * context window or was silently truncated by the provider — in both cases the
 * model quietly lost the beginning of the conversation, usually without saying
 * so.
 *
 * Four settings claimed to control this (contextWindow, maxContextMessages,
 * smartContextTruncation, compressOldMessages, contextWindowReserve,
 * compressionThreshold) and none of them was read anywhere. This module is
 * where they now actually take effect.
 */
namespace ContextManager
{

// Knobs, all of which map onto settings the user already has.
struct Options {
    // The model's context window. 0 means "unknown", and a conservative
    // per-model default is used instead.
    int contextWindow = 0;
    // Tokens held back for the response, so a long answer cannot push the
    // request over the limit.
    int reserveForResponse = 4096;
    // How much of the recent tail is kept verbatim. Everything older than this
    // is compacted.
    int keepRecentTokens = 16000;
    // If non-zero, an absolute cap on how many messages are sent at all.
    int maxMessages = 0;
    // Per-request cap on how much of an old user request survives into the
    // summary.
    int summaryRequestChars = 200;
};

// Rough token estimate. Deliberately simple: the alternative is a real
// tokenizer per model, which is not worth the dependency for a budget that only
// needs to be roughly right.
int estimateTokens(const QString &text);
int estimateTokens(const QList<ChatMessage> &messages);

// A conservative context window for a model name, used when the provider does
// not report one. Unknown models get a safe middle-of-the-road value rather
// than an optimistic one, because overshooting fails the request outright while
// undershooting merely compacts a little early.
int contextWindowFor(const QString &model);

/**
 * Returns a list that fits the budget.
 *
 * The tail is preserved verbatim; older turns are replaced by a single
 * summary message that keeps the user's requests, the files that were touched,
 * and whether each tool succeeded. A tool call is never separated from its
 * result, because providers reject a history containing an orphaned result and
 * the model reads it as a missing action.
 */
QList<ChatMessage> build(const QList<ChatMessage> &history, const Options &options);

// The compacted-history marker, exposed for tests and for the transcript.
QString summaryHeader();

} // namespace ContextManager

} // namespace KateAi