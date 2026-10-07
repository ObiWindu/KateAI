/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "contextmanager.h"

#include <KLocalizedString>

#include <QJsonDocument>
#include <QRegularExpression>

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
    int total = kTokensPerMessageOverhead + estimateTokens(message.content) + estimateTokens(message.thinking);
    for (const QJsonValue &value : message.toolCalls) {
        total += estimateTokens(QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)));
    }
    return total;
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
        flat = flat.left(limit) + QStringLiteral("…");
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
    const int head = qMax(24, keepChars * 55 / 100);
    const int tail = qMax(24, keepChars - head - 40);
    if (head + tail + 40 >= text.length()) {
        return text.left(keepChars) + QStringLiteral("…");
    }
    return text.left(head) + QStringLiteral("\n[… compacted …]\n") + text.right(tail);
}

} // namespace

int estimateTokens(const QString &text)
{
    if (text.isEmpty()) {
        return 0;
    }
    return qMax(1, text.length() / kCharsPerToken);
}

int estimateTokens(const QList<ChatMessage> &messages)
{
    int total = 0;
    for (const ChatMessage &message : messages) {
        total += tokensOf(message);
    }
    return total;
}

int contextWindowFor(const QString &model)
{
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

QString buildSummary(const QList<ChatMessage> &dropped, int requestChars)
{
    QStringList userRequests;
    QStringList files;
    QStringList tools;
    int rejected = 0;

    for (const ChatMessage &message : dropped) {
        switch (message.role) {
        case ChatMessage::Role::User: {
            if (message.content.startsWith(QLatin1String("[KateAI agent controller]"))) {
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
                const QString path = function.value(u"arguments"_s).toObject().value(u"path"_s).toString();
                if (!path.isEmpty() && !files.contains(path)) {
                    files << path;
                }
                if (!toolName.isEmpty() && !tools.contains(toolName)) {
                    tools << toolName;
                }
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
        if (shown.size() > 40) {
            shown = shown.mid(0, 40);
            shown << QStringLiteral("… (%1 more)").arg(files.size() - 40);
        }
        lines << QStringLiteral("\nFiles touched:");
        lines << QStringLiteral("  %1").arg(shown.join(QStringLiteral(", ")));
    }
    if (!tools.isEmpty()) {
        lines << QStringLiteral("\nTools used: %1").arg(tools.join(QStringLiteral(", ")));
    }
    if (rejected > 0) {
        lines << QStringLiteral("%1 tool call(s) were rejected or denied; do not assume they ran.")
                     .arg(rejected);
    }
    return lines.join(u'\n');
}

bool isSummary(const ChatMessage &message)
{
    return message.role == ChatMessage::Role::User && message.content.startsWith(summaryHeader());
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
        const int keepChars = qMax(summaryHeader().length() + 8,
                                   trimmed.content.length() - excess * kCharsPerToken);
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

    if (estimateTokens(history) <= budget && options.compactionLevel <= 0) {
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
    const int pressure = qBound(0, options.compactionLevel, 6);
    if (pressure > 0) {
        keepTokens = qMax(400, keepTokens / (1 << qMin(pressure, 4)));
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
        summary.content = buildSummary(history.mid(bodyStart, keepFrom - bodyStart), requestChars);
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

    Q_UNUSED(options.autoCompact)
    return result;
}

} // namespace ContextManager
} // namespace KateAi
