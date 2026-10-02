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

// ~4 characters per token is the usual English-text approximation, plus a few
// tokens of framing per message. A real per-model tokenizer is not worth the
// dependency for a budget that only has to be roughly right.
constexpr int kCharsPerToken = 4;
constexpr int kTokensPerMessageOverhead = 4;

// Used when the model is unknown. Deliberately modest: overshooting fails the
// request outright, while undershooting merely compacts a little early.
constexpr int kDefaultContextWindow = 32768;

int tokensOf(const ChatMessage &message)
{
    int total = kTokensPerMessageOverhead + estimateTokens(message.content) + estimateTokens(message.thinking);
    for (const QJsonValue &value : message.toolCalls) {
        total += estimateTokens(QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)));
    }
    return total;
}

// A tool result without the call that produced it makes providers reject the
// request, so the tail never starts inside a tool group.
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
    // No model-name table on purpose. Hard-coding windows per model goes stale
    // every time a provider ships one, and a wrong guess here either overflows
    // the window or compacts needlessly. The window comes from settings
    // (user-supplied) or from the provider's catalogue; this only supplies a
    // safe default for the case where neither has told us.
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

// Builds the compacted replacement for the dropped span.
QString buildSummary(const QList<ChatMessage> &dropped, int requestChars)
{
    QStringList userRequests;
    QStringList files;
    QStringList tools;
    int rejected = 0;

    for (const ChatMessage &message : dropped) {
        switch (message.role) {
        case ChatMessage::Role::User: {
            // Controller messages are our own scaffolding, not user intent.
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
        // Saying so explicitly stops the model assuming a rejected edit landed.
        lines << QStringLiteral("%1 tool call(s) were rejected or denied; do not assume they ran.")
                     .arg(rejected);
    }
    return lines.join(u'\n');
}

bool isSummary(const ChatMessage &message)
{
    return message.role == ChatMessage::Role::User && message.content.startsWith(summaryHeader());
}

} // namespace

QList<ChatMessage> build(const QList<ChatMessage> &history, const Options &options)
{
    if (history.isEmpty()) {
        return history;
    }

    const int window = options.contextWindow > 0 ? options.contextWindow : kDefaultContextWindow;
    const int budget = qMax(1024, window - qMax(0, options.reserveForResponse));

    auto applyMessageCap = [&options](QList<ChatMessage> list) {
        if (options.maxMessages > 0 && list.size() > options.maxMessages) {
            list = list.mid(list.size() - options.maxMessages);
        }
        return list;
    };

    // A conversation that already fits is sent untouched: compacting it would
    // throw away detail for nothing.
    if (estimateTokens(history) <= budget) {
        return applyMessageCap(history);
    }

    // Leading system messages are the instructions the turn depends on and are
    // never compacted.
    int bodyStart = 0;
    while (bodyStart < history.size() && history.at(bodyStart).role == ChatMessage::Role::System) {
        ++bodyStart;
    }

    // Keep the newest tail verbatim until the verbatim budget is spent.
    int keepFrom = history.size();
    int running = 0;
    for (int i = history.size() - 1; i >= bodyStart; --i) {
        const int cost = tokensOf(history.at(i));
        if (running + cost > options.keepRecentTokens) {
            keepFrom = i + 1;
            break;
        }
        running += cost;
        keepFrom = i;
    }
    keepFrom = qBound(bodyStart, safeBoundary(history, keepFrom), history.size());

    QList<ChatMessage> result;
    for (int i = 0; i < bodyStart; ++i) {
        result.append(history.at(i));
    }
    if (keepFrom > bodyStart) {
        ChatMessage summary;
        // A user turn, because providers reject unknown roles in some positions
        // and the model reads it as history rather than as a new request.
        summary.role = ChatMessage::Role::User;
        summary.content = buildSummary(history.mid(bodyStart, keepFrom - bodyStart), options.summaryRequestChars);
        result.append(summary);
    }
    for (int i = keepFrom; i < history.size(); ++i) {
        result.append(history.at(i));
    }

    // The summary still has to fit; shrink it rather than overflow.
    const int excess = estimateTokens(result) - budget;
    if (excess > 0) {
        for (int i = 0; i < result.size(); ++i) {
            if (!isSummary(result.at(i))) {
                continue;
            }
            ChatMessage trimmed = result.at(i);
            const int keepChars = qMax(0, trimmed.content.length() - excess * kCharsPerToken);
            trimmed.content = trimmed.content.left(keepChars);
            result[i] = trimmed;
            break;
        }
    }

    return applyMessageCap(result);
}

} // namespace ContextManager
} // namespace KateAi