/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "llmclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QDateTime>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <limits>
#include <optional>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// Idle watchdog for an in-flight completion. Qt treats this as an *idle*
// timeout -- it only fires when no bytes arrive -- so a long generation that
// keeps streaming is never cut short, while a half-open connection (captive
// portal, proxy black hole, sleeping laptop) still ends the turn with a
// retryable TimeoutError instead of hanging forever.
constexpr int kRequestIdleTimeoutMs = 180000;

// Ceiling for backoff saturation, in seconds. Keeps the 64-bit multiply below
// well inside range and is far beyond any sane wait.
constexpr qint64 kRetryDelaySaturation = 1000000000;

// Upper bound on the response bytes retained for error reporting. Error payloads
// are small and sit at the end of the stream (a failed request *is* its error
// body; a mid-stream error is already lifted into m_completionError by the SSE
// parser), so keeping only the tail is enough and bounds memory for long
// generations.
constexpr qsizetype kMaxRetainedResponseBytes = 256 * 1024;

// Patterns that may indicate sensitive data in response bodies
static const QStringList sensitivePatterns = {
    u"api_key"_s, u"apikey"_s, u"api-key"_s, u"access_token"_s,
    u"access-token"_s, u"secret"_s, u"password"_s, u"authorization"_s,
    u"bearer "_s, u"x-api-key"_s, u"x-auth"_s
};

static QString sanitizeResponseBody(const QByteArray &body)
{
    QString text = QString::fromUtf8(body);
    for (const QString &pattern : sensitivePatterns) {
        // Simple redaction - replace values after sensitive keys
        QRegularExpression re(QString(u"(\"%1\"\\s*:\\s*\")([^\"]+)(\")").arg(pattern), QRegularExpression::CaseInsensitiveOption);
        text.replace(re, QString(u"\\1[REDACTED]\\3"));
        re = QRegularExpression(QString(u"('%1'\\s*:\\s*')([^']+)(')").arg(pattern), QRegularExpression::CaseInsensitiveOption);
        text.replace(re, QString(u"\\1[REDACTED]\\3"));
    }
    return text;
}

    // The system prompt for the Anthropic Messages API must be the top-level
    // `system` parameter, never a system-role message: the API only accepts
    // user/assistant turns and requires them to alternate starting with user.
    static QStringList anthropicSystemParts(const QList<ChatMessage> &messages)
    {
        QStringList parts;
        for (const ChatMessage &message : messages) {
            if (message.role == ChatMessage::Role::System && !message.content.trimmed().isEmpty()) {
                parts << message.content;
            }
        }
        return parts;
    }

    // Convert OpenAI-shaped tool definitions
    // ({type:function, function:{name,description,parameters}}) into Anthropic's
    // shape ({name,description,input_schema}), which the Messages API requires.
    static QJsonArray anthropicTools(const QJsonArray &openAiTools)
    {
        QJsonArray tools;
        for (const QJsonValue &value : openAiTools) {
            const QJsonObject fn = value.toObject().value(u"function"_s).toObject();
            const QString name = fn.value(u"name"_s).toString();
            if (name.isEmpty()) {
                continue;
            }
            QJsonObject tool;
            tool.insert(u"name"_s, name);
            const QString description = fn.value(u"description"_s).toString();
            if (!description.isEmpty()) {
                tool.insert(u"description"_s, description);
            }
            QJsonValue schema = fn.value(u"parameters"_s);
            if (!schema.isObject() || schema.toObject().isEmpty()) {
                QJsonObject fallback;
                fallback.insert(u"type"_s, u"object"_s);
                fallback.insert(u"properties"_s, QJsonObject());
                schema = fallback;
            }
            tool.insert(u"input_schema"_s, schema);
            tools.append(tool);
        }
        return tools;
    }

} // namespace

LlmClient::LlmClient(QObject *parent)
    : QObject(parent)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &LlmClient::executeRetry);
}

LlmClient::~LlmClient()
{
    abort();
    m_retryTimer.stop();
}

