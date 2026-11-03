/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "completionengine.h"
#include "llmclient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// Completions are deliberately short: a ghost-text suggestion that runs to
// hundreds of lines is unreadable and slow. Providers that honour max_tokens
// stop there; the rest is dropped by the caller's line display anyway.
constexpr int kDefaultCompletionMaxTokens = 256;

QString completionModelFor(const Settings &settings)
{
    // A dedicated small fast model, defaulting to the chat model when empty.
    const QString dedicated = settings.completionModel.trimmed();
    return dedicated.isEmpty() ? modelFor(settings) : dedicated;
}

} // namespace

CompletionEngine::CompletionEngine(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

CompletionEngine::~CompletionEngine()
{
    cancel();
}

QStringList CompletionEngine::stopSequences()
{
    // The model is told to output only the completion, but models drift; the
    // stop sequence cuts the usual way of explaining oneself short. Bare "#"
    // and "//" anchored to a line start were tried here and removed: real
    // completions legitimately contain comment lines, and stopping there cut
    // them mid-thought.
    return {u"\n\n\n"_s};
}

bool CompletionEngine::isStale(const CompletionPromptContext &request, int cursorLine, int cursorColumn,
                               const QString &currentText, const QString &currentPath)
{
    if (request.cursorLine != cursorLine || request.cursorColumn != cursorColumn) {
        return true;
    }
    // The cursor alone is not enough: an undo, a paste or an external reload
    // can change the text under a stationary cursor (and a Save-As changes the
    // path). Compare those too when the caller passes them.
    if (!currentPath.isEmpty() && currentPath != request.path) {
        return true;
    }
    if (!currentText.isEmpty()) {
        // Same line-based offset as buildContext: the text before the cursor
        // must still end with the request's prefix, and the text after it
        // must still start with the request's suffix.
        int offset = 0;
        for (int line = 0; line < cursorLine; ++line) {
            const int newline = currentText.indexOf(QChar(u'\n'), offset);
            if (newline < 0) {
                return true;
            }
            offset = newline + 1;
        }
        const int lineStart = offset;
        const int newlineInLine = currentText.indexOf(QChar(u'\n'), lineStart);
        const int lineLength = newlineInLine >= 0 ? newlineInLine - lineStart
                                                  : currentText.size() - lineStart;
        const int cursorStart = lineStart + std::min(cursorColumn, lineLength);
        if (!currentText.left(cursorStart).endsWith(request.prefix)
            || !currentText.mid(cursorStart).startsWith(request.suffix)) {
            return true;
        }
    }
    return false;
}

CompletionPromptContext CompletionEngine::buildContext(const QString &text, int cursorLine, int cursorColumn,
                                                       const QString &path, const QString &language,
                                                       int prefixChars, int suffixChars)
{
    CompletionPromptContext context;
    context.path = path;
    context.language = language;
    context.cursorLine = cursorLine;
    context.cursorColumn = cursorColumn;
    if (cursorLine < 0 || cursorColumn < 0) {
        return context;
    }

    // Line-based offset computation without mid(): indexOf from an offset
    // scans in place instead of copying the tail per line (O(n), not O(n²)).
    int offset = 0;
    for (int line = 0; line < cursorLine; ++line) {
        const int newline = text.indexOf(QChar(u'\n'), offset);
        if (newline < 0) {
            return context;
        }
        offset = newline + 1;
    }
    const int lineStart = offset;
    const QString lineRest = text.mid(lineStart);
    const int newlineInLine = lineRest.indexOf(QChar(u'\n'));
    const int lineLength = newlineInLine >= 0 ? newlineInLine : lineRest.size();
    const int cursorStart = lineStart + std::min(cursorColumn, lineLength);

    // Prefix: up to prefixChars before the cursor, snapped forward to the
    // next line start so the result never exceeds the budget. On a single
    // long line the snap would empty the prefix; in that case keep the tail
    // of the budget (right) so minified files still complete.
    if (prefixChars > 0) {
        int begin = qMax(0, cursorStart - prefixChars);
        if (begin > 0) {
            const int newline = text.indexOf(QChar(u'\n'), begin);
            if (newline >= 0 && newline < cursorStart) {
                begin = newline + 1;
            }
            // Otherwise keep the budget tail as-is: on a single long line
            // there is no line start ahead, and a newline at or after the
            // cursor is not a line start for the prefix.
        }
        if (begin <= cursorStart) {
            QString raw = text.mid(begin, cursorStart - begin);
            if (raw.size() > prefixChars) {
                raw = raw.right(prefixChars);
            }
            context.prefix = raw;
        }
    }

    // Suffix: up to suffixChars after the cursor, truncated at the budget.
    // Never extended past it: reaching forward to the next newline lets one
    // long line blow the budget.
    if (suffixChars > 0) {
        const int end = qMin(text.size(), cursorStart + suffixChars);
        context.suffix = text.mid(cursorStart, end - cursorStart);
    }
    return context;
}

QList<ChatMessage> CompletionEngine::buildMessages(const CompletionPromptContext &context)
{
    QList<ChatMessage> messages;

    ChatMessage system;
    system.role = ChatMessage::Role::System;
    system.content = u"You are a code completion engine. Continue the code at "
                     "<CURSOR>. Output only the completion text that belongs "
                     "at <CURSOR>: no explanations, no markdown, no repetition "
                     "of the code before the cursor. Keep it short and local "
                     "to the surrounding lines."_s;
    messages.append(system);

    ChatMessage user;
    user.role = ChatMessage::Role::User;
    QString prompt;
    if (!context.language.isEmpty() && context.language != u"None"_s) {
        prompt += u"Language: "_s + context.language + u"\n"_s;
    }
    if (!context.path.isEmpty()) {
        prompt += u"File: "_s + context.path + u"\n"_s;
    }
    prompt += u"\n<BEFORE_CURSOR>\n"_s + context.prefix + u"<CURSOR>\n"_s
        + context.suffix + u"\n<AFTER_CURSOR>\n"_s
        + u"Continue at <CURSOR> with only the completion text.\n"_s;
    user.content = prompt;
    messages.append(user);
    return messages;
}

QString CompletionEngine::fimPrompt(const CompletionPromptContext &context, Provider provider)
{
    // DeepSeek's FIM beta expects its own sentinel markers; the generic
    // OpenAI-style markers are what vLLM, LM Studio and friends translate.
    if (provider == Provider::DeepSeek) {
        return u"<｜fim▁begin｜>"_s + context.prefix
            + u"<｜fim▁hole｜>"_s + context.suffix + u"<｜fim▁end｜>"_s;
    }
    return u"<|fim_prefix|>"_s + context.prefix
        + u"<|fim_suffix|>"_s + context.suffix + u"<|fim_middle|>"_s;
}

void CompletionEngine::pruneBudgetWindow() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (!m_requestTimes.isEmpty() && now - m_requestTimes.first() > 60000) {
        m_requestTimes.removeFirst();
    }
}

