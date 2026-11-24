/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "../types.h"

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace KateAi
{
class ProjectGraph;
}

namespace KateAi
{

/**
 * Graduated context compression.
 *
 * The legacy ContextManager applies one summary when the history no longer
 * fits. This engine applies increasingly invasive shapers and stops as soon as
 * the request fits:
 *
 *   1. tool results over budget go to the content store and become a preview;
 *   2. old tool output is pruned to one line (failures and recent output stay);
 *   3. the oldest turns are snipped while the first request and the recent tail
 *      are preserved;
 *   4. the dropped span is merged into an incremental summary, so a second
 *      compaction keeps the first one's decisions;
 *   5. full compaction is the last resort.
 *
 * Rehydration then re-attaches root instructions, the plan, modified files,
 * running tasks, and invoked skills within explicit caps.
 */
struct ContextBudget {
    int window = 32768;
    int reserve = 8192;
    int usable = 24576;
    // Compact before the request is exactly full: a new turn and its tool
    // results need room too.
    int trigger = 0;
    int keepRecent = 8192;
};

struct ContextOptions {
    int contextWindow = 0;
    int reserveForResponse = 0;
    int keepRecentTokens = 0;
    int compactionLevel = 0;
    bool autoCompact = true;
    bool summarizeEverything = false;
    // Reports can inspect the request without writing the content store.
    bool allowContentStore = true;
    double tokenFactor = 1.0;
    ProjectGraph *projectGraph = nullptr;

    // Per-tool result caps supplied by the registry. The engine still uses a
    // conservative default for tools that are not in the map.
    QHash<QString, int> toolResultBudgets;

    // The summary produced by an earlier compaction. Passing it back in is
    // what makes the merge incremental instead of lossy.
    QString previousSummary;

    // Rehydration sources.
    QString rootInstructions;
    QString plan;
    QStringList modifiedFiles;
    QStringList runningTasks;
    QString skills;
};

struct ContextResult {
    QList<ChatMessage> messages;
    QString summary;
    QJsonArray shapers;
    QStringList notes;
    int beforeTokens = 0;
    int afterTokens = 0;
    bool compacted = false;
};

class ContextEngine
{
public:
    // Per-tool result cap applied while building a request. The capture path
    // already stores very large output; this catches output that accumulated
    // before the flag existed or arrived through a provider.
    static constexpr int ToolResultPreviewChars = 24000;
    // Older tool output is pruned to this many characters once it falls out of
    // the recent window.
    static constexpr int PrunedToolChars = 200;
    static constexpr int SummaryTargetChars = 6000;

    static ContextBudget budgetFor(int window, int reserve, int keepRecentTokens = 0);
    static ContextResult build(const QList<ChatMessage> &history, const ContextOptions &options);

    static QString summaryHeader();
    static bool isSummary(const ChatMessage &message);
    static QString renderManifest(const ContextResult &result, int window);

    // Exposed for tests: the one-line summary of a dropped span.
    static QString summarizeDropped(const QList<ChatMessage> &dropped, int requestChars = 600);
    static QString mergeSummary(const QString &previous, const QString &addition, int maxChars = SummaryTargetChars);

private:
    static int tokensOf(const ChatMessage &message, double factor);
    static int tokensOf(const QList<ChatMessage> &messages, double factor);
};

} // namespace KateAi