void LlmClient::abortModelFetches()
{
    const auto replies = m_modelReplies;
    m_modelReplies.clear();
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (!reply) {
            continue;
        }
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

QJsonArray LlmClient::messagesToJson(const QList<ChatMessage> &messages)
{
    QJsonArray out;
    for (const ChatMessage &msg : messages) {
        QJsonObject obj;
        switch (msg.role) {
        case ChatMessage::Role::System:
            obj.insert(u"role"_s, u"system"_s);
            obj.insert(u"content"_s, msg.content);
            break;
        case ChatMessage::Role::User:
            obj.insert(u"role"_s, u"user"_s);
            obj.insert(u"content"_s, msg.content);
            break;
        case ChatMessage::Role::Assistant:
            obj.insert(u"role"_s, u"assistant"_s);
            obj.insert(u"content"_s, msg.content);
            // Hidden reasoning is sent back to the model so it can reference
            // its own earlier analysis without re-deriving it.
            if (!msg.thinking.isEmpty()) {
                obj.insert(u"reasoning_content"_s, msg.thinking);
            }
            if (!msg.plan.isEmpty()) {
                obj.insert(u"plan"_s, msg.plan);
            }
            if (!msg.toolCalls.isEmpty()) {
                obj.insert(u"tool_calls"_s, msg.toolCalls);
            }
            break;
        case ChatMessage::Role::Tool:
            obj.insert(u"role"_s, u"tool"_s);
            obj.insert(u"content"_s, msg.content);
            obj.insert(u"tool_call_id"_s, msg.toolCallId);
            if (!msg.name.isEmpty()) {
                obj.insert(u"name"_s, msg.name);
            }
            break;
        }
        out.append(obj);
    }
    return out;
}

QList<ToolCall> LlmClient::completedToolsFromAccumulator()
{
    return drainToolAccumulator(&m_toolAcc);
}

// The text form of a streamed tool call's "arguments". The OpenAI shape sends
// it as a JSON string, but several OpenAI-compatible servers -- Ollama's /v1
// endpoint, which this plugin points at by default, among them -- deliver it as
// a JSON object instead. QJsonValue::toString() on an object is an empty
// string, so reading it the same way for both shapes drops every argument the
// model sent and hands the tool an empty object: the suggest card comes out
// bodyless and the cards of every other tool show an "Input {}" body.
static QString toolArgumentsText(const QJsonValue &value)
{
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    if (value.isString()) {
        return value.toString();
    }
    // A number or a boolean is a valid argument value, but
    // QJsonValue::toString() returns an empty string for it. The value is
    // written as JSON instead, so the argument survives.
    if (!value.isNull() && !value.isUndefined()) {
        return QString::fromUtf8(QJsonDocument::fromVariant(value.toVariant()).toJson(QJsonDocument::Compact));
    }
    return QString();
}

void LlmClient::applyAuthHeaders(QNetworkRequest &request, ApiFormat format, const QString &key)
{
    if (format == ApiFormat::AnthropicCompatible) {
        request.setRawHeader("x-api-key", key.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
        return;
    }
    request.setRawHeader("Authorization", QString(u"Bearer "_s + key).toUtf8());
}

// Turns the streamed tool-call accumulator into finished ToolCalls. The
// accumulator is keyed by the provider's block/tool index and only ever holds
// the raw JSON text of the arguments, so parsing is shared by the OpenAI
// "[DONE]" and the Anthropic "message_stop" terminators.
QList<ToolCall> LlmClient::drainToolAccumulator(QHash<int, ToolCall> *acc)
{
    QList<ToolCall> tools;
    if (!acc) {
        return tools;
    }
    // Walk the accumulator in index order, not by tool id: ids are arbitrary
    // strings ("call_2" sorts before "call_10"), and the order the provider
    // streamed the calls in is the order the model meant them to run.
    QList<int> keys = acc->keys();
    std::sort(keys.begin(), keys.end());
    tools.reserve(keys.size());
    for (int key : keys) {
        ToolCall tool = acc->value(key);
        if (tool.id.isEmpty()) {
            tool.id = u"call_%1"_s.arg(key);
        }
        if (!tool.argumentsJson.isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument argsDoc = QJsonDocument::fromJson(tool.argumentsJson.toUtf8(), &parseError);
            if (parseError.error == QJsonParseError::NoError && argsDoc.isObject()) {
                tool.arguments = argsDoc.object();
            } else {
                // A truncated stream or a server that mixed the two argument
                // shapes in one call. The message would reach the cards as an
                // empty object, so say so rather than failing silently.
                qWarning().noquote() << u"[LlmClient] unparseable tool arguments for"_s
                                     << tool.name << u":"_s << tool.argumentsJson;
            }
        }
        tools.append(tool);
    }
    acc->clear();
    return tools;
}

CompletionChunk LlmClient::parseSseLine(const QByteArray &line, QHash<int, ToolCall> *acc, bool *parsed)
{
    CompletionChunk chunk;
    if (parsed) {
        *parsed = false;
    }
    QByteArray payload = line.trimmed();
    if (payload.startsWith("data:")) {
        payload = payload.mid(5).trimmed();
    }
    if (payload.isEmpty()) {
        return chunk;
    }
    if (payload == "[DONE]") {
        if (parsed) {
            *parsed = true;
        }
        chunk.finished = true;
        chunk.finishReason = u"stop"_s;
        if (acc) {
            chunk.completedTools.append(drainToolAccumulator(acc));
        }
        return chunk;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return chunk;
    }
    if (parsed) {
        *parsed = true;
    }

    const QJsonObject root = doc.object();
    if (root.contains(u"error"_s)) {
        const QJsonValue error = root.value(u"error"_s);
        chunk.error = error.isObject() ? error.toObject().value(u"message"_s).toString() : error.toString();
        if (chunk.error.isEmpty()) {
            chunk.error = QString::fromUtf8(payload);
        }
        chunk.finished = true;
        return chunk;
    }

    // Anthropic Messages API: every event carries its own "type" and there is no
    // "choices" envelope, so the OpenAI branch below would silently drop the
    // whole stream. Events arrive as "event: <name>\ndata: {...}", and only the
    // data line is parsed here.
    const QString anthropicType = root.value(u"type"_s).toString();
    if (!anthropicType.isEmpty() && !root.contains(u"choices"_s)) {
        if (anthropicType == u"content_block_delta"_s) {
            const QJsonObject delta = root.value(u"delta"_s).toObject();
            const QString kind = delta.value(u"type"_s).toString();
            if (kind == u"text_delta"_s) {
                chunk.contentDelta = delta.value(u"text"_s).toString();
            } else if (kind == u"thinking_delta"_s) {
                chunk.thinkingDelta = delta.value(u"thinking"_s).toString();
            } else if (kind == u"signature_delta"_s) {
                // A signature is a cryptographic blob, not prose. Showing it as
                // reasoning would put noise in the transcript and store it as
                // the assistant's thinking.
            } else if (kind == u"input_json_delta"_s && acc) {
                // Tool arguments are streamed as JSON fragments; keep them raw
                // so drainToolAccumulator() can parse the assembled object.
                ToolCall &tool = (*acc)[root.value(u"index"_s).toInt()];
                tool.argumentsJson += delta.value(u"partial_json"_s).toString();
            }
        } else if (anthropicType == u"content_block_start"_s) {
            const QJsonObject block = root.value(u"content_block"_s).toObject();
            if (block.value(u"type"_s).toString() == u"tool_use"_s && acc) {
                ToolCall &tool = (*acc)[root.value(u"index"_s).toInt()];
                tool.id = block.value(u"id"_s).toString();
                tool.name = block.value(u"name"_s).toString();
                // A short input can arrive complete here, with no delta events.
                // The protocol makes it an object, but an agent that sends it as
                // a JSON string would otherwise lose every argument to
                // QJsonValue::toObject()'s empty result on a string.
                const QJsonValue input = block.value(u"input"_s);
                if (input.isString() && !input.toString().isEmpty()) {
                    tool.argumentsJson += input.toString();
                } else if (input.isObject()) {
                    tool.arguments = input.toObject();
                }
            }
        } else if (anthropicType == u"message_delta"_s) {
            chunk.finishReason = root.value(u"delta"_s).toObject().value(u"stop_reason"_s).toString();
        } else if (anthropicType == u"message_stop"_s) {
            // Anthropic never sends "data: [DONE]".
            chunk.finished = true;
            if (chunk.finishReason.isEmpty()) {
                chunk.finishReason = u"stop"_s;
            }
            if (acc) {
                chunk.completedTools.append(drainToolAccumulator(acc));
            }
        } else if (anthropicType == u"error"_s) {
            const QJsonValue error = root.value(u"error"_s);
            chunk.error = error.isObject() ? error.toObject().value(u"message"_s).toString() : error.toString();
            if (chunk.error.isEmpty()) {
                chunk.error = QString::fromUtf8(payload);
            }
            chunk.finished = true;
        }
        return chunk;
    }

    const QJsonArray choices = root.value(u"choices"_s).toArray();
    if (choices.isEmpty()) {
        return chunk;
    }

    const QJsonObject choice = choices.at(0).toObject();
    const QJsonObject delta = choice.contains(u"delta"_s)
        ? choice.value(u"delta"_s).toObject()
        : choice.value(u"message"_s).toObject();
    chunk.contentDelta = delta.value(u"content"_s).toString();
    chunk.thinkingDelta = delta.value(u"reasoning_content"_s).toString();
    if (chunk.thinkingDelta.isEmpty()) {
        chunk.thinkingDelta = delta.value(u"reasoning"_s).toString();
    }
    chunk.finishReason = choice.value(u"finish_reason"_s).toString();

    if (acc) {
        const QJsonArray toolCalls = delta.value(u"tool_calls"_s).toArray();
        for (const QJsonValue &value : toolCalls) {
            const QJsonObject obj = value.toObject();
            int index = obj.value(u"index"_s).toInt();
            // A provider that omits "index" would send every parallel tool call
            // as index 0, so they would overwrite each other. Fall back to the
            // next free slot in that case.
            if (!obj.contains(u"index"_s)) {
                // The next free slot. The keys of a hash are not ordered, so
                // the highest one is looked for explicitly.
                int next = 0;
                for (auto it = acc->constBegin(); it != acc->constEnd(); ++it) {
                    next = qMax(next, it.key() + 1);
                }
                index = next;
            }
            ToolCall &tool = (*acc)[index];
            if (obj.contains(u"id"_s) && !obj.value(u"id"_s).toString().isEmpty()) {
                tool.id = obj.value(u"id"_s).toString();
            }
            const QJsonObject fn = obj.value(u"function"_s).toObject();
            if (fn.contains(u"name"_s) && !fn.value(u"name"_s).toString().isEmpty()) {
                tool.name = fn.value(u"name"_s).toString();
            }
            if (fn.contains(u"arguments"_s)) {
                const QJsonValue arguments = fn.value(u"arguments"_s);
                if (arguments.isObject() || arguments.isArray()) {
                    // The whole call arrives in one delta. Replace rather than
                    // append: a server that opens with an empty {} and follows
                    // it with the real arguments would otherwise leave the
                    // accumulated text unparseable.
                    tool.argumentsJson = toolArgumentsText(arguments);
                } else if (arguments.isString()) {
                    const QString fragment = arguments.toString();
                    // A few compatible servers stream an empty object as a
                    // placeholder before sending the real JSON as string
                    // fragments. Treat that placeholder like an empty
                    // accumulator; concatenating it with the first fragment
                    // produces e.g. `{}{\"suggestion\":...}`, which silently
                    // drops the arguments before suggest/question reach the UI.
                    //
                    // The placeholder is only a placeholder while it stands
                    // alone. A "{}" that arrives after real arguments is a
                    // second, complete call and must not overwrite them, so
                    // it is dropped instead of appended.
                    if (fragment.trimmed() == u"{}"_s) {
                        if (tool.argumentsJson.trimmed().isEmpty()) {
                            tool.argumentsJson = fragment;
                        }
                    } else if (tool.argumentsJson.trimmed() == u"{}"_s) {
                        tool.argumentsJson = fragment;
                    } else {
                        tool.argumentsJson += fragment;
                    }
                }
            }
        }
    }

    if (!chunk.finishReason.isEmpty() && chunk.finishReason != u"null"_s) {
        chunk.finished = true;
        if (acc) {
            chunk.completedTools.append(drainToolAccumulator(acc));
        }
    }
    return chunk;
}

void LlmClient::resetCompletionState()
{
    m_buffer.clear();
    m_responseBody.clear();
    m_text.clear();
    m_toolAcc.clear();
    m_completedTools.clear();
    m_completionError.clear();
    m_sawDone = false;
    m_finishEmitted = false;
    // m_abortRequested is deliberately not cleared here: resetCompletionState()
    // runs at the start of every request and right after an abort, and the flag
    // has to survive the abort so the late "finished" signal is recognised as
    // ours. complete() clears it for the next request.
}

void LlmClient::complete(const QList<ChatMessage> &messages)
{
    // Starting a new completion while another one is alive is a programming
    // error in the agent state machine. Never abort the active reply here.
    if (m_reply || m_retryScheduled) {
        Q_EMIT failed(u"A model request is already in progress."_s);
        return;
    }

    // Initialize retry context for a new request
    m_abortRequested = false;
    m_retryContext = RetryContext{};
    m_retryContext.attempt = 0;
    m_retryContext.maxAttempts = qMax(1, m_settings.maxRetryAttempts);
    m_retryContext.baseDelaySeconds = qMax(1, m_settings.baseRetryDelaySeconds);
    // A negative cap would disable capping entirely, so normalise it to "no cap".
    m_retryContext.maxDelaySeconds = qMax(0, m_settings.maxRetryDelaySeconds);
    m_retryContext.strategy = m_settings.retryStrategy;
    m_retryContext.providerName = providerLabel(m_settings.provider);
    m_retryContext.lastErrorCategory = RetryErrorCategory::None;
    m_retryContext.lastErrorMessage.clear();
    m_retryContext.retryAfter = QDateTime();
    m_retryContext.hasProviderRetryAfter = false;

    // Store messages for potential retry
    storeRequestForRetry(messages);

    doComplete(messages);
}

void LlmClient::doComplete(const QList<ChatMessage> &messages)
{
    resetCompletionState();

    const QString key = apiKeyFor(m_settings).trimmed();
    if (key.isEmpty()) {
        Q_EMIT failed(u"No API key configured for %1."_s.arg(providerLabel(m_settings.provider)));
        clearStoredRequest();
        return;
    }

    const QString model = modelFor(m_settings);
    if (model.isEmpty()) {
        Q_EMIT failed(u"No model selected."_s);
        clearStoredRequest();
        return;
    }

    // Determine API format
    ApiFormat apiFormat = ApiFormat::OpenAICompatible;
    if (m_settings.provider == Provider::Acp) {
        apiFormat = m_settings.apiFormat;
    } else if (m_settings.provider == Provider::ClaudeCompatible) {
        apiFormat = ApiFormat::AnthropicCompatible;
    }

    QJsonObject body;
    QString endpoint;
    QByteArray requestBody;
    QString acceptHeader = u"text/event-stream"_s;

    switch (apiFormat) {
    case ApiFormat::AnthropicCompatible:
        body = buildAnthropicRequest(messages, model);
        endpoint = u"/v1/messages"_s;
        requestBody = QJsonDocument(body).toJson(QJsonDocument::Compact);
        acceptHeader = u"text/event-stream"_s;
        break;
    case ApiFormat::AcpNative:
        body = buildAcpNativeRequest(messages, model);
        endpoint = u"/acp/v1/chat/completions"_s;
        requestBody = QJsonDocument(body).toJson(QJsonDocument::Compact);
        acceptHeader = u"text/event-stream"_s;
        break;
    case ApiFormat::OpenAICompatible:
    default:
        body = buildOpenAIRequest(messages, model);
        endpoint = u"/chat/completions"_s;
        requestBody = QJsonDocument(body).toJson(QJsonDocument::Compact);
        acceptHeader = u"text/event-stream"_s;
        break;
    }

    QNetworkRequest request{QUrl(providerBaseUrl(m_settings) + endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    // Idle watchdog: a provider that accepts the connection and then goes quiet
    // would otherwise leave the turn pending forever, because nothing else here
    // sets a deadline. Qt treats this as an *idle* timeout -- it only fires when
    // no bytes arrive -- so a long generation that keeps streaming is never cut
    // short, while a half-open connection still ends with a retryable error.
    request.setTransferTimeout(kRequestIdleTimeoutMs);
    applyAuthHeaders(request, apiFormat, key);
    request.setRawHeader("Accept", acceptHeader.toUtf8());
    if (m_settings.provider == Provider::OpenRouter) {
        request.setRawHeader("HTTP-Referer", "https://kate-editor.org");
        request.setRawHeader("X-Title", "Kate AI");
    }

    m_reply = m_nam.post(request, requestBody);

    // Watchdog: mirrors the request-level transfer timeout above. On expiry Qt
    // reports TimeoutError, which classifyError() already maps to a retryable
    // NetworkError.
    auto *idleTimer = new QTimer(this);
    idleTimer->setSingleShot(true);
    m_requestTimers.insert(m_reply, idleTimer);
    connect(idleTimer, &QTimer::timeout, this, [this, reply = QPointer<QNetworkReply>(m_reply)] {
        if (!reply || !reply->isRunning()) {
            return;
        }
        stopTimerFor(reply);
        // Detach before aborting: abort() emits finished() synchronously, which
        // would run handleFinished() and report the stall twice. After aborting
        // we re-enter handleFinished() by hand, so the existing classify/retry
        // logic sees a plain network failure and the turn is never left pending.
        reply->disconnect(this);
        reply->abort();
        m_completionError = u"No data received from the provider."_s;
        handleFinished();
    });
    idleTimer->start(kRequestIdleTimeoutMs);

    connect(m_reply, &QNetworkReply::readyRead, this, &LlmClient::handleReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &LlmClient::handleFinished);
}

void LlmClient::appendResponseBody(const QByteArray &chunk)
{
    m_responseBody.append(chunk);
    if (m_responseBody.size() > kMaxRetainedResponseBytes) {
        m_responseBody.remove(0, m_responseBody.size() - kMaxRetainedResponseBytes);
    }
}

void LlmClient::stopTimerFor(QNetworkReply *reply)
{
    const auto it = m_requestTimers.constFind(reply);
    if (it == m_requestTimers.constEnd()) {
        return;
    }
    const QPointer<QTimer> timer = it.value();
    m_requestTimers.erase(it);
    if (timer) {
        timer->stop();
        timer->deleteLater();
    }
}

QJsonArray LlmClient::advertisedTools() const
{
    QJsonArray tools = toolDefinitions(m_toolAccess);
    for (const QJsonValue &tool : m_extraTools) {
        const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
        // An extra tool still has to be permitted by the active mode.
        if (m_toolAccess.isEmpty() || m_toolAccess.allows(name)) {
            tools.append(tool);
        }
    }
    return tools;
}

QJsonObject LlmClient::buildOpenAIRequest(const QList<ChatMessage> &messages, const QString &model)
{
    QJsonObject body;
    body.insert(u"model"_s, model);
    body.insert(u"messages"_s, messagesToJson(messages));
    body.insert(u"tools"_s, advertisedTools());
    // "none" is not "no parallelism": it forbids tool calls altogether, so the
    // agent could not act at all when the user turned batching off. Batching is
    // controlled by parallel_tool_calls instead.
    body.insert(u"tool_choice"_s, u"auto"_s);
    body.insert(u"parallel_tool_calls"_s, m_settings.parallelToolCalls);
    body.insert(u"stream"_s, true);
    body.insert(u"temperature"_s, m_settings.temperature);
    body.insert(u"top_p"_s, m_settings.topP);
    if (m_settings.maxTokens > 0) {
        body.insert(u"max_tokens"_s, m_settings.maxTokens);
    }
    if (m_settings.frequencyPenalty != 0.0) {
        body.insert(u"frequency_penalty"_s, m_settings.frequencyPenalty);
    }
    if (m_settings.presencePenalty != 0.0) {
        body.insert(u"presence_penalty"_s, m_settings.presencePenalty);
    }
    if (!m_settings.reasoningEffort.trimmed().isEmpty()) {
        body.insert(u"reasoning_effort"_s, m_settings.reasoningEffort.trimmed());
    }
    // Enhanced intelligence parameters
    if (m_settings.structuredThinking) {
        body.insert(u"include_reasoning"_s, true);
    }
    if (m_settings.maxThinkingTokens > 0) {
        body.insert(u"max_reasoning_tokens"_s, m_settings.maxThinkingTokens);
    }
    return body;
}

QJsonObject LlmClient::buildAnthropicRequest(const QList<ChatMessage> &messages, const QString &model)
{
    QJsonObject body;
    body.insert(u"model"_s, model);

    // The system prompt is a top-level parameter, never a system-role message.
    const QStringList systemParts = anthropicSystemParts(messages);
    if (!systemParts.isEmpty()) {
        body.insert(u"system"_s, systemParts.join(u"\n\n"_s));
    }

    body.insert(u"messages"_s, messagesToAnthropicJson(messages));
    body.insert(u"stream"_s, true);
    // max_tokens is required by the Messages API; fall back to a sane default.
    const int maxTokens = m_settings.maxTokens > 0 ? m_settings.maxTokens : 4096;
    body.insert(u"max_tokens"_s, maxTokens);

    // Extended thinking is the Messages API counterpart of the OpenAI
    // include_reasoning/max_reasoning_tokens pair; without it the thinking
    // toggle is a no-op on this provider even though the stream parser already
    // handles thinking_delta. While it is on the API requires a budget of at
    // least 1024 tokens that stays below max_tokens, and it pins both sampling
    // knobs to their defaults: a clamped or configured value is rejected.
    const bool thinkingEnabled = m_settings.structuredThinking && m_settings.maxThinkingTokens > 0;
    body.insert(u"temperature"_s, thinkingEnabled ? 1.0 : qBound(0.0, m_settings.temperature, 1.0));
    if (!thinkingEnabled && m_settings.topP > 0.0) {
        body.insert(u"top_p"_s, qBound(0.0, m_settings.topP, 1.0));
    }
    if (thinkingEnabled) {
        QJsonObject thinking;
        thinking.insert(u"type"_s, u"enabled"_s);
        const int ceiling = qMax(1024, maxTokens - 1024);
        thinking.insert(u"budget_tokens"_s, qBound(1024, m_settings.maxThinkingTokens, ceiling));
        body.insert(u"thinking"_s, thinking);
    }

    // Tools must be in Anthropic's {name, input_schema} shape, not OpenAI's.
    // Anthropic has no parallel_tool_calls flag; it always decides on its own
    // which tools to batch, and "none" would forbid tool calls entirely.
    const QJsonArray tools = anthropicTools(advertisedTools());
    if (!tools.isEmpty()) {
        body.insert(u"tools"_s, tools);
        body.insert(u"tool_choice"_s, u"auto"_s);
    }
    return body;
}

QJsonObject LlmClient::buildAcpNativeRequest(const QList<ChatMessage> &messages, const QString &model)
{
    // ACP Native format - similar to OpenAI but with ACP-specific extensions
    QJsonObject body;
    body.insert(u"model"_s, model);
    body.insert(u"messages"_s, messagesToJson(messages));
    body.insert(u"tools"_s, advertisedTools());
    // "none" is not "no parallelism": it forbids tool calls altogether, so the
    // agent could not act at all when the user turned batching off. Batching is
    // controlled by parallel_tool_calls instead.
    body.insert(u"tool_choice"_s, u"auto"_s);
    body.insert(u"parallel_tool_calls"_s, m_settings.parallelToolCalls);
    body.insert(u"stream"_s, true);
    body.insert(u"temperature"_s, m_settings.temperature);
    body.insert(u"top_p"_s, m_settings.topP);
    if (m_settings.maxTokens > 0) {
        body.insert(u"max_tokens"_s, m_settings.maxTokens);
    }
    // ACP-specific fields
    body.insert(u"acp_version"_s, u"1.0"_s);
    return body;
}

QJsonArray LlmClient::messagesToAnthropicJson(const QList<ChatMessage> &messages)
{
    QJsonArray turns;

    // Merge a run of same-role content into a single turn so the Messages API
    // never sees two consecutive turns with the same role (it requires strict
    // user/assistant alternation).
    auto lastRoleIs = [&turns](const QString &role) {
        return !turns.isEmpty() && turns.last().toObject().value(u"role"_s).toString() == role;
    };
    auto appendBlock = [&turns](const QJsonObject &block) {
        if (turns.isEmpty()) {
            return;
        }
        QJsonObject turn = turns.last().toObject();
        QJsonArray content = turn.value(u"content"_s).toArray();
        content.append(block);
        turn.insert(u"content"_s, content);
        turns.replace(turns.size() - 1, turn);
    };
    auto ensureTurn = [&turns, &lastRoleIs](const QString &role) {
        if (lastRoleIs(role)) {
            return;
        }
        QJsonObject turn;
        turn.insert(u"role"_s, role);
        turn.insert(u"content"_s, QJsonArray());
        turns.append(turn);
    };

    for (const ChatMessage &message : messages) {
        switch (message.role) {
        case ChatMessage::Role::System:
            // Emitted as the top-level `system` parameter, not a turn here.
            break;
        case ChatMessage::Role::User: {
            ensureTurn(u"user"_s);
            QJsonObject block;
            block.insert(u"type"_s, u"text"_s);
            block.insert(u"text"_s, message.content);
            appendBlock(block);
            break;
        }
        case ChatMessage::Role::Assistant: {
            // An assistant turn with neither text nor tool calls (a cancelled or
            // aborted response) would serialise as an empty content array, which
            // the Messages API rejects. Skip it entirely.
            if (message.content.trimmed().isEmpty() && message.toolCalls.isEmpty()) {
                break;
            }
            ensureTurn(u"assistant"_s);
            // Hidden reasoning is intentionally dropped: Anthropic requires
            // thinking blocks to round-trip with a signature, which we do not
            // retain, so replaying them would be rejected.
            if (!message.content.trimmed().isEmpty()) {
                QJsonObject block;
                block.insert(u"type"_s, u"text"_s);
                block.insert(u"text"_s, message.content);
                appendBlock(block);
            }
            for (const QJsonValue &value : message.toolCalls) {
                const QJsonObject call = value.toObject();
                const QJsonObject fn = call.value(u"function"_s).toObject();
                QJsonObject block;
                block.insert(u"type"_s, u"tool_use"_s);
                block.insert(u"id"_s, call.value(u"id"_s).toString());
                block.insert(u"name"_s, fn.value(u"name"_s).toString());
                QJsonParseError parseError;
                const QJsonDocument argsDoc = QJsonDocument::fromJson(fn.value(u"arguments"_s).toString().toUtf8(), &parseError);
                block.insert(u"input"_s, parseError.error == QJsonParseError::NoError && argsDoc.isObject()
                        ? QJsonValue(argsDoc.object()) : QJsonValue(QJsonObject()));
                appendBlock(block);
            }
            break;
        }
        case ChatMessage::Role::Tool: {
            // A tool result is only valid directly after the assistant turn that
            // requested it (or as a later block of that same batch). Anything
            // else -- history starting with a tool result, or one arriving after
            // an unrelated user message -- would make the Messages API reject
            // the whole request, and appendBlock() would drop it silently.
            bool validPlacement = false;
            if (!turns.isEmpty()) {
                const QJsonObject last = turns.last().toObject();
                if (last.value(u"role"_s).toString() == u"assistant"_s) {
                    validPlacement = true;
                } else if (last.value(u"role"_s).toString() == u"user"_s) {
                    const QJsonArray blocks = last.value(u"content"_s).toArray();
                    for (const QJsonValue &block : blocks) {
                        if (block.toObject().value(u"type"_s).toString() == u"tool_result"_s) {
                            validPlacement = true;
                            break;
                        }
                    }
                }
            }
            if (!validPlacement) {
                break;
            }
            ensureTurn(u"user"_s);
            QJsonObject block;
            block.insert(u"type"_s, u"tool_result"_s);
            block.insert(u"tool_use_id"_s, message.toolCallId);
            block.insert(u"content"_s, message.content);
            appendBlock(block);
            break;
        }
        }
    }

    // The Messages API requires the first turn to be a user turn. If the
    // conversation happened to begin with assistant content (e.g. a restored
    // session), put a short continuation prompt in front instead.
    if (!turns.isEmpty() && turns.first().toObject().value(u"role"_s).toString() == u"assistant"_s) {
        QJsonObject lead;
        lead.insert(u"role"_s, u"user"_s);
        QJsonArray content;
        QJsonObject block;
        block.insert(u"type"_s, u"text"_s);
        block.insert(u"text"_s, QStringLiteral("Please continue."));
        content.append(block);
        lead.insert(u"content"_s, content);
        QJsonArray shifted;
        shifted.append(lead);
        for (const QJsonValue &value : turns) {
            shifted.append(value);
        }
        turns = shifted;
    }

    return turns;
}

void LlmClient::fetchModels(Provider provider)
{
    Settings providerSettings = m_settings;
    providerSettings.provider = provider;
    const QString key = apiKeyFor(providerSettings).trimmed();
    if (key.isEmpty()) {
        Q_EMIT modelsFailed(provider, u"No API key configured."_s);
        return;
    }

    // Determine API format for model fetching
    ApiFormat apiFormat = ApiFormat::OpenAICompatible;
    if (provider == Provider::Acp) {
        apiFormat = providerSettings.apiFormat;
    } else if (provider == Provider::ClaudeCompatible) {
        apiFormat = ApiFormat::AnthropicCompatible;
    }

    QString modelsEndpoint;
    switch (apiFormat) {
    case ApiFormat::AnthropicCompatible:
        modelsEndpoint = u"/v1/models"_s;
        break;
    case ApiFormat::AcpNative:
        modelsEndpoint = u"/acp/v1/models"_s;
        break;
    case ApiFormat::OpenAICompatible:
    default:
        modelsEndpoint = u"/models"_s;
        break;
    }

    // Respect the configured endpoint, not just the provider default, so a
    // custom Ollama/LocalAI/ACP URL actually takes effect.
    QNetworkRequest request{QUrl(providerBaseUrl(providerSettings) + modelsEndpoint)};
    applyAuthHeaders(request, apiFormat, key);
    request.setHeader(QNetworkRequest::UserAgentHeader, u"Kate AI"_s);
    // A models listing is a single short response; it gets the same idle
    // watchdog so a stalled endpoint cannot hang the settings dialog.
    request.setTransferTimeout(kRequestIdleTimeoutMs);
    if (provider == Provider::OpenRouter) {
        request.setRawHeader("HTTP-Referer", "https://kate-editor.org");
        request.setRawHeader("X-Title", "Kate AI");
    }

    QNetworkReply *reply = m_nam.get(request);
    reply->setParent(this);
    m_modelReplies.append(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, provider]() {
        m_modelReplies.removeAll(reply);
        handleModelsFinished(reply, provider);
    });
}

void LlmClient::abort()
{
    m_retryTimer.stop();
    m_retryScheduled = false;
    abortModelFetches();

    const auto timers = m_requestTimers;
    m_requestTimers.clear();
    for (const QPointer<QTimer> &timer : timers) {
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
    }

    QNetworkReply *reply = m_reply;
    if (!reply) {
        return;
    }

    // Clear our ownership first. Any queued QNetworkReply::finished signal will
    // see a null m_reply and therefore cannot re-enter the agent lifecycle.
    m_reply = nullptr;
    m_abortRequested = true;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    resetCompletionState();
    clearStoredRequest();
}

void LlmClient::reset()
{
    abort();
}

void LlmClient::retryLastRequest()
{
    if (m_storedMessages.isEmpty()) {
        Q_EMIT failed(u"No previous request to retry."_s);
        return;
    }
    if (m_reply || m_retryScheduled) {
        Q_EMIT failed(u"A request is already in progress."_s);
        return;
    }

    // Reset retry context for manual retry - use current settings
    m_retryContext = RetryContext{};
    m_retryContext.attempt = 0;
    m_retryContext.maxAttempts = qMax(1, m_settings.maxRetryAttempts);
    m_retryContext.baseDelaySeconds = qMax(1, m_settings.baseRetryDelaySeconds);
    m_retryContext.maxDelaySeconds = qMax(0, m_settings.maxRetryDelaySeconds);
    m_retryContext.strategy = m_settings.retryStrategy;
    m_retryContext.providerName = providerLabel(m_settings.provider);
    m_retryContext.lastErrorCategory = RetryErrorCategory::None;
    m_retryContext.lastErrorMessage.clear();
    m_retryContext.retryAfter = QDateTime();
    m_retryContext.hasProviderRetryAfter = false;

    doComplete(m_storedMessages);
}

void LlmClient::emitCompletedOnce()
{
    if (m_finishEmitted) {
        return;
    }
    m_finishEmitted = true;
    clearStoredRequest();
    Q_EMIT finished(m_text, m_completedTools);
}

void LlmClient::handleModelsFinished(QNetworkReply *reply, Provider provider)
{
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString networkError = reply->errorString();
    const bool ok = reply->error() == QNetworkReply::NoError;
    reply->deleteLater();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (!ok || parseError.error != QJsonParseError::NoError || !document.isObject()) {
        const QString detail = !ok ? networkError : u"The response was not valid JSON."_s;
        Q_EMIT modelsFailed(provider, u"Could not load models (HTTP %1): %2"_s.arg(status).arg(detail));
        return;
    }

    const QJsonObject root = document.object();
    const QJsonValue apiError = root.value(u"error"_s);
    if (!apiError.isUndefined() && !apiError.isNull()) {
        const QString message = apiError.isObject() ? apiError.toObject().value(u"message"_s).toString() : apiError.toString();
        Q_EMIT modelsFailed(provider, message.isEmpty() ? u"The provider rejected the API key."_s : message);
        return;
    }

    QStringList models;
    
    // Determine API format for parsing models response
    ApiFormat apiFormat = ApiFormat::OpenAICompatible;
    if (provider == Provider::Acp) {
        apiFormat = m_settings.apiFormat;
    } else if (provider == Provider::ClaudeCompatible) {
        apiFormat = ApiFormat::AnthropicCompatible;
    }

    switch (apiFormat) {
    case ApiFormat::AnthropicCompatible:
        // Anthropic returns models in a different format
        for (const QJsonValue &value : root.value(u"data"_s).toArray()) {
            const QString id = value.toObject().value(u"id"_s).toString().trimmed();
            if (!id.isEmpty() && !models.contains(id)) {
                models.append(id);
            }
        }
        break;
    case ApiFormat::AcpNative:
        // ACP Native format - similar to OpenAI
        for (const QJsonValue &value : root.value(u"data"_s).toArray()) {
            const QString id = value.toObject().value(u"id"_s).toString().trimmed();
            if (!id.isEmpty() && !models.contains(id)) {
                models.append(id);
            }
        }
        break;
    case ApiFormat::OpenAICompatible:
    default:
        // OpenAI format
        for (const QJsonValue &value : root.value(u"data"_s).toArray()) {
            const QString id = value.toObject().value(u"id"_s).toString().trimmed();
            if (!id.isEmpty() && !models.contains(id)) {
                models.append(id);
            }
        }
        break;
    }

    models.sort(Qt::CaseInsensitive);
    if (models.isEmpty()) {
        Q_EMIT modelsFailed(provider, u"The provider did not return any available models."_s);
        return;
    }
    Q_EMIT modelsReceived(provider, models);
}

bool LlmClient::applySseChunk(const CompletionChunk &chunk)
{
    if (!chunk.error.isEmpty()) {
        m_completionError = chunk.error;
        if (m_reply) {
            // Detach first: abort() emits finished() synchronously, which
            // would re-enter handleFinished() *inside* this slot with the
            // rest of the stream still in m_buffer. handleFinished() is then
            // called by hand, and the remaining lines are dropped instead
            // of being emitted after the failure was already recorded.
            //
            // The abort makes the reply report OperationCanceledError, which
            // is our doing and not a transport failure. handleFinished()
            // therefore treats the provider error as the real cause, so the
            // message is classified as the provider intended (a context
            // overflow, an invalid key) instead of as a retryable network
            // error.
            m_reply->disconnect(this);
            m_reply->abort();
            handleFinished();
        }
        return false;
    }
    if (!chunk.contentDelta.isEmpty()) {
        m_text += chunk.contentDelta;
        Q_EMIT textDelta(chunk.contentDelta);
    }
    if (!chunk.thinkingDelta.isEmpty()) {
        Q_EMIT thinkingDelta(chunk.thinkingDelta);
    }
    if (!chunk.completedTools.isEmpty()) {
        m_completedTools.append(chunk.completedTools);
    }
    if (chunk.finished) {
        m_sawDone = true;
    }
    return true;
}

bool LlmClient::feedSseLine(const QByteArray &line)
{
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        // End of an event. Anything still buffered never parsed, so the event
        // was truncated: drop it instead of dispatching half a payload.
        m_pendingEventData.clear();
        return true;
    }
    if (trimmed.startsWith("data:")) {
        const QByteArray payload = trimmed.mid(5).trimmed();
        if (payload.isEmpty()) {
            return true;
        }
        // The SSE specification allows one event to be spread over several
        // "data:" lines, which are joined with a newline. The provider sends a
        // single line per event in practice, so the buffer is dispatched as
        // soon as it parses; only a split event stays buffered.
        if (m_pendingEventData.isEmpty()) {
            m_pendingEventData = payload;
        } else {
            m_pendingEventData += '\n';
            m_pendingEventData += payload;
        }
        bool parsed = false;
        const CompletionChunk chunk = parseSseLine(m_pendingEventData, &m_toolAcc, &parsed);
        if (!parsed) {
            return true;
        }
        m_pendingEventData.clear();
        return applySseChunk(chunk);
    }
    // "event:", "id:", "retry:" and comment lines carry no payload. A pending
    // event that never parsed is dropped here: more data will not make it
    // valid.
    m_pendingEventData.clear();
    return true;
}

void LlmClient::handleReadyRead()
{
    if (!m_reply || m_abortRequested) {
        return;
    }

    const QByteArray incoming = m_reply->readAll();
    // Keep the tail, not just what is still buffered: a provider error is
    // reported at the end of the response, long after the trailing
    // unterminated line that `leftover` holds.
    appendResponseBody(incoming);
    m_buffer.append(incoming);
    // The watchdog measures idleness, not total duration: restart it on every
    // chunk so a long but healthy stream is never cut off.
    if (QTimer *idleTimer = m_requestTimers.value(m_reply)) {
        idleTimer->start(kRequestIdleTimeoutMs);
    }
    while (true) {
        const int idx = m_buffer.indexOf('\n');
        if (idx < 0) {
            break;
        }
        const QByteArray line = m_buffer.left(idx);
        m_buffer.remove(0, idx + 1);
        if (line.trimmed().isEmpty()) {
            feedSseLine(line);
            continue;
        }

        if (!feedSseLine(line)) {
            return;
        }
    }
}

void LlmClient::handleFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply) {
        return;
    }

    // Detach the reply before emitting anything. AgentLoop may immediately
    // schedule the next turn, and complete() must observe a fully idle client.
    m_reply = nullptr;
    reply->disconnect(this);
    stopTimerFor(reply);

    const QByteArray tail = reply->readAll();
    appendResponseBody(tail);
    const QByteArray leftover = m_buffer + tail;
    QNetworkReply::NetworkError networkError = reply->error();
    // An SSE "error" event aborts the reply on purpose (see onReadyRead), so
    // the resulting OperationCanceledError is our own doing. It must not be
    // classified as a transport failure: the provider's message is the cause.
    if (!m_completionError.isEmpty() && networkError == QNetworkReply::OperationCanceledError) {
        networkError = QNetworkReply::NoError;
    }
    const QString errorString = reply->errorString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // Read the provider's backoff hint while the reply is still alive. It is
    // deleted at the end of this block, and rawHeader() on a deleted reply is a
    // use-after-free.
    const std::optional<int> headerRetryAfter = parseRetryAfterHeader(reply);
    reply->deleteLater();

    if (m_abortRequested) {
        resetCompletionState();
        clearStoredRequest();
        return;
    }

    // Consume a possible final SSE event without a trailing newline. Once the
    // stream has already failed, anything still buffered belongs to that failed
    // response and must not reach the transcript as if it were text.
    if (m_completionError.isEmpty() && !leftover.trimmed().isEmpty()) {
        for (const QByteArray &line : leftover.split('\n')) {
            if (line.trimmed().isEmpty()) {
                // End of an event: flush what was accumulated.
                feedSseLine(line);
                continue;
            }
            // A provider error in the tail must not be reported as text.
            if (!feedSseLine(line)) {
                break;
            }
        }
    }
    m_buffer.clear();
    m_pendingEventData.clear();

    if (!m_toolAcc.isEmpty()) {
        m_completedTools.append(completedToolsFromAccumulator());
    }

    // Classify the error. Only trust the body as provider error text when the
    // request actually failed: on a 200 the body is the model's own output, and
    // an answer mentioning "blocked" or "safety" must not read as an error.
    const bool requestFailed = networkError != QNetworkReply::NoError || status >= 400
        || !m_completionError.isEmpty();
    const QByteArray errorBody = requestFailed ? m_responseBody : QByteArray();
    RetryErrorCategory errorCategory = classifyError(status, m_completionError, errorBody, networkError);
    m_retryContext.lastErrorCategory = errorCategory;

    // Check if we should retry
    if (errorCategory != RetryErrorCategory::NonRetryable && errorCategory != RetryErrorCategory::None
        && errorCategory != RetryErrorCategory::ContextOverflow) {
        // Extract provider-suggested retry delay
        std::optional<int> providerDelaySeconds = headerRetryAfter;
        if (!providerDelaySeconds.has_value()) {
            providerDelaySeconds = parseRetryAfterFromBody(m_responseBody);
        }

        if (providerDelaySeconds.has_value()) {
            m_retryContext.hasProviderRetryAfter = true;
            m_retryContext.retryAfter = QDateTime::currentDateTime().addSecs(*providerDelaySeconds);
        }

        if (shouldRetry(m_retryContext)) {
            scheduleRetry(m_retryContext);
            return;
        }
    }

    // No retry - emit final error
    QString finalError;
    // A pure network failure never carries a status code, and "HTTP 0: ..." is
    // noise that hides the real cause.
    const auto describe = [status](const QString &message) {
        return status > 0 ? u"HTTP %1: %2"_s.arg(status).arg(message) : message;
    };
    if (!m_completionError.isEmpty()) {
        finalError = describe(m_completionError);
    } else if (networkError != QNetworkReply::NoError || status >= 400) {
        QString message = errorString;
        if (message.isEmpty()) {
            if (status == 401) {
                message = u"Invalid API key."_s;
            } else if (status == 403) {
                message = u"Access forbidden. Check your API key permissions."_s;
            } else if (status == 404) {
                message = u"Model not found."_s;
            } else if (status == 429) {
                message = u"Rate limit exceeded."_s;
            } else if (status >= 500) {
                message = u"Provider server error."_s;
            } else {
                message = u"Request failed."_s;
            }
        }
        if (!m_responseBody.trimmed().isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(m_responseBody, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                const QJsonObject apiError = doc.object().value(u"error"_s).toObject();
                const QString apiMessage = apiError.value(u"message"_s).toString();
                if (!apiMessage.isEmpty()) {
                    message = apiMessage;
                }
            }
        }
        finalError = describe(message);
    } else {
        // If the streaming response already delivered content or tool calls,
        // the request succeeded -- complete normally instead of falling through
        // to the "no content" error below.
        if (!m_text.isEmpty() || !m_completedTools.isEmpty()) {
            emitCompletedOnce();
            return;
        }
        if (m_sawDone) {
            // The provider closed the stream without saying anything. Completing
            // here would end the turn with a blank reply, which looks like the
            // agent lost the answer rather than that the provider failed.
            finalError = u"The provider returned an empty response."_s;
        }
    }
    if (finalError.isEmpty()) {
        // Some compatible providers may ignore streaming and return ordinary JSON.
        // Read the accumulated body, not just the trailing line: a pretty-printed
        // response is spread over many lines and would otherwise be discarded by
        // the SSE splitter above.
        if (!m_responseBody.trimmed().isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(m_responseBody, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                const QJsonObject root = doc.object();
                const QJsonValue apiError = root.value(u"error"_s);
                if (!apiError.isUndefined() && !apiError.isNull()) {
                    const QString message = apiError.isObject() ? apiError.toObject().value(u"message"_s).toString() : apiError.toString();
                    finalError = message.isEmpty() ? u"The provider returned an unknown error."_s : message;
                } else {
                    const QJsonArray choices = root.value(u"choices"_s).toArray();
                    if (!choices.isEmpty()) {
                        const QJsonObject message = choices.first().toObject().value(u"message"_s).toObject();
                        const QString content = message.value(u"content"_s).toString();
                        const QJsonArray toolCalls = message.value(u"tool_calls"_s).toArray();
                        QList<ToolCall> tools;
                        for (const QJsonValue &value : toolCalls) {
                            const QJsonObject obj = value.toObject();
                            ToolCall tool;
                            tool.id = obj.value(u"id"_s).toString();
                            const QJsonObject fn = obj.value(u"function"_s).toObject();
                            tool.name = fn.value(u"name"_s).toString();
                            // Same two shapes as the streaming path: some
                            // servers answer a non-streaming request with the
                            // arguments as a JSON object.
                            tool.argumentsJson = toolArgumentsText(fn.value(u"arguments"_s));
                            const QJsonDocument argsDoc = QJsonDocument::fromJson(tool.argumentsJson.toUtf8());
                            if (argsDoc.isObject()) {
                                tool.arguments = argsDoc.object();
                            }
                            tools.append(tool);
                        }
                        if (!content.isEmpty()) {
                            m_text = content;
                            Q_EMIT textDelta(content);
                        }
                        m_completedTools = tools;
                        emitCompletedOnce();
                        return;
                    }
                    finalError = u"The provider returned an unexpected response format."_s;
                }
            } else {
                finalError = u"The provider returned an invalid response."_s;
            }
        } else {
            finalError = u"Request failed with no content."_s;
        }
    }

    if (!finalError.isEmpty()) {
        // Add friendly retry-exhausted message for retryable errors
        if (errorCategory != RetryErrorCategory::NonRetryable && errorCategory != RetryErrorCategory::None
        && errorCategory != RetryErrorCategory::ContextOverflow) {
            const QString exhausted = formatRetryExhaustedMessage(errorCategory, m_retryContext.providerName);
            // Keep the provider's own words: "Network connection failed" is
            // useless when the provider actually said why the request failed.
            if (!m_completionError.trimmed().isEmpty()) {
                QString detail = m_completionError.trimmed();
                if (detail.size() > 200) {
                    detail = detail.left(200) + u"…"_s;
                }
                finalError = exhausted + u" ("_s + detail + u")"_s;
            } else {
                finalError = exhausted;
            }
        }
        if (errorCategory == RetryErrorCategory::ContextOverflow
            && !finalError.contains(u"context_window_exceeded"_s, Qt::CaseInsensitive)) {
            finalError = u"context_window_exceeded: "_s + finalError;
        }
        Q_EMIT failed(finalError);
    }

    resetCompletionState();
    // The request is kept on purpose: the user can ask for a manual retry
    // after the automatic retries are exhausted. It is dropped on success
    // (emitCompletedOnce) and on abort.
}

