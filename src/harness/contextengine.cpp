/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "contextengine.h"

#include "../contextmanager.h"
#include "../graph/projectgraph.h"
#include "toolresults.h"
#include "tokens.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <utility>
#include <limits>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace
{

QString oneLine(QString text, int limit){
    static const QRegularExpression newlines(QStringLiteral("[\\r\\n]+"));
    text.replace(newlines, u" "_s);
    text = text.simplified();
    if (limit > 0 && text.size() > limit) {
        text = truncateSafe(text, limit) + u"…"_s;
    }
    return text;
}

QString toolNameOf(const ChatMessage &message)
{
    return message.name.isEmpty() ? u"tool"_s : message.name;
}

bool looksLikeFailure(const QString &text)
{
    const QString lower = text.toLower();
    // Native tool messages are formatted with a STATUS line, so the raw
    // "error"/"failed" prefixes are not present in the conversation.
    return lower.startsWith(u"error"_s) || lower.startsWith(u"failed"_s)
        || lower.startsWith(u"tool rejected"_s) || lower.startsWith(u"tool denied"_s)
        || lower.contains(u"status: failure"_s) || lower.contains(u"status: failed"_s)
        || lower.contains(u"tool rejected"_s) || lower.contains(u"tool denied"_s)
        || lower.contains(u"timed out"_s) || lower.contains(u"exit code "_s)
        || lower.contains(u"permission denied"_s);
}

QString messagePath(const ChatMessage &message)
{
    if (message.role != ChatMessage::Role::Assistant) {
        return QString();
    }
    for (const QJsonValue &value : message.toolCalls) {
        const QJsonObject function = value.toObject().value(u"function"_s).toObject();
        QJsonObject arguments = function.value(u"arguments"_s).toObject();
        if (arguments.isEmpty() && function.value(u"arguments"_s).isString()) {
            const QJsonDocument parsed =
                QJsonDocument::fromJson(function.value(u"arguments"_s).toString().toUtf8());
            if (parsed.isObject()) {
                arguments = parsed.object();
            }
        }
        QString path = arguments.value(u"path"_s).toString();
        if (path.isEmpty()) {
            path = arguments.value(u"file_path"_s).toString();
        }
        if (!path.isEmpty()) {
            return path;
        }
    }
    return QString();
}

QString messageToolName(const ChatMessage &message)
{
    for (const QJsonValue &value : message.toolCalls) {
        const QString name =
            value.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
        if (!name.isEmpty()) {
            return name;
        }
    }
    return QString();
}

ChatMessage makeSummaryMessage(const QString &text)
{
    ChatMessage message;
    message.role = ChatMessage::Role::User;
    message.name = u"context_summary"_s;
    message.content = text;
    return message;
}

int firstNonSystem(const QList<ChatMessage> &messages)
{
    int index = 0;
    while (index < messages.size() && messages.at(index).role == ChatMessage::Role::System) {
        ++index;
    }
    return index;
}

// A cut must not split an assistant tool-call message from its tool results.
int safeCutForward(const QList<ChatMessage> &messages, int index)
{
    int cut = qBound(0, index, messages.size());
    while (cut < messages.size() && messages.at(cut).role == ChatMessage::Role::Tool) {
        ++cut;
    }
    // Starting on an assistant that still has tool calls would orphan the
    // calls; skip the whole group.
    while (cut < messages.size() && messages.at(cut).role == ChatMessage::Role::Assistant
           && !messages.at(cut).toolCalls.isEmpty()) {
        ++cut;
        while (cut < messages.size() && messages.at(cut).role == ChatMessage::Role::Tool) {
            ++cut;
        }
    }
    return cut;
}

int latestUserIndex(const QList<ChatMessage> &messages, int from)
{
    for (int i = messages.size() - 1; i >= from; --i) {
        if (messages.at(i).role == ChatMessage::Role::User && !ContextEngine::isSummary(messages.at(i))) {
            return i;
        }
    }
    return -1;
}

} // namespace