bool CompletionEngine::canRequestNow() const
{
    // The budget is enforced here rather than in the UI: an auto-trigger loop
    // must not be able to fire a metered key a hundred times an hour because a
    // dialog was closed.
    if (m_agentBusy) {
        return false;
    }
    // 0 means unlimited (no budget); any other value caps the last minute.
    if (m_settings.completionRequestsPerMinute <= 0) {
        return true;
    }
    pruneBudgetWindow();
    return m_requestTimes.size() < m_settings.completionRequestsPerMinute;
}

void CompletionEngine::cancel()
{
    if (!m_reply) {
        return;
    }
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void CompletionEngine::request(const CompletionPromptContext &context)
{
    // The budget is enforced here rather than in the UI: a trigger loop must
    // not be able to fire a metered key a hundred times an hour because a
    // dialog was closed. Refusing is silent; an over-budget trigger is not
    // an error the user needs to hear about.
    if (!canRequestNow()) {
        return;
    }

    // One in-flight request; a retrigger cancels the previous request.
    cancel();

    const QString key = apiKeyFor(m_settings).trimmed();
    const QString model = completionModelFor(m_settings);
    if (key.isEmpty()) {
        Q_EMIT failed(u"No API key configured for %1."_s.arg(providerLabel(m_settings.provider)));
        return;
    }
    if (model.isEmpty()) {
        Q_EMIT failed(u"No model selected."_s);
        return;
    }

    ApiFormat apiFormat = ApiFormat::OpenAICompatible;
    if (m_settings.provider == Provider::Acp) {
        apiFormat = m_settings.apiFormat;
    } else if (m_settings.provider == Provider::ClaudeCompatible) {
        apiFormat = ApiFormat::AnthropicCompatible;
    }

    // Optional FIM path: providers that expose a fill-in-the-middle
    // completion endpoint get it; everything else uses the chat path, which
    // works everywhere chat works.
    const bool fim = apiFormat == ApiFormat::OpenAICompatible && providerSupportsFim(m_settings.provider);

    // Completions are short by design: cap at the dedicated 256 budget, and
    // never above the chat maxTokens when the user lowered that.
    const int completionMaxTokens = m_settings.maxTokens > 0
        ? qMin(kDefaultCompletionMaxTokens, m_settings.maxTokens)
        : kDefaultCompletionMaxTokens;

    QString endpoint;
    QJsonObject body;
    if (fim) {
        endpoint = u"/completions"_s;
        body.insert(u"model"_s, model);
        body.insert(u"prompt"_s, fimPrompt(context, m_settings.provider));
        body.insert(u"stream"_s, false);
        body.insert(u"max_tokens"_s, completionMaxTokens);
        body.insert(u"stop"_s, QJsonArray::fromStringList(stopSequences()));
    } else if (apiFormat == ApiFormat::AnthropicCompatible) {
        endpoint = u"/v1/messages"_s;
        body.insert(u"model"_s, model);
        // The Messages API takes the system prompt as a top-level parameter.
        QStringList systemParts;
        QString userText;
        for (const ChatMessage &message : buildMessages(context)) {
            if (message.role == ChatMessage::Role::System) {
                systemParts << message.content;
            } else {
                userText = message.content;
            }
        }
        body.insert(u"system"_s, systemParts.join(u"\n\n"_s));
        QJsonArray content;
        QJsonObject block;
        block.insert(u"type"_s, u"text"_s);
        block.insert(u"text"_s, userText);
        content.append(block);
        QJsonArray turns;
        QJsonObject turn;
        turn.insert(u"role"_s, u"user"_s);
        turn.insert(u"content"_s, content);
        turns.append(turn);
        body.insert(u"messages"_s, turns);
        body.insert(u"stream"_s, false);
        // max_tokens is required by the Messages API.
        body.insert(u"max_tokens"_s, completionMaxTokens);
        body.insert(u"stop_sequences"_s, QJsonArray::fromStringList(stopSequences()));
    } else {
        // The default path: OpenAI-compatible chat completions, including the
        // ACP HTTP shapes the chat transport itself uses.
        endpoint = apiFormat == ApiFormat::AcpNative ? u"/acp/v1/chat/completions"_s : u"/chat/completions"_s;
        body.insert(u"model"_s, model);
        QJsonArray jsonMessages;
        for (const ChatMessage &message : buildMessages(context)) {
            QJsonObject obj;
            obj.insert(u"role"_s,
                       message.role == ChatMessage::Role::System
                           ? u"system"_s
                           : (message.role == ChatMessage::Role::Assistant ? u"assistant"_s : u"user"_s));
            obj.insert(u"content"_s, message.content);
            jsonMessages.append(obj);
        }
        body.insert(u"messages"_s, jsonMessages);
        body.insert(u"stream"_s, false);
        body.insert(u"stop"_s, QJsonArray::fromStringList(stopSequences()));
        body.insert(u"max_tokens"_s, completionMaxTokens);
    }

    QNetworkRequest networkRequest{QUrl(providerBaseUrl(m_settings) + endpoint)};
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    LlmClient::applyAuthHeaders(networkRequest, apiFormat, key);
    if (m_settings.provider == Provider::OpenRouter) {
        networkRequest.setRawHeader("HTTP-Referer", "https://kate-editor.org");
        networkRequest.setRawHeader("X-Title", "Kate AI");
    }
    // A completion nobody is waiting for anymore must not linger; Kate keeps
    // typing while the request is in flight.
    networkRequest.setTransferTimeout(15000);

    m_activeContext = context;
    m_reply = m_nam->post(networkRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
    // Only a request that actually went out consumes budget: record after a
    // successful post, not before.
    if (!m_reply) {
        Q_EMIT failed(u"Completion request could not be started."_s);
        return;
    }
    recordRequestTime();
    connect(m_reply, &QNetworkReply::finished, this, &CompletionEngine::handleReplyFinished);
}

void CompletionEngine::handleReplyFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply) {
        return;
    }
    m_reply = nullptr;
    const CompletionPromptContext context = m_activeContext;

    // Read everything off the reply first: errorString() below must not run
    // after the reply is scheduled for deletion.
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool transportOk = reply->error() == QNetworkReply::NoError;
    const QString transportError = transportOk ? QString() : reply->errorString();
    reply->deleteLater();

    if (!transportOk || status >= 400) {
        // HTTP 0 means the request never got an answer: report the network
        // error string, not an empty status code.
        const QString detail = status == 0 ? transportError : QString::number(status);
        Q_EMIT failed(detail.isEmpty() ? u"Completion request failed."_s
                                       : u"Completion request failed (%1)."_s.arg(detail));
        return;
    }

    // Extract the completion text from every response shape the chat
    // transport can produce: plain JSON (stream:false), the OpenAI
    // non-streaming envelope, the /completions envelope and Anthropic's
    // content block list. choices wins when both are present; content blocks
    // are the fallback, never an addition.
    QString completion;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    auto appendTextParts = [](const QJsonValue &contentValue) {
        // message.content also arrives in array form (content blocks); join
        // the text parts.
        QString out;
        if (contentValue.isArray()) {
            for (const QJsonValue &part : contentValue.toArray()) {
                const QJsonObject block = part.toObject();
                if (block.value(u"type"_s).toString() == u"text"_s) {
                    out += block.value(u"text"_s).toString();
                } else if (part.isString()) {
                    out += part.toString();
                }
            }
        } else if (contentValue.isString()) {
            out = contentValue.toString();
        }
        return out;
    };
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonArray choices = root.value(u"choices"_s).toArray();
        if (!choices.isEmpty()) {
            const QJsonObject choice = choices.at(0).toObject();
            if (choice.contains(u"message"_s)) {
                completion = appendTextParts(choice.value(u"message"_s).toObject().value(u"content"_s));
            } else {
                completion = choice.value(u"text"_s).toString();
            }
        } else {
            const QJsonArray contentBlocks = root.value(u"content"_s).toArray();
            for (const QJsonValue &value : contentBlocks) {
                if (value.toObject().value(u"type"_s).toString() == u"text"_s) {
                    completion += value.toObject().value(u"text"_s).toString();
                }
            }
        }
    } else {
        // A provider that ignored stream:false and answered with SSE: parse
        // buffered events (blank-line separated), joining the data: lines of
        // each event before parsing, so multi-line payloads survive.
        const QString payload = QString::fromUtf8(body);
        const QStringList events = payload.split(u"\n\n"_s);
        for (const QString &event : events) {
            QStringList dataLines;
            for (const QString &rawLine : event.split(u'\n')) {
                QString line = rawLine.trimmed();
                if (line.endsWith(u'\r')) {
                    line.chop(1);
                }
                if (line.startsWith(u"data:"_s)) {
                    dataLines.append(line.mid(5).trimmed());
                }
            }
            if (dataLines.isEmpty()) {
                continue;
            }
            const QByteArray joined = dataLines.join(u'\n').toUtf8();
            bool parsed = false;
            const CompletionChunk chunk = LlmClient::parseSseLine(joined, nullptr, &parsed);
            if (parsed && !chunk.contentDelta.isEmpty()) {
                completion += chunk.contentDelta;
            }
            if (chunk.finished) {
                break;
            }
        }
    }

    if (completion.trimmed().isEmpty()) {
        Q_EMIT failed(u"The provider returned an empty completion."_s);
        return;
    }
    // Trailing spaces and tabs are layout noise in an inline note; the model's
    // leading text is the completion itself and is kept verbatim. One trailing
    // newline is intentional (the completion ends its line) and is preserved;
    // only the blank lines after it are collapsed away.
    while (completion.endsWith(u' ') || completion.endsWith(u'\t') || completion.endsWith(u'\r')) {
        completion.chop(1);
    }
    int trailingNewlines = 0;
    while (completion.endsWith(u'\n')) {
        completion.chop(1);
        ++trailingNewlines;
    }
    if (trailingNewlines > 0) {
        completion += u'\n';
    }
    Q_EMIT completed(completion, context);
}

void CompletionEngine::recordRequestTime()
{
    // Prune here too, not just on the check path: with the budget disabled
    // (unlimited) nothing ever prunes and the list grows without bound.
    pruneBudgetWindow();
    m_requestTimes.append(QDateTime::currentMSecsSinceEpoch());
}

} // namespace KateAi