RetryErrorCategory LlmClient::classifyByMessage(const QString &lowerError, const QString &lowerBody)
{
    // Anthropic / Claude compatible
    if (lowerBody.contains(u"overloaded_error") || lowerBody.contains(u"overloaded")) {
        return RetryErrorCategory::ProviderOverloaded;
    }
    if (lowerBody.contains(u"rate_limit") || lowerBody.contains(u"rate limit exceeded")) {
        return RetryErrorCategory::RateLimitExceeded;
    }

    // OpenAI / OpenAI-compatible
    if (lowerBody.contains(u"rate_limit_exceeded") || lowerBody.contains(u"rate limit")) {
        return RetryErrorCategory::RateLimitExceeded;
    }
    if (lowerBody.contains(u"server_error") || lowerBody.contains(u"internal server error")) {
        return RetryErrorCategory::ServerOverloaded;
    }
    if (lowerBody.contains(u"insufficient_quota") || lowerBody.contains(u"quota exceeded")) {
        return RetryErrorCategory::QuotaExceeded;
    }
    if (lowerBody.contains(u"context_length_exceeded") || lowerBody.contains(u"maximum context length")
        || lowerBody.contains(u"context_window_exceeded")) {
        return RetryErrorCategory::ContextOverflow;
    }
    if (lowerBody.contains(u"model_not_found") || lowerBody.contains(u"does not exist")) {
        return RetryErrorCategory::NonRetryable;
    }
    if (lowerBody.contains(u"invalid_api_key") || lowerBody.contains(u"invalid api key")
        || lowerBody.contains(u"incorrect api key")) {
        return RetryErrorCategory::NonRetryable;
    }

    // Google / Gemini
    if (lowerBody.contains(u"quota") && (lowerBody.contains(u"exceeded") || lowerBody.contains(u"limit"))) {
        return RetryErrorCategory::QuotaExceeded;
    }
    if (lowerBody.contains(u"resource_exhausted")) {
        return RetryErrorCategory::RateLimitExceeded;
    }
    if (lowerBody.contains(u"retrydelay") || lowerBody.contains(u"retry_delay")) {
        return RetryErrorCategory::RateLimitExceeded;
    }

    // AWS Bedrock
    if (lowerBody.contains(u"throttlingexception") || lowerBody.contains(u"throttling")) {
        return RetryErrorCategory::RateLimitExceeded;
    }
    if (lowerBody.contains(u"modelstreamerror") || lowerBody.contains(u"model_timeout")) {
        return RetryErrorCategory::ServerOverloaded;
    }

    // Grok / xAI
    if (lowerBody.contains(u"rate limit") || lowerBody.contains(u"too many requests")) {
        return RetryErrorCategory::RateLimitExceeded;
    }
    if (lowerBody.contains(u"server error") || lowerBody.contains(u"internal error")) {
        return RetryErrorCategory::ServerOverloaded;
    }

    // Content policy / safety - generally non-retryable unless provider says otherwise
    if (lowerBody.contains(u"content_policy") || lowerBody.contains(u"safety") ||
        lowerBody.contains(u"violation") || lowerBody.contains(u"blocked") ||
        lowerBody.contains(u"refusal") || lowerBody.contains(u"inappropriate")) {
        return RetryErrorCategory::NonRetryable;
    }

    // A provider-reported error message carries the cause even when the body
    // is empty (an SSE "error" event, for example).
    if (lowerError.contains(u"context_length_exceeded") || lowerError.contains(u"context_window_exceeded")
        || lowerError.contains(u"too many tokens") || lowerError.contains(u"maximum context length")
        || lowerError.contains(u"context length")) {
        return RetryErrorCategory::ContextOverflow;
    }
    if (lowerError.contains(u"invalid_api_key") || lowerError.contains(u"invalid api key")
        || lowerError.contains(u"incorrect api key") || lowerError.contains(u"unauthorized")
        || lowerError.contains(u"forbidden")) {
        return RetryErrorCategory::NonRetryable;
    }
    if (lowerError.contains(u"rate limit") || lowerError.contains(u"too many requests")
        || lowerError.contains(u"overloaded")) {
        return RetryErrorCategory::RateLimitExceeded;
    }

    return RetryErrorCategory::None;
}