QString ContextEngine::summaryHeader()
{
    return u"[Earlier conversation was compacted to fit the context window. This incremental summary "_s
           u"preserves decisions, user requests, changed files, tool use, and failures. Full tool output "_s
           u"lives in the content store and can be paged with read_tool_result.]"_s;
}

namespace
{

// Both engines have historically produced summaries. A summary created by the
// legacy ContextManager must still be recognised here, or the next contextV2
// turn treats it as a user request and summarizes it again.
QString summaryBody(QString text)
{
    if (text.startsWith(ContextEngine::summaryHeader())) {
        return text.mid(ContextEngine::summaryHeader().size()).trimmed();
    }
    if (text.startsWith(ContextManager::summaryHeader())) {
        return text.mid(ContextManager::summaryHeader().size()).trimmed();
    }
    return text.trimmed();
}

QString canonicalSummary(const QString &text)
{
    const QString body = summaryBody(text);
    if (body.isEmpty()) {
        return QString();
    }
    return ContextEngine::summaryHeader() + u"\n\n"_s + body;
}

} // namespace

bool ContextEngine::isSummary(const ChatMessage &message)
{
    return message.role == ChatMessage::Role::User
        && (message.name == u"context_summary"_s
            || message.content.startsWith(summaryHeader())
            || message.content.startsWith(ContextManager::summaryHeader()));
}

ContextBudget ContextEngine::budgetFor(int window, int reserve, int keepRecentTokens)
{
    ContextBudget budget;
    budget.window = window > 0 ? window : 32768;
    // A reserve larger than the window would make every request overflow.
    // Keep half the window available for the prompt and cap the reserve.
    const int requestedReserve = reserve > 0 ? reserve : 8192;
    budget.reserve = qMax(512, qMin(requestedReserve, qMax(512, budget.window / 2)));
    budget.usable = qMax(512, budget.window - budget.reserve);
    // Leave a full new turn of headroom, but never compact before 10% of the
    // window: a tiny model would otherwise compact on every step.
    budget.trigger = qMax(budget.window / 10, budget.usable - 13000);
    const int defaultKeep = qMax(1024, budget.usable / 3);
    budget.keepRecent = keepRecentTokens > 0
        ? qMin(keepRecentTokens, budget.usable)
        : qMin(defaultKeep, budget.usable);
    return budget;
}

int ContextEngine::tokensOf(const ChatMessage &message, double factor)
{
    qint64 total = 4 + Tokens::estimate(message.content) + Tokens::estimate(message.thinking);
    for (const QJsonValue &value : message.toolCalls) {
        total += Tokens::estimate(
            QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)));
    }
    total = qMin<qint64>(total, std::numeric_limits<int>::max());
    return Tokens::applyFactor(static_cast<int>(total), factor);
}

