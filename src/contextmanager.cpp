/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "types.h"
#include "contextmanager.h"
#include "graph/projectgraph.h"

#include <KLocalizedString>

#include <QJsonDocument>
#include <QRegularExpression>
#include <QDir>
#include <limits>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace ContextManager
{

namespace
{

constexpr int kCharsPerToken = 4;
constexpr int kTokensPerMessageOverhead = 4;
constexpr int kDefaultContextWindow = 32768;
constexpr int kDefaultReserve = 4096;
constexpr int kDefaultSummaryRequestChars = 280;
constexpr int kMinBudget = 512;

int tokensOf(const ChatMessage &message)
{
    // qint64 accumulator then saturate: huge tool output must not wrap to
    // negative and bypass the budget check.
    qint64 total = (qint64)kTokensPerMessageOverhead + estimateTokens(message.content) + estimateTokens(message.thinking);
    for (const QJsonValue &value : message.toolCalls) {
        total += estimateTokens(QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)));
        if (total > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
    }
    return (int)qMin<qint64>(total, std::numeric_limits<int>::max());
}

bool startsToolGroup(const QList<ChatMessage> &history, int index)
{
    if (index <= 0 || index >= history.size()) {
        return false;
    }
    const ChatMessage &message = history.at(index);
    if (message.role == ChatMessage::Role::Tool) {
        return true;
    }
    const ChatMessage &previous = history.at(index - 1);
    return previous.role == ChatMessage::Role::Assistant && !previous.toolCalls.isEmpty()
        && message.role != ChatMessage::Role::User;
}

int safeBoundary(const QList<ChatMessage> &history, int start)
{
    int index = start;
    while (index > 0 && startsToolGroup(history, index)) {
        --index;
    }
    return index;
}

QString oneLine(const QString &text, int limit)
{
    QString flat = text.simplified();
    flat.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral(" "));
    if (limit > 0 && flat.length() > limit) {
        flat = truncateSafe(flat, limit) + QStringLiteral("…");
    }
    return flat;
}

QString shrinkText(const QString &text, int keepChars)
{
    if (keepChars <= 0) {
        return QString();
    }
    if (text.length() <= keepChars) {
        return text;
    }
    // The marker takes room too, so the head and the tail are chosen so that
    // the result stays inside the budget. Otherwise the caller, which loops
    // until the text fits, never converges.
    const QString marker = QStringLiteral("\n[… compacted …]\n");
    const int room = qMax(0, keepChars - marker.size() - 1);
    const int head = qBound(1, room * 55 / 100, room);
    const int tail = qMax(0, room - head);
    if (head + tail + marker.size() + 1 >= text.length()) {
        return truncateSafe(text, keepChars - 1) + QStringLiteral("…");
    }
    return truncateSafe(text, head) + marker + text.right(tail);
}

} // namespace

int estimateTokens(const QString &text)
{
    if (text.isEmpty()) {
        return 0;
    }
    // Better token estimation: use UTF-8 byte count / 4 for non-ASCII, chars / 4 for ASCII
    // This is more accurate for mixed content
    const QByteArray utf8 = text.toUtf8();
    const int byteCount = utf8.size();
    const int charCount = text.length();
    // If mostly ASCII (1 byte per char), use char count. If multibyte, use byte count.
    if (byteCount <= charCount * 1.2) {
        return qMax(1, charCount / 4);
    }
    return qMax(1, byteCount / 4);
}