RetryErrorCategory LlmClient::classifyError(int httpStatus, const QString &errorMessage, const QByteArray &responseBody, QNetworkReply::NetworkError networkError)
{
    const QString lowerError = errorMessage.toLower();
    const QString lowerBody = QString::fromUtf8(responseBody).toLower();

    // A provider-reported error is authoritative and is checked before the
    // network error. An SSE "error" event aborts the reply, so the network
    // error is always set in that case; classifying it first would turn every
    // provider error into a retryable network failure and throw away the
    // provider's message.
    if (!errorMessage.trimmed().isEmpty()) {
        const RetryErrorCategory fromMessage = classifyByMessage(lowerError, lowerBody);
        if (fromMessage != RetryErrorCategory::None) {
            return fromMessage;
        }
    }

    // HTTP status code classification. Qt reports a network error for every
    // status >= 400 as well, so the status has to be inspected first: 401, 404
    // and 429 are provider errors, not transport failures, and retrying them
    // only delays the real message.
    if (httpStatus >= 400) {
        switch (httpStatus) {
        case 429: // Too Many Requests - Rate limit
            return RetryErrorCategory::RateLimitExceeded;

        case 500: // Internal Server Error
        case 502: // Bad Gateway
        case 503: // Service Unavailable
        case 504: // Gateway Timeout
        case 509: // Bandwidth Limit Exceeded
            return RetryErrorCategory::ServerOverloaded;

        case 400: // Bad Request - check for specific retryable cases
            // Some providers return 400 with retryable messages
            if (lowerBody.contains(u"rate limit") || lowerBody.contains(u"quota") ||
                lowerBody.contains(u"overloaded") || lowerBody.contains(u"throttl")) {
                return RetryErrorCategory::RateLimitExceeded;
            }
            if (lowerBody.contains(u"context window") || lowerBody.contains(u"too many tokens") ||
                lowerBody.contains(u"prompt too large") || lowerBody.contains(u"maximum context") ||
                lowerBody.contains(u"context_length_exceeded") || lowerBody.contains(u"context too long")) {
                return RetryErrorCategory::ContextOverflow;
            }
            {
                const RetryErrorCategory fromBody = classifyByMessage(lowerError, lowerBody);
                if (fromBody != RetryErrorCategory::None) {
                    return fromBody;
                }
            }
            return RetryErrorCategory::NonRetryable;

        case 401: // Unauthorized
        case 403: // Forbidden
            return RetryErrorCategory::NonRetryable;

        case 404: // Not Found - model not found, etc.
            return RetryErrorCategory::NonRetryable;

        case 408: // Request Timeout
            return RetryErrorCategory::Timeout;

        case 413: // Payload Too Large
            return RetryErrorCategory::ContextOverflow;

        default:
            if (httpStatus >= 500 && httpStatus < 600) {
                return RetryErrorCategory::ServerOverloaded;
            }
            break;
        }

        // An unknown error status still carries a body worth classifying.
        const RetryErrorCategory fromBody = classifyByMessage(lowerError, lowerBody);
        if (fromBody != RetryErrorCategory::None) {
            return fromBody;
        }
        return RetryErrorCategory::UnknownTransient;
    }

    // Network-level errors (connection errors, timeouts, etc.). Only reachable
    // when the server did not answer with an HTTP error status.
    if (networkError != QNetworkReply::NoError) {
        return RetryErrorCategory::NetworkError;
    }

    const RetryErrorCategory fromBody = classifyByMessage(lowerError, lowerBody);
    if (fromBody != RetryErrorCategory::None) {
        return fromBody;
    }

    // If we have an error message but couldn't classify it, treat as unknown transient
    if (!errorMessage.isEmpty()) {
        return RetryErrorCategory::UnknownTransient;
    }

    return RetryErrorCategory::None;
}

