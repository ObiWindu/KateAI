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
    // stop sequences cut the usual ways of explaining oneself short.
    return {u"\n\n\n"_s, u"#"_s, u"//"_s};
}

bool CompletionEngine::isStale(const CompletionPromptContext &request, int cursorLine, int cursorColumn)
{
    return request.cursorLine != cursorLine || request.cursorColumn != cursorColumn;
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

    // Line-based offset computation: walking the lines keeps the snapping
    // rules (prefix starts at a line start, suffix ends at the start of the
    // line after the cursor's own) readable.
    int offset = 0;
    for (int line = 0; line < cursorLine; ++line) {
        offset += text.mid(offset).indexOf(QChar(u'\n'));
        if (offset < 0) {
            return context;
        }
        ++offset;
    }
    const int lineStart = offset;
    const QString lineRest = text.mid(lineStart);
    const int newlineInLine = lineRest.indexOf(QChar(u'\n'));
    const int lineLength = newlineInLine >= 0 ? newlineInLine : lineRest.size();
    const int cursorStart = lineStart + std::min(cursorColumn, lineLength);

    // Prefix: up to prefixChars before the cursor, then snapped forward to
    // the next line start so the budget holds and the prompt never begins in
    // the middle of a line.
    if (prefixChars > 0) {
        int begin = qMax(0, cursorStart - prefixChars);
        if (begin > 0) {
            const int newline = text.indexOf(QChar(u'\n'), begin - 1);
            begin = newline >= 0 && newline + 1 <= cursorStart ? newline + 1 : cursorStart;
        }
        if (begin <= cursorStart) {
            context.prefix = text.mid(begin, cursorStart - begin);
        }
    }

    // Suffix: up to suffixChars after the cursor, snapped forward so the
    // prompt never ends inside a line the model might want to complete.
    if (suffixChars > 0) {
        int end = qMin(text.size(), cursorStart + suffixChars);
        if (end < text.size()) {
            const int newline = text.indexOf(QChar(u'\n'), end);
            end = newline >= 0 ? newline + 1 : text.size();
        }
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
    pruneBudgetWindow();
    return m_requestTimes.size() < qMax(1, m_settings.completionRequestsPerMinute);
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

    QString endpoint;
    QJsonObject body;
    if (fim) {
        endpoint = u"/completions"_s;
        body.insert(u"model"_s, model);
        body.insert(u"prompt"_s, fimPrompt(context, m_settings.provider));
        body.insert(u"stream"_s, false);
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
        body.insert(u"max_tokens"_s,
                    m_settings.maxTokens > 0 ? m_settings.maxTokens : kDefaultCompletionMaxTokens);
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
        body.insert(u"max_tokens"_s,
                    m_settings.maxTokens > 0 ? m_settings.maxTokens : kDefaultCompletionMaxTokens);
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
    recordRequestTime();
    m_reply = m_nam->post(networkRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
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

    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool transportOk = reply->error() == QNetworkReply::NoError;
    reply->deleteLater();

    if (!transportOk || status >= 400) {
        Q_EMIT failed(u"Completion request failed (HTTP %1)."_s.arg(status));
        return;
    }

    // Extract the completion text from every response shape the chat
    // transport can produce: plain JSON (stream:false), the OpenAI
    // non-streaming envelope, the /completions envelope and Anthropic's
    // content block list.
    QString completion;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonArray choices = root.value(u"choices"_s).toArray();
        if (!choices.isEmpty()) {
            const QJsonObject choice = choices.at(0).toObject();
            if (choice.contains(u"message"_s)) {
                completion = choice.value(u"message"_s).toObject().value(u"content"_s).toString();
            } else {
                completion = choice.value(u"text"_s).toString();
            }
        }
        const QJsonArray contentBlocks = root.value(u"content"_s).toArray();
        for (const QJsonValue &value : contentBlocks) {
            if (value.toObject().value(u"type"_s).toString() == u"text"_s) {
                completion += value.toObject().value(u"text"_s).toString();
            }
        }
    } else {
        // A provider that ignored stream:false and answered with SSE. The
        // chat client's line parser already knows the event shapes.
        for (const QByteArray &line : body.split('\n')) {
            bool parsed = false;
            const CompletionChunk chunk = LlmClient::parseSseLine(line, nullptr, &parsed);
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
    // Trailing whitespace is layout noise in an inline note; the model's
    // leading text is the completion itself and is kept verbatim.
    while (completion.endsWith(u'\n') || completion.endsWith(u' ')) {
        completion.chop(1);
    }
    Q_EMIT completed(completion, context);
}

void CompletionEngine::recordRequestTime()
{
    m_requestTimes.append(QDateTime::currentMSecsSinceEpoch());
}

} // namespace KateAi