int estimateTokens(const QList<ChatMessage> &messages)
{
    qint64 total = 0;
    for (const ChatMessage &message : messages) {
        total += tokensOf(message);
        if (total > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
    }
    return (int)total;
}

int contextWindowFor(const QString &model)
{
    // Deliberately no per-model table: it went stale on every model release
    // and could not know about new ones, and an overestimated window fails
    // the request outright. The conservative default errs the safe way (it
    // compacts a little early); settings.contextWindow or the provider
    // catalogue overrides it. test_contextmanager pins this behaviour.
    Q_UNUSED(model)
    return kDefaultContextWindow;
}

QString summaryHeader()
{
    return i18n("[Earlier conversation was compacted to fit the context window. "
                "Your requests and the files touched are preserved; full tool output is not.]");
}

namespace
{

bool isSummary(const ChatMessage &message)
{
    return message.role == ChatMessage::Role::User && message.content.startsWith(summaryHeader());
}

QString buildSummary(const QList<ChatMessage> &dropped, int requestChars, ProjectGraph *projectGraph = nullptr)
{
    QStringList userRequests;
    QStringList files;
    QSet<QString> fileSet;
    QStringList tools;
    QStringList outcomes;
    int rejected = 0;

    for (const ChatMessage &message : dropped) {
        switch (message.role) {
        case ChatMessage::Role::User: {
            if (message.content.startsWith(QLatin1String("[KateAI agent controller]"))) {
                break;
            }
            // An earlier compaction's summary is not a request the user made;
            // folding it in would report the summary itself as an "asked" line
            // and re-summarize it again on every compaction.
            if (isSummary(message)) {
                break;
            }
            const QString line = oneLine(message.content, requestChars);
            if (!line.isEmpty() && !userRequests.contains(line)) {
                userRequests << line;
            }
            break;
        }
        case ChatMessage::Role::Assistant:
            for (const QJsonValue &value : message.toolCalls) {
                const QJsonObject call = value.toObject();
                const QJsonObject function = call.value(u"function"_s).toObject();
                const QString toolName = function.value(u"name"_s).toString();
                // The arguments go over the wire as JSON text, so they are
                // stored as a string; reading that string as an object always
                // yields an empty one and drops the path from the summary.
                const QJsonValue rawArguments = function.value(u"arguments"_s);
                QJsonObject arguments = rawArguments.toObject();
                if (arguments.isEmpty() && rawArguments.isString()) {
                    const QJsonDocument parsed = QJsonDocument::fromJson(rawArguments.toString().toUtf8());
                    if (parsed.isObject()) {
                        arguments = parsed.object();
                    }
                }
                QString path = arguments.value(u"path"_s).toString();
                if (path.isEmpty()) {
                    path = arguments.value(u"TargetFile"_s).toString();
                }
                if (!path.isEmpty()) {
                    if (!fileSet.contains(path)) {
                        fileSet.insert(path);
                        files << path;
                    }
                }
                if (!toolName.isEmpty() && !tools.contains(toolName)) {
                    tools << toolName;
                }
            }
            // The last thing the agent said before the span was dropped is the
            // other half of "where did this get to": the files tell the next
            // turn what was touched, the closing words tell it what was found.
            // Every closing word is collected, because the loop walks the
            // messages from the start: keeping the first two would report the
            // oldest conclusions of the dropped span instead of the newest.
            if (!message.content.trimmed().isEmpty()) {
                outcomes.append(oneLine(message.content, 400));
            }
            break;
        case ChatMessage::Role::Tool:
            if (message.content.startsWith(QLatin1String("Tool rejected"))
                || message.content.startsWith(QLatin1String("Tool denied"))) {
                ++rejected;
            }
            break;
        case ChatMessage::Role::System:
            break;
        }
    }

    QStringList lines;
    lines << summaryHeader();

    if (!userRequests.isEmpty()) {
        lines << QStringLiteral("\nWhat was asked:");
        for (int i = 0; i < userRequests.size(); ++i) {
            lines << QStringLiteral("  %1. %2").arg(i + 1).arg(userRequests.at(i));
        }
    }
    if (!files.isEmpty()) {
        QStringList shown = files;
        if (shown.size() > 60) {
            shown = shown.mid(0, 60);
            shown << QStringLiteral("… (%1 more)").arg(files.size() - 60);
        }
        lines << QStringLiteral("\nFiles touched:");
        lines << QStringLiteral("  %1").arg(shown.join(QStringLiteral(", ")));
    }
    if (projectGraph && projectGraph->getNodeCount() > 0 && !files.isEmpty()) {
        // Add related files from project graph to preserve important context
        QSet<QString> related;
        for (const QString &f : files) {
            if (related.size() >= 30) break;
            // Try to find node by path
            GraphNode *node = projectGraph->getNode(f);
            if (!node) {
                // Try to find by ID or partial match
                const QList<GraphNode*> all = projectGraph->getAllNodes();
                for (GraphNode *n : all) {
                    if (n && (n->path.endsWith(f) || f.endsWith(QFileInfo(n->path).fileName()))) {
                        node = n;
                        break;
                    }
                }
            }
            if (node) {
                // Add dependencies/imports
                const QList<GraphEdge*> edges = projectGraph->getEdgesFrom(node->id);
                for (GraphEdge *e : edges) {
                    if (e && e->targetId != node->id) {
                        GraphNode *t = projectGraph->getNode(e->targetId);
                        if (t && !fileSet.contains(t->path) && !related.contains(t->path)) {
                            related.insert(t->path);
                            if (related.size() >= 30) break;
                        }
                    }
                }
                const QList<GraphEdge*> inEdges = projectGraph->getEdgesTo(node->id);
                for (GraphEdge *e : inEdges) {
                    if (e && e->sourceId != node->id) {
                        GraphNode *s = projectGraph->getNode(e->sourceId);
                        if (s && !fileSet.contains(s->path) && !related.contains(s->path)) {
                            related.insert(s->path);
                            if (related.size() >= 30) break;
                        }
                    }
                }
            }
        }
        if (!related.isEmpty()) {
            QStringList relList = related.values();
            if (relList.size() > 20) {
                relList = relList.mid(0, 20);
                relList << QStringLiteral("…");
            }
            lines << QStringLiteral("\nRelated files (from project graph):");
            lines << QStringLiteral("  %1").arg(relList.join(QStringLiteral(", ")));
        }
    }
    if (!tools.isEmpty()) {
        lines << QStringLiteral("\nTools used: %1").arg(tools.join(QStringLiteral(", ")));
    }
    // Keep the last few outcomes so the compacted summary retains where the work
    // got to. Four is a reasonable balance between fidelity and size.
    while (outcomes.size() > 4) {
        outcomes.removeFirst();
    }
    if (!outcomes.isEmpty()) {
        lines << QStringLiteral("\nWhere it got to:");
        for (int i = 0; i < outcomes.size(); ++i) {
            lines << QStringLiteral("  %1. %2").arg(i + 1).arg(outcomes.at(i));
        }
    }
    if (rejected > 0) {
        lines << QStringLiteral("%1 tool call(s) were rejected or denied; do not assume they ran.")
                     .arg(rejected);
    }
    return lines.join(u'\n');
}

int lastUserIndex(const QList<ChatMessage> &history, int bodyStart)
{
    for (int i = history.size() - 1; i >= bodyStart; --i) {
        if (history.at(i).role == ChatMessage::Role::User) {
            return i;
        }
    }
    return -1;
}

void shrinkToBudget(QList<ChatMessage> &result, int budget, int lastProtected)
{
    int excess = estimateTokens(result) - budget;
    if (excess <= 0) {
        return;
    }

    for (int i = 0; i < result.size(); ++i) {
        if (!isSummary(result.at(i))) {
            continue;
        }
        ChatMessage trimmed = result.at(i);
        // qint64 math: excess (int) * kCharsPerToken overflows before /4 for
        // budgets near INT_MAX. Saturate instead of UB.
        const qint64 cutChars = (qint64)excess * kCharsPerToken;
        const qint64 keep64 = (qint64)trimmed.content.length() - cutChars;
        const int keepChars = qMax(summaryHeader().length() + 8,
                                   (int)qBound<qint64>(0, keep64, (qint64)std::numeric_limits<int>::max()));
        trimmed.content = trimmed.content.left(keepChars);
        result[i] = trimmed;
        break;
    }

    excess = estimateTokens(result) - budget;
    if (excess <= 0) {
        return;
    }

    // Drop hidden reasoning on everything except the protected tail, then
    // shrink bulky tool output. This is how a still-oversized request keeps
    // being sendable instead of aborting the turn.
    for (int pass = 0; pass < 3 && estimateTokens(result) > budget; ++pass) {
        const int keepChars = pass == 0 ? 1200 : (pass == 1 ? 400 : 160);
        for (int i = 0; i < result.size(); ++i) {
            if (i >= lastProtected) {
                continue;
            }
            ChatMessage &message = result[i];
            if (message.role == ChatMessage::Role::System || isSummary(message)) {
                continue;
            }
            if (!message.thinking.isEmpty()) {
                message.thinking.clear();
            }
            if (message.role == ChatMessage::Role::Tool && message.content.length() > keepChars) {
                message.content = shrinkText(message.content, keepChars);
            } else if (message.role == ChatMessage::Role::Assistant && message.content.length() > keepChars * 2) {
                message.content = shrinkText(message.content, keepChars * 2);
            }
        }
    }
}

} // namespace

QList<ChatMessage> resetContext(const QList<ChatMessage> &history, const Options &options)
{
    if (history.isEmpty()) {
        return history;
    }
    Options reset = options;
    reset.summarizeEverything = true;
    return build(history, reset);
}

QList<ChatMessage> build(const QList<ChatMessage> &history, const Options &options)
{
    if (history.isEmpty()) {
        return history;
    }

    const int window = options.contextWindow > 0 ? options.contextWindow : kDefaultContextWindow;
    const int reserve = options.reserveForResponse > 0 ? options.reserveForResponse : kDefaultReserve;
    const int budget = qMax(kMinBudget, window - reserve);
    const int requestChars = options.summaryRequestChars > 0 ? options.summaryRequestChars
                                                             : kDefaultSummaryRequestChars;

    if (!options.autoCompact && options.compactionLevel <= 0 && !options.summarizeEverything) {
        // Auto-compaction is off: send the history unchanged. If it then
        // overflows the window the provider rejects it and the caller's
        // overflow recovery retries with compactionLevel > 0, which compacts
        // here; summarizeEverything is the explicit user-driven reset and is
        // honoured regardless.
        return history;
    }

    if (estimateTokens(history) <= budget && options.compactionLevel <= 0 && !options.summarizeEverything) {
        return history;
    }

    int bodyStart = 0;
    while (bodyStart < history.size() && history.at(bodyStart).role == ChatMessage::Role::System) {
        ++bodyStart;
    }

    const int newestUser = lastUserIndex(history, bodyStart);

    int keepTokens = options.keepRecentTokens;
    if (keepTokens <= 0) {
        keepTokens = qMax(800, budget / 2);
    }
    if (options.summarizeEverything) {
        // Only the live user turn survives verbatim: everything older is
        // information the summary carries instead.
        keepTokens = 0;
    }
    const int pressure = qBound(0, options.compactionLevel, 6);
    if (pressure > 0) {
        // Ensure minimum keepTokens is at least 10% of budget or 800 tokens, whichever is larger
        const int minKeepTokens = qMax(800, budget / 10);
        keepTokens = qMax(minKeepTokens, keepTokens / (1 << qMin(pressure, 4)));
    }

    int keepFrom = history.size();
    int running = 0;
    for (int i = history.size() - 1; i >= bodyStart; --i) {
        const int cost = tokensOf(history.at(i));
        if (running + cost > keepTokens && i < history.size() - 1) {
            keepFrom = i + 1;
            break;
        }
        running += cost;
        keepFrom = i;
    }
    keepFrom = qBound(bodyStart, safeBoundary(history, keepFrom), history.size());

    // The latest user turn is the live request; never fold it into the summary.
    if (newestUser >= bodyStart && keepFrom > newestUser) {
        keepFrom = safeBoundary(history, newestUser);
    }

    QList<ChatMessage> result;
    for (int i = 0; i < bodyStart; ++i) {
        result.append(history.at(i));
    }
    if (keepFrom > bodyStart) {
        ChatMessage summary;
        summary.role = ChatMessage::Role::User;
        summary.content = buildSummary(history.mid(bodyStart, keepFrom - bodyStart), requestChars, options.projectGraph);
        result.append(summary);
    }
    for (int i = keepFrom; i < history.size(); ++i) {
        result.append(history.at(i));
    }

    const int protectedFrom = result.size() - (history.size() - keepFrom);
    shrinkToBudget(result, budget, qMax(bodyStart, protectedFrom));

    // Last resort: if the protected tail alone still overflows, shrink it too
    // rather than fail the request. Keep the newest user message readable.
    if (estimateTokens(result) > budget) {
        int latestUserInResult = -1;
        for (int i = result.size() - 1; i >= 0; --i) {
            if (result.at(i).role == ChatMessage::Role::User && !isSummary(result.at(i))) {
                latestUserInResult = i;
                break;
            }
        }
        for (int i = 0; i < result.size(); ++i) {
            if (result.at(i).role == ChatMessage::Role::System) {
                continue;
            }
            ChatMessage &message = result[i];
            message.thinking.clear();
            const int cap = (i == latestUserInResult) ? qMax(400, budget * kCharsPerToken / 4) : 240;
            if (message.content.length() > cap) {
                message.content = shrinkText(message.content, cap);
            }
        }
    }

    return result;
}

} // namespace ContextManager
} // namespace KateAi