std::optional<qint64> LlmClient::secondsUntilHttpDate(const QByteArray &header)
{
    // RFC 9110 requires the IMF-fixdate form, whose zone is the name "GMT":
    //   Wed, 21 Oct 2099 07:28:00 GMT
    // Qt's RFC2822Date parser rejects that name outright and its ISODate
    // fallback does not accept the shape at all, so both used to fail and the
    // caller's backoff hint was silently discarded -- the client then retried
    // on exponential backoff while the provider had explicitly asked it to wait.
    QString text = QString::fromLatin1(header).trimmed();
    if (text.isEmpty()) {
        return std::nullopt;
    }

    const int zonePos = text.lastIndexOf(u' ');
    if (zonePos > 0) {
        const QString zone = text.mid(zonePos + 1).toUpper();
        if (zone == u"GMT"_s || zone == u"UT"_s || zone == u"UTC"_s || zone == u"Z"_s) {
            text = text.left(zonePos) + u" +0000"_s;
        }
    }

    QDateTime parsed = QDateTime::fromString(text, Qt::RFC2822Date);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(text, Qt::ISODate);
    }
    if (!parsed.isValid()) {
        return std::nullopt;
    }

    // qint64: secsTo() returns seconds, and a date more than ~68 years out
    // overflows an int and wraps into a negative delay.
    const qint64 secs = QDateTime::currentDateTime().toUTC().secsTo(parsed.toUTC());
    if (secs <= 0) {
        return std::nullopt;
    }
    return secs;
}