int ContextEngine::tokensOf(const QList<ChatMessage> &messages, double factor)
{
    qint64 total = 0;
    for (const ChatMessage &message : messages) {
        total += tokensOf(message, factor);
        if (total > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
    }
    return static_cast<int>(total);
}

QString ContextEngine::summarizeDropped(const QList<ChatMessage> &dropped, int requestChars)
{
    QStringList requests;
    QStringList files;
    QSet<QString> fileSet;
    QStringList tools;
    QStringList outcomes;
    QStringList failures;
    QStringList storedResults;
    int rejected = 0;

    for (const ChatMessage &message : dropped) {
        switch (message.role) {
        case ChatMessage::Role::User: {
            if (isSummary(message) || message.content.startsWith(QLatin1String("[KateAI agent controller]"))) {
                break;
            }
            const QString line = oneLine(message.content, requestChars);
            if (!line.isEmpty() && !requests.contains(line)) {
                requests.append(line);
            }
            break;
        }
        case ChatMessage::Role::Assistant: {
            const QString path = messagePath(message);
            if (!path.isEmpty() && !fileSet.contains(path)) {
                fileSet.insert(path);
                files.append(path);
            }
            const QString tool = messageToolName(message);
            if (!tool.isEmpty() && !tools.contains(tool)) {
                tools.append(tool);
            }
            if (!message.content.trimmed().isEmpty()) {
                outcomes.append(oneLine(message.content, 300));
            }
            break;
        }
        case ChatMessage::Role::Tool: {
            const QString text = message.content.trimmed();
            {
                static const QRegularExpression idPattern(uR"(tool_result id=([A-Za-z0-9-]+))"_s);
                const QRegularExpressionMatch match = idPattern.match(message.content);
                if (match.hasMatch() && !storedResults.contains(match.captured(1))) {
                    storedResults.append(match.captured(1));
                }
            }
            const QString lower = text.toLower();
            if (lower.startsWith(QLatin1String("tool rejected"))
                || lower.startsWith(QLatin1String("tool denied"))
                || lower.contains(QLatin1String("tool rejected"))
                || lower.contains(QLatin1String("tool denied"))) {
                ++rejected;
            }
            if (looksLikeFailure(text)) {
                failures.append(oneLine(text, 240));
                while (failures.size() > 4) {
                    failures.removeFirst();
                }
            }
            break;
        }
        case ChatMessage::Role::System:
            break;
        }
    }

    QStringList lines;
    if (!requests.isEmpty()) {
        lines.append(u"Requests:"_s);
        for (int i = 0; i < requests.size(); ++i) {
            lines.append(u"  %1. %2"_s.arg(i + 1).arg(requests.at(i)));
        }
    }
    if (!files.isEmpty()) {
        QStringList shown = files;
        if (shown.size() > 40) {
            shown = shown.mid(0, 40);
            shown.append(u"… (%1 more)"_s.arg(files.size() - 40));
        }
        lines.append(u"Files touched: "_s + shown.join(u", "_s));
    }
    if (!tools.isEmpty()) {
        lines.append(u"Tools used: "_s + tools.join(u", "_s));
    }
    while (outcomes.size() > 4) {
        outcomes.removeFirst();
    }
    if (!outcomes.isEmpty()) {
        lines.append(u"Where it got to:"_s);
        for (const QString &outcome : std::as_const(outcomes)) {
            lines.append(u"  - "_s + outcome);
        }
    }
    if (!failures.isEmpty()) {
        lines.append(u"Recent errors or constraints:"_s);
        for (const QString &failure : std::as_const(failures)) {
            lines.append(u"  - "_s + failure);
        }
    }
    if (rejected > 0) {
        lines.append(u"%1 tool call(s) were rejected or denied; do not assume they ran."_s.arg(rejected));
    }
    if (!storedResults.isEmpty()) {
        lines.append(u"Stored tool results still pageable with read_tool_result:"_s);
        for (const QString &id : std::as_const(storedResults)) {
            lines.append(u"  - id="_s + id);
        }
    }
    return lines.join(u'\n');
}

QString ContextEngine::mergeSummary(const QString &previous, const QString &addition, int maxChars)
{
    QString body = summaryBody(previous);
    if (!addition.trimmed().isEmpty()) {
        // Put the update label at the head of the merged body so the
        // head/tail truncation below cannot cut it out. The first summary is
        // just the summary; only later merges are "updates".
        body = body.trimmed().isEmpty()
            ? addition.trimmed()
            : u"Update after further compaction:\n"_s + body.trimmed()
                + u"\n\n"_s + addition.trimmed();
    }
    if (body.size() > maxChars) {
        // Keep a head and a tail: the first request and the pinned decisions
        // are at the top, while the latest update is at the bottom. Dropping
        // the head made a second compaction forget why the work started.
        const QString marker = u"\n[… middle of the incremental summary truncated …]\n"_s;
        const int room = qMax(0, maxChars - marker.size());
        const int head = room * 2 / 5;
        const int tail = room - head;
        QString tailText = body.right(tail);
        if (!tailText.isEmpty() && tailText.at(0).isLowSurrogate()) {
            tailText.remove(0, 1);
        }
        body = truncateSafe(body, head) + marker + tailText;
    }
    return summaryHeader() + u"\n\n"_s + body.trimmed();
}

QString ContextEngine::renderManifest(const ContextResult &result, int window)
{
    QStringList shapers;
    for (const QJsonValue &value : result.shapers) {
        shapers.append(value.toString());
    }
    QString text;
    text += u"Context engine: " + QString::number(result.beforeTokens) + u" → "_s
        + QString::number(result.afterTokens) + u" tokens (window "_s + QString::number(window)
        + u")\n"_s;
    text += u"Shapers: "_s + (shapers.isEmpty() ? u"none"_s : shapers.join(u", "_s)) + u"\n"_s;
    if (!result.summary.isEmpty()) {
        text += u"Incremental summary: "_s + QString::number(result.summary.size() / 4)
            + u" tokens\n"_s;
    }
    for (const QString &note : result.notes) {
        text += u"- "_s + note + u"\n"_s;
    }
    return text;
}

ContextResult ContextEngine::build(const QList<ChatMessage> &history, const ContextOptions &options)
{
    ContextResult result;
    result.beforeTokens = tokensOf(history, options.tokenFactor);
    result.messages = history;

    const ContextBudget budget = budgetFor(
        options.contextWindow, options.reserveForResponse, options.keepRecentTokens);
    const bool wantCompaction = options.summarizeEverything || options.compactionLevel > 0
        || (options.autoCompact && result.beforeTokens > budget.trigger);
    if (!wantCompaction) {
        result.afterTokens = result.beforeTokens;
        return result;
    }

    auto addShaper = [&result](const QString &id) {
        if (!result.shapers.contains(QJsonValue(id))) {
            result.shapers.append(id);
        }
    };

    const int bodyStart = firstNonSystem(result.messages);

    // The previous summary is folded into the new one, never duplicated in the
    // transcript.
    QString previousSummary = options.previousSummary;
    QList<ChatMessage> working;
    for (int i = 0; i < result.messages.size(); ++i) {
        if (isSummary(result.messages.at(i))) {
            if (previousSummary.isEmpty()) {
                previousSummary = canonicalSummary(result.messages.at(i).content);
            }
            continue;
        }
        working.append(result.messages.at(i));
    }

    // Shaper 1: any single tool result over the cap is paged to the content
    // store. This also covers output that was appended before the capture path
    // learned to budget it.
    for (ChatMessage &message : working) {
        if (!options.allowContentStore || message.role != ChatMessage::Role::Tool) {
            continue;
        }
        const int toolBudget = qMax(1024, options.toolResultBudgets.value(
            message.name, ToolResultPreviewChars));
        if (message.content.size() <= toolBudget) {
            continue;
        }
        if (message.content.contains(u"tool_result id="_s)) {
            continue;
        }
        const StoredToolResult stored = ToolResults::store(
            message.content, qMax(1000, toolBudget * 2 / 3), qMax(500, toolBudget / 3));
        if (!stored.id.isEmpty()) {
            message.content = stored.preview;
            addShaper(u"tool-result-budget"_s);
            result.notes.append(u"stored a %1-character tool result as %2"_s
                                    .arg(stored.totalChars).arg(stored.id));
        }
    }

    // Measure each message once. The shapers below walk the list several
    // times, and tokensOf() has to serialise every tool-call object to weigh
    // it; doing that again for each shaping pass made a long conversation cost
    // far more than the compaction itself.
    QList<int> workingCosts;
    workingCosts.reserve(working.size());
    qint64 workingTotal = 0;
    for (const ChatMessage &message : std::as_const(working)) {
        const int cost = tokensOf(message, options.tokenFactor);
        workingCosts.append(cost);
        workingTotal += cost;
    }
    int current = static_cast<int>(qMin<qint64>(workingTotal, std::numeric_limits<int>::max()));

    // Shaper 2: prune old tool output, preserving failures and the recent tail.
    if (current > budget.trigger) {
        const int newestUser = latestUserIndex(working, bodyStart);
        int recentFrom = qMax(bodyStart, working.size() - 1);
        int running = 0;
        for (int i = working.size() - 1; i >= bodyStart; --i) {
            running += workingCosts.at(i);
            recentFrom = i;
            if (running >= budget.keepRecent / 2) {
                break;
            }
        }
        if (newestUser >= 0) {
            recentFrom = qMin(recentFrom, newestUser);
        }
        bool pruned = false;
        for (int i = bodyStart; i < recentFrom; ++i) {
            ChatMessage &message = working[i];
            if (message.role != ChatMessage::Role::Tool) {
                continue;
            }
            if (message.content.size() <= PrunedToolChars || looksLikeFailure(message.content)) {
                continue;
            }
            // A paged result's preview is already bounded and carries the id
            // the model needs to page it; pruning it would lose that handle.
            if (message.content.contains(u"tool_result id="_s)) {
                continue;
            }
            const QString first = oneLine(message.content, PrunedToolChars);
            const int before = workingCosts.at(i);
            message.content = u"[old tool output pruned: "_s + toolNameOf(message) + u"] "_s + first;
            workingCosts[i] = tokensOf(message, options.tokenFactor);
            current += workingCosts.at(i) - before;
            pruned = true;
        }
        if (pruned) {
            addShaper(u"prune-tool-outputs"_s);
            result.notes.append(u"pruned old tool output outside the recent window"_s);
            // Pruning only removed text, so older tool-call/result pairs stay
            // valid for the provider.
        }
    }

    // Shaper 3: snip the oldest turns. The first request and the live user
    // turn are always represented; the first request survives in the summary.
    QString summary = previousSummary;
    QList<ChatMessage> kept;
    int cut = bodyStart;
    if (current > budget.trigger || options.summarizeEverything) {
        int keepTokens = options.summarizeEverything ? 0 : budget.keepRecent;
        if (options.compactionLevel > 0 && !options.summarizeEverything) {
            keepTokens = qMax(qMax(800, budget.usable / 10),
                              keepTokens / (1 << qMin(options.compactionLevel, 4)));
        }
        int running = 0;
        cut = working.size();
        for (int i = working.size() - 1; i >= bodyStart; --i) {
            const int cost = workingCosts.at(i);
            if (running + cost > keepTokens && i < working.size() - 1) {
                cut = i + 1;
                break;
            }
            running += cost;
            cut = i;
        }
        cut = qBound(bodyStart, safeCutForward(working, cut), working.size());
        const int newestUser = latestUserIndex(working, bodyStart);
        if (!options.summarizeEverything && newestUser >= bodyStart && cut > newestUser) {
            cut = qBound(bodyStart, newestUser, working.size());
        }
        const QList<ChatMessage> dropped = working.mid(bodyStart, cut - bodyStart);
        if (!dropped.isEmpty()) {
            const QString addition = summarizeDropped(dropped);
            summary = mergeSummary(previousSummary, addition);
            addShaper(u"snip-old-turns"_s);
            result.notes.append(u"snipped %1 messages into the incremental summary"_s
                                    .arg(dropped.size()));
        }
    } else {
        cut = bodyStart;
    }

    for (int i = 0; i < bodyStart; ++i) {
        kept.append(working.at(i));
    }
    if (!summary.trimmed().isEmpty()) {
        kept.append(makeSummaryMessage(summary));
        addShaper(u"incremental-summary"_s);
    }
    for (int i = cut; i < working.size(); ++i) {
        kept.append(working.at(i));
    }

    // Shaper 5: if the preserved tail alone still overflows, fall back to the
    // legacy full compaction, which also trims thinking and bulky output.
    current = tokensOf(kept, options.tokenFactor);
    if (current > budget.usable) {
        ContextManager::Options legacy;
        legacy.contextWindow = budget.window;
        legacy.reserveForResponse = budget.reserve;
        legacy.keepRecentTokens = qMax(800, budget.usable / 10);
        legacy.compactionLevel = qMax(1, options.compactionLevel);
        legacy.autoCompact = true;
        legacy.projectGraph = options.projectGraph;
        legacy.summarizeEverything = options.summarizeEverything;
        kept = ContextManager::build(kept, legacy);
        // The legacy builder may have produced the summary that actually
        // survived. Fold it into the result so the next turn can merge it
        // incrementally instead of dropping it or re-summarizing it.
        for (const ChatMessage &message : std::as_const(kept)) {
            if (isSummary(message)) {
                const QString normalized = canonicalSummary(message.content);
                if (!normalized.isEmpty()) {
                    summary = normalized;
                }
                break;
            }
        }
        addShaper(u"full-compaction"_s);
        result.notes.append(u"fell back to full compaction for the preserved tail"_s);
        current = tokensOf(kept, options.tokenFactor);
    }

    // Rehydration travels as a transient user message so it never becomes part
    // of the persisted conversation and never changes the static prefix.
    QStringList rehydrate;
    if (!options.rootInstructions.trimmed().isEmpty()) {
        rehydrate.append(u"<root_instructions>\n"_s + options.rootInstructions.trimmed()
                         + u"\n</root_instructions>"_s);
    }
    if (!options.plan.trimmed().isEmpty()) {
        rehydrate.append(u"<plan>\n"_s + options.plan.trimmed() + u"\n</plan>"_s);
    }
    if (!options.modifiedFiles.isEmpty()) {
        QStringList files;
        for (const QString &file : options.modifiedFiles) {
            if (files.size() >= 5) {
                break;
            }
            files.append(file);
        }
        rehydrate.append(u"<modified_files>\n"_s + files.join(u'\n') + u"\n</modified_files>"_s);
    }
    if (!options.runningTasks.isEmpty()) {
        QString tasks;
        for (const QString &task : options.runningTasks) {
            const QString line = u"- "_s + task + u"\n"_s;
            if (tasks.size() + line.size() > 25000) {
                break;
            }
            tasks += line;
        }
        if (!tasks.isEmpty()) {
            rehydrate.append(u"<running_tasks>\n"_s + tasks + u"</running_tasks>"_s);
        }
    }
    if (!options.skills.trimmed().isEmpty()) {
        rehydrate.append(u"<invoked_skills>\n"_s + options.skills.left(25000) + u"\n</invoked_skills>"_s);
    }
    if (!rehydrate.isEmpty()) {
        QString body = u"<rehydration>\n"_s + rehydrate.join(u"\n"_s) + u"\n</rehydration>"_s;
        bool rehydrateBodyWasTooLarge = false;
        if (body.size() > 50000) {
            body = truncateSafe(body, 50000);
            rehydrateBodyWasTooLarge = true;
        }
        // Rehydration is inserted after compaction, so it must fit the room
        // that remains. Otherwise a large plan/task list can push a request
        // that had just been compacted straight back over the window.
        const int remaining = qMax(0, budget.usable - current);
        bool bodyTruncated = false;
        if (remaining <= 0) {
            body.clear();
        } else {
            body = Tokens::truncateToTokenBudget(body, remaining, &bodyTruncated);
        }
        if (!body.trimmed().isEmpty()) {
            if (bodyTruncated || rehydrateBodyWasTooLarge) {
                const QString marker =
                    u"\n[… rehydration truncated by the harness budget …]"_s;
                const int markerTokens = Tokens::estimate(marker);
                body = Tokens::truncateToTokenBudget(
                    body, qMax(0, remaining - markerTokens));
                if (!body.trimmed().isEmpty()) {
                    body += marker;
                }
            }
            if (body.trimmed().isEmpty()) {
                body.clear();
            }
        }
        if (!body.trimmed().isEmpty()) {
            ChatMessage message;
            message.role = ChatMessage::Role::User;
            message.name = u"context_rehydration"_s;
            message.transient = true;
            message.content = body;
            const int at = (!kept.isEmpty() && kept.first().role == ChatMessage::Role::System) ? 1 : 0;
            kept.insert(at, message);
            addShaper(u"rehydration"_s);
        }
    }

    result.messages = kept;
    result.summary = summary;
    result.afterTokens = tokensOf(kept, options.tokenFactor);
    result.compacted = result.afterTokens < result.beforeTokens || !summary.isEmpty();
    return result;
}

} // namespace KateAi