std::optional<int> LlmClient::clampProviderDelay(qint64 seconds) const
{
    if (seconds <= 0) {
        return std::nullopt;
    }
    const qint64 cap = m_retryContext.maxDelaySeconds > 0 ? m_retryContext.maxDelaySeconds : 300;
    return static_cast<int>(qMin(seconds, cap));
}

std::optional<int> LlmClient::parseRetryAfterHeader(const QNetworkReply *reply) const
{
    if (!reply) {
        return std::nullopt;
    }

    // Check Retry-After header (seconds or HTTP-date)
    QByteArray retryAfterHeader = reply->rawHeader("Retry-After");
    if (retryAfterHeader.isEmpty()) {
        retryAfterHeader = reply->rawHeader("retry-after");
    }
    if (!retryAfterHeader.isEmpty()) {
        bool ok = false;
        int seconds = retryAfterHeader.toInt(&ok);
        if (ok && seconds > 0) {
            // Cap at reasonable maximum
            return std::min(seconds, m_retryContext.maxDelaySeconds > 0 ? m_retryContext.maxDelaySeconds : 300);
        }
        // Try parsing as HTTP-date.
        if (const std::optional<qint64> secs = secondsUntilHttpDate(retryAfterHeader)) {
            return clampProviderDelay(*secs);
        }
    }

    // Check x-ratelimit-reset headers (Unix timestamp)
    QByteArray resetHeader = reply->rawHeader("x-ratelimit-reset");
    if (resetHeader.isEmpty()) {
        resetHeader = reply->rawHeader("X-RateLimit-Reset");
    }
    if (resetHeader.isEmpty()) {
        resetHeader = reply->rawHeader("x-ratelimit-reset-requests");
    }
    if (resetHeader.isEmpty()) {
        resetHeader = reply->rawHeader("X-RateLimit-Reset-Requests");
    }
    if (!resetHeader.isEmpty()) {
        bool ok = false;
        qint64 resetTime = resetHeader.toLongLong(&ok);
        if (ok && resetTime > 0) {
            QDateTime resetDateTime = QDateTime::fromSecsSinceEpoch(resetTime);
            const qint64 secs = QDateTime::currentDateTime().toUTC().secsTo(resetDateTime.toUTC());
            if (secs > 0) {
                return clampProviderDelay(secs);
            }
        }
    }

    // Check x-ratelimit-reset-after (seconds)
    QByteArray resetAfterHeader = reply->rawHeader("x-ratelimit-reset-after");
    if (resetAfterHeader.isEmpty()) {
        resetAfterHeader = reply->rawHeader("X-RateLimit-Reset-After");
    }
    if (!resetAfterHeader.isEmpty()) {
        bool ok = false;
        int seconds = resetAfterHeader.toInt(&ok);
        if (ok && seconds > 0) {
            return std::min(seconds, m_retryContext.maxDelaySeconds > 0 ? m_retryContext.maxDelaySeconds : 300);
        }
    }

    return std::nullopt;
}

std::optional<int> LlmClient::parseRetryAfterFromBody(const QByteArray &body) const
{
    if (body.isEmpty()) {
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }

    const QJsonObject root = doc.object();

    // Check for structured retryDelay field (Google Gemini, etc.)
    if (root.contains(u"retryDelay"_s)) {
        const QJsonValue val = root.value(u"retryDelay"_s);
        if (val.isString()) {
            QString str = val.toString();
            // Parse duration string like "1.5s" or "30s"
            if (str.endsWith(u's')) {
                str.chop(1);
                bool ok = false;
                double seconds = str.toDouble(&ok);
                if (ok && seconds > 0) {
                    return std::min(static_cast<int>(std::ceil(seconds)),
                                   m_retryContext.maxDelaySeconds > 0 ? m_retryContext.maxDelaySeconds : 300);
                }
            }
        } else if (val.isDouble()) {
            double seconds = val.toDouble();
            if (seconds > 0) {
                return std::min(static_cast<int>(std::ceil(seconds)),
                               m_retryContext.maxDelaySeconds > 0 ? m_retryContext.maxDelaySeconds : 300);
            }
        }
    }

    // Check error object for retry_delay (Anthropic, etc.)
    if (root.contains(u"error"_s)) {
        const QJsonObject error = root.value(u"error"_s).toObject();
        if (error.contains(u"retry_delay"_s)) {
            const QJsonValue val = error.value(u"retry_delay"_s);
            if (val.isString()) {
                QString str = val.toString();
                if (str.endsWith(u's')) {
                    str.chop(1);
                    bool ok = false;
                    double seconds = str.toDouble(&ok);
                    if (ok && seconds > 0) {
                        return std::min(static_cast<int>(std::ceil(seconds)),
                                       m_retryContext.maxDelaySeconds > 0 ? m_retryContext.maxDelaySeconds : 300);
                    }
                }
            } else if (val.isDouble()) {
                double seconds = val.toDouble();
                if (seconds > 0) {
                    return std::min(static_cast<int>(std::ceil(seconds)),
                                   m_retryContext.maxDelaySeconds > 0 ? m_retryContext.maxDelaySeconds : 300);
                }
            }
        }
    }

    return std::nullopt;
}

int LlmClient::calculateDelay(const RetryContext &ctx, std::optional<int> providerDelaySeconds) const
{
    // Prefer provider-supplied delay
    if (providerDelaySeconds.has_value()) {
        int delay = *providerDelaySeconds;
        // Cap at max delay if configured
        if (ctx.maxDelaySeconds > 0 && delay > ctx.maxDelaySeconds) {
            delay = ctx.maxDelaySeconds;
        }
        return qMax(1, delay);
    }

    // Fall back to configured strategy
    qint64 delay = 0;

    if (ctx.strategy.compare(u"fixed"_s, Qt::CaseInsensitive) == 0) {
        delay = ctx.baseDelaySeconds;
    } else {
        // Exponential backoff: base * 2^(attempt-1).
        //
        // Computed by repeated doubling in 64-bit with checked multiplication.
        // The previous `base * (1 << (attempt - 1))` overflowed twice over: the shift is
        // undefined once attempt exceeds 31, and even a well-defined result
        // wraps negative once it is multiplied by 1000 for QTimer::start(int).
        // A negative interval makes QTimer refuse to start at all, which left
        // m_retryScheduled stuck true -- the UI showed an idle client while
        // every subsequent request was rejected as "already in progress".
        delay = qMax(1, ctx.baseDelaySeconds);
        const int doublings = qBound(0, ctx.attempt - 1, 62);
        for (int i = 0; i < doublings && delay < kRetryDelaySaturation; ++i) {
            const qint64 nextDelay = delay * 2;
            if (nextDelay > kRetryDelaySaturation || nextDelay < delay) {
                delay = kRetryDelaySaturation;
                break;
            }
            delay = nextDelay;
        }
        delay = qMin(delay, kRetryDelaySaturation);
    }

    // Cap at max delay if configured
    if (ctx.maxDelaySeconds > 0 && delay > ctx.maxDelaySeconds) {
        delay = ctx.maxDelaySeconds;
    }

    return static_cast<int>(qMin<qint64>(qMax<qint64>(1, delay), std::numeric_limits<int>::max()));
}

QString LlmClient::formatRetryMessage(const RetryContext &ctx, int delaySeconds) const
{
    QString categoryStr;
    switch (ctx.lastErrorCategory) {
    case RetryErrorCategory::RateLimitExceeded:
        categoryStr = u"Rate limit reached"_s;
        break;
    case RetryErrorCategory::ServerOverloaded:
        categoryStr = u"Server overloaded"_s;
        break;
    case RetryErrorCategory::NetworkError:
        categoryStr = u"Network error"_s;
        break;
    case RetryErrorCategory::Timeout:
        categoryStr = u"Request timeout"_s;
        break;
    case RetryErrorCategory::ProviderOverloaded:
        categoryStr = u"Provider overloaded"_s;
        break;
    case RetryErrorCategory::QuotaExceeded:
        categoryStr = u"Quota exceeded"_s;
        break;
    case RetryErrorCategory::UnknownTransient:
        categoryStr = u"Transient error"_s;
        break;
    default:
        categoryStr = u"Error"_s;
        break;
    }

    QString provider = ctx.providerName.isEmpty() ? u"The provider"_s : ctx.providerName;
    return u"%1 – %2 will retry automatically in %3s (attempt %4/%5)"_s
        .arg(categoryStr, provider, QString::number(delaySeconds),
             QString::number(ctx.attempt), QString::number(ctx.maxAttempts));
}

QString LlmClient::formatRetryExhaustedMessage(RetryErrorCategory category, const QString &providerName) const
{
    QString provider = providerName.isEmpty() ? u"The provider"_s : providerName;

    switch (category) {
    case RetryErrorCategory::RateLimitExceeded:
        return u"%1's rate limit was reached. %1 will retry automatically. You can also wait a moment and try again."_s.arg(provider);
    case RetryErrorCategory::ServerOverloaded:
        return u"%1's servers are temporarily unavailable. %1 will retry automatically. You can also wait a moment and try again."_s.arg(provider);
    case RetryErrorCategory::ProviderOverloaded:
        return u"%1 is currently overloaded. %1 will retry automatically. You can also wait a moment and try again."_s.arg(provider);
    case RetryErrorCategory::QuotaExceeded:
        return u"%1's quota has been exceeded. Please check your account limits and try again later."_s.arg(provider);
    case RetryErrorCategory::NetworkError:
        return u"Network connection failed after multiple attempts. Please check your connection and try again."_s;
    case RetryErrorCategory::Timeout:
        return u"The request timed out after multiple attempts. Please try again."_s;
    default:
        return u"The request could not be completed after multiple attempts. Please try again."_s;
    }
}

bool LlmClient::shouldRetry(const RetryContext &ctx) const
{
    if (!m_settings.enableAutoRetry) {
        return false;
    }

    // Check if we've exhausted retry attempts
    if (ctx.attempt >= ctx.maxAttempts) {
        return false;
    }

    // Don't retry non-retryable errors
    if (ctx.lastErrorCategory == RetryErrorCategory::NonRetryable
        || ctx.lastErrorCategory == RetryErrorCategory::ContextOverflow) {
        return false;
    }

    // Don't retry if aborted
    if (m_abortRequested) {
        return false;
    }

    return true;
}

void LlmClient::scheduleRetry(const RetryContext &ctx)
{
    if (m_retryScheduled) {
        return;
    }

    m_retryScheduled = true;

    // The text streamed before the failure belongs to the attempt that failed.
    // The UI has to drop it, otherwise the transcript shows the failed answer
    // followed by the whole retried answer.
    if (!m_text.isEmpty()) {
        m_text.clear();
        Q_EMIT textReset();
    }

    // Increment attempt counter
    RetryContext nextCtx = ctx;
    nextCtx.attempt = ctx.attempt + 1;

    // Calculate delay
    std::optional<int> providerDelay;
    if (ctx.hasProviderRetryAfter) {
        int secs = QDateTime::currentDateTime().secsTo(ctx.retryAfter);
        if (secs > 0) {
            providerDelay = secs;
        }
    }

    int delaySeconds = calculateDelay(nextCtx, providerDelay);
    nextCtx.retryAfter = QDateTime::currentDateTime().addSecs(delaySeconds);
    nextCtx.hasProviderRetryAfter = providerDelay.has_value();

    // Log the retry attempt
    qDebug() << "[LlmClient] Scheduling retry" << nextCtx.attempt << "of" << nextCtx.maxAttempts
             << "in" << delaySeconds << "seconds"
             << "category:" << static_cast<int>(ctx.lastErrorCategory)
             << "providerDelay:" << (providerDelay.has_value() ? QString::number(*providerDelay) : QStringLiteral("none"));

    // Emit retry status for UI
    QString message = formatRetryMessage(nextCtx, delaySeconds);
    Q_EMIT retryStatus(message, nextCtx.attempt, nextCtx.maxAttempts, delaySeconds);
    Q_EMIT retryScheduled(nextCtx.attempt, nextCtx.maxAttempts, delaySeconds);

    // Update stored context
    m_retryContext = nextCtx;

    // QTimer::start() takes an int millisecond interval. Clamp the seconds into
    // a range that survives the *1000, so the timer always actually starts.
    // Also cap at a reasonable maximum (24 hours) to avoid absurdly long waits.
    constexpr qint64 kMaxRetryIntervalMs = 24 * 60 * 60 * 1000; // 24 hours
    const qint64 intervalMs64 = qMin<qint64>(qMax<qint64>(1, qint64(delaySeconds) * 1000), kMaxRetryIntervalMs);
    const int intervalMs = static_cast<int>(intervalMs64);
    m_retryTimer.start(intervalMs);
    if (!m_retryTimer.isActive()) {
        // Unreachable while the interval is clamped, but a refused timer would
        // strand m_retryScheduled and wedge the client, so fail loudly instead.
        m_retryScheduled = false;
        clearStoredRequest();
        Q_EMIT failed(u"Could not schedule the retry."_s);
        return;
    }
}

void LlmClient::executeRetry()
{
    m_retryScheduled = false;

    if (m_abortRequested || m_storedMessages.isEmpty()) {
        clearStoredRequest();
        return;
    }

    // Check if we should still retry (user might have disabled auto-retry in settings)
    if (!shouldRetry(m_retryContext)) {
        QString finalError = formatRetryExhaustedMessage(m_retryContext.lastErrorCategory, m_retryContext.providerName);
        Q_EMIT failed(finalError);
        clearStoredRequest();
        return;
    }

    // Execute the retry
    doComplete(m_storedMessages);
}

void LlmClient::storeRequestForRetry(const QList<ChatMessage> &messages)
{
    m_storedMessages = messages;
}

void LlmClient::clearStoredRequest()
{
    m_storedMessages.clear();
}

} // namespace KateAi
