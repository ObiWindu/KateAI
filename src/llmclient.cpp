#include "llmclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <optional>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

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
    QList<ToolCall> tools;
    tools.reserve(m_toolAcc.size());
    for (auto it = m_toolAcc.cbegin(); it != m_toolAcc.cend(); ++it) {
        ToolCall tool = it.value();
        if (tool.id.isEmpty()) {
            tool.id = u"call_%1"_s.arg(it.key());
        }
        QJsonParseError parseError;
        const QJsonDocument argsDoc = QJsonDocument::fromJson(tool.argumentsJson.toUtf8(), &parseError);
        if (parseError.error == QJsonParseError::NoError && argsDoc.isObject()) {
            tool.arguments = argsDoc.object();
        }
        tools.append(tool);
    }
    std::sort(tools.begin(), tools.end(), [](const ToolCall &a, const ToolCall &b) {
        return a.id < b.id;
    });
    m_toolAcc.clear();
    return tools;
}

CompletionChunk LlmClient::parseSseLine(const QByteArray &line, QHash<int, ToolCall> *acc)
{
    CompletionChunk chunk;
    QByteArray payload = line.trimmed();
    if (payload.startsWith("data:")) {
        payload = payload.mid(5).trimmed();
    }
    if (payload.isEmpty()) {
        return chunk;
    }
    if (payload == "[DONE]") {
        chunk.finished = true;
        chunk.finishReason = u"stop"_s;
        if (acc) {
            for (auto it = acc->cbegin(); it != acc->cend(); ++it) {
                ToolCall tool = it.value();
                if (tool.id.isEmpty()) {
                    tool.id = u"call_%1"_s.arg(it.key());
                }
                QJsonParseError parseError;
                const QJsonDocument argsDoc = QJsonDocument::fromJson(tool.argumentsJson.toUtf8(), &parseError);
                if (parseError.error == QJsonParseError::NoError && argsDoc.isObject()) {
                    tool.arguments = argsDoc.object();
                }
                chunk.completedTools.append(tool);
            }
            acc->clear();
        }
        return chunk;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return chunk;
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
            const int index = obj.value(u"index"_s).toInt();
            ToolCall &tool = (*acc)[index];
            if (obj.contains(u"id"_s) && !obj.value(u"id"_s).toString().isEmpty()) {
                tool.id = obj.value(u"id"_s).toString();
            }
            const QJsonObject fn = obj.value(u"function"_s).toObject();
            if (fn.contains(u"name"_s) && !fn.value(u"name"_s).toString().isEmpty()) {
                tool.name = fn.value(u"name"_s).toString();
            }
            if (fn.contains(u"arguments"_s)) {
                tool.argumentsJson += fn.value(u"arguments"_s).toString();
            }
        }
    }

    if (!chunk.finishReason.isEmpty() && chunk.finishReason != u"null"_s) {
        chunk.finished = true;
        if (acc) {
            for (auto it = acc->cbegin(); it != acc->cend(); ++it) {
                ToolCall tool = it.value();
                if (tool.id.isEmpty()) {
                    tool.id = u"call_%1"_s.arg(it.key());
                }
                QJsonParseError parseError;
                const QJsonDocument argsDoc = QJsonDocument::fromJson(tool.argumentsJson.toUtf8(), &parseError);
                if (parseError.error == QJsonParseError::NoError && argsDoc.isObject()) {
                    tool.arguments = argsDoc.object();
                }
                chunk.completedTools.append(tool);
            }
            acc->clear();
        }
    }
    return chunk;
}

void LlmClient::resetCompletionState()
{
    m_buffer.clear();
    m_text.clear();
    m_toolAcc.clear();
    m_completedTools.clear();
    m_completionError.clear();
    m_sawDone = false;
    m_finishEmitted = false;
    m_abortRequested = false;
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
    m_retryContext = RetryContext{};
    m_retryContext.attempt = 0;
    m_retryContext.maxAttempts = qMax(1, m_settings.maxRetryAttempts);
    m_retryContext.baseDelaySeconds = qMax(1, m_settings.baseRetryDelaySeconds);
    m_retryContext.maxDelaySeconds = m_settings.maxRetryDelaySeconds;
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

    QJsonObject body;
    body.insert(u"model"_s, model);
    body.insert(u"messages"_s, messagesToJson(messages));
    body.insert(u"tools"_s, toolDefinitions(m_settings.planMode));
    body.insert(u"tool_choice"_s, m_settings.parallelToolCalls ? u"auto"_s : u"none"_s);
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

    QNetworkRequest request{QUrl(providerBaseUrl(m_settings.provider) + u"/chat/completions"_s)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    request.setRawHeader("Accept", "text/event-stream");
    if (m_settings.provider == Provider::OpenRouter) {
        request.setRawHeader("HTTP-Referer", "https://kate-editor.org");
        request.setRawHeader("X-Title", "Kate AI");
    }

    m_reply = m_nam.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &LlmClient::handleReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &LlmClient::handleFinished);
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

    QNetworkRequest request{QUrl(providerBaseUrl(provider) + u"/models"_s)};
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    request.setHeader(QNetworkRequest::UserAgentHeader, u"Kate AI"_s);
    if (provider == Provider::OpenRouter) {
        request.setRawHeader("HTTP-Referer", "https://kate-editor.org");
        request.setRawHeader("X-Title", "Kate AI");
    }

    QNetworkReply *reply = m_nam.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, provider]() {
        handleModelsFinished(reply, provider);
    });
}

void LlmClient::abort()
{
    m_retryTimer.stop();
    m_retryScheduled = false;

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

    // Reset retry context for manual retry
    m_retryContext.attempt = 0;
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
    for (const QJsonValue &value : root.value(u"data"_s).toArray()) {
        const QString id = value.toObject().value(u"id"_s).toString().trimmed();
        if (!id.isEmpty() && !models.contains(id)) {
            models.append(id);
        }
    }
    models.sort(Qt::CaseInsensitive);
    if (models.isEmpty()) {
        Q_EMIT modelsFailed(provider, u"The provider did not return any available models."_s);
        return;
    }
    Q_EMIT modelsReceived(provider, models);
}

void LlmClient::handleReadyRead()
{
    if (!m_reply || m_abortRequested) {
        return;
    }

    m_buffer.append(m_reply->readAll());
    while (true) {
        const int idx = m_buffer.indexOf('\n');
        if (idx < 0) {
            break;
        }
        const QByteArray line = m_buffer.left(idx);
        m_buffer.remove(0, idx + 1);
        if (line.trimmed().isEmpty()) {
            continue;
        }

        const CompletionChunk chunk = parseSseLine(line, &m_toolAcc);
        if (!chunk.error.isEmpty()) {
            m_completionError = chunk.error;
            if (m_reply) {
                // Do not emit failed() or call abort() from readyRead(). Keep
                // lifecycle transitions inside handleFinished().
                m_reply->abort();
            }
            return;
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

    const QByteArray leftover = m_buffer + reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString errorString = reply->errorString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (m_abortRequested) {
        resetCompletionState();
        clearStoredRequest();
        return;
    }

    // Consume a possible final SSE event without a trailing newline.
    if (!leftover.trimmed().isEmpty()) {
        for (const QByteArray &line : leftover.split('\n')) {
            if (line.trimmed().isEmpty()) {
                continue;
            }
            const CompletionChunk chunk = parseSseLine(line, &m_toolAcc);
            if (!chunk.error.isEmpty() && m_completionError.isEmpty()) {
                m_completionError = chunk.error;
            }
            if (!chunk.contentDelta.isEmpty()) {
                m_text += chunk.contentDelta;
                Q_EMIT textDelta(chunk.contentDelta);
            }
            if (!chunk.completedTools.isEmpty()) {
                m_completedTools.append(chunk.completedTools);
            }
            if (chunk.finished) {
                m_sawDone = true;
            }
        }
    }

    if (!m_toolAcc.isEmpty()) {
        m_completedTools.append(completedToolsFromAccumulator());
    }

    // Classify the error
    RetryErrorCategory errorCategory = classifyError(status, m_completionError, leftover, networkError);
    m_retryContext.lastErrorCategory = errorCategory;

    // Check if we should retry
    if (errorCategory != RetryErrorCategory::NonRetryable && errorCategory != RetryErrorCategory::None) {
        // Extract provider-suggested retry delay
        std::optional<int> providerDelaySeconds = parseRetryAfterHeader(reply);
        if (!providerDelaySeconds.has_value()) {
            providerDelaySeconds = parseRetryAfterFromBody(leftover);
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
    if (!m_completionError.isEmpty()) {
        finalError = u"HTTP %1: %2"_s.arg(status).arg(m_completionError);
    } else if (networkError != QNetworkReply::NoError) {
        QString message = errorString;
        if (message.isEmpty()) {
            message = u"Network request failed."_s;
        }
        if (!leftover.trimmed().isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(leftover, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                const QJsonObject apiError = doc.object().value(u"error"_s).toObject();
                const QString apiMessage = apiError.value(u"message"_s).toString();
                if (!apiMessage.isEmpty()) {
                    message = apiMessage;
                }
            }
        }
        finalError = u"HTTP %1: %2"_s.arg(status).arg(message);
    } else {
        // Some compatible providers may ignore streaming and return ordinary JSON.
        if (!m_sawDone && m_text.isEmpty() && m_completedTools.isEmpty() && !leftover.trimmed().isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(leftover, &parseError);
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
                            tool.argumentsJson = fn.value(u"arguments"_s).toString();
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
        if (errorCategory != RetryErrorCategory::NonRetryable && errorCategory != RetryErrorCategory::None) {
            finalError = formatRetryExhaustedMessage(errorCategory, m_retryContext.providerName);
        }
        Q_EMIT failed(finalError);
    }

    resetCompletionState();
    clearStoredRequest();
}

RetryErrorCategory LlmClient::classifyError(int httpStatus, const QString &errorMessage, const QByteArray &responseBody, QNetworkReply::NetworkError networkError)
{
    const QString lowerError = errorMessage.toLower();
    const QString lowerBody = QString::fromUtf8(responseBody).toLower();

    // Network-level errors (connection errors, timeouts, etc.)
    if (networkError != QNetworkReply::NoError) {
        switch (networkError) {
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:
        case QNetworkReply::SslHandshakeFailedError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::UnknownNetworkError:
            return RetryErrorCategory::NetworkError;
        default:
            return RetryErrorCategory::NetworkError;
        }
    }

    // HTTP status code classification
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
            lowerBody.contains(u"prompt too large") || lowerBody.contains(u"maximum context")) {
            return RetryErrorCategory::NonRetryable;
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
        return RetryErrorCategory::NonRetryable;

    default:
        if (httpStatus >= 500 && httpStatus < 600) {
            return RetryErrorCategory::ServerOverloaded;
        }
        break;
    }

    // Provider-specific error message classification
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
    if (lowerBody.contains(u"context_length_exceeded") || lowerBody.contains(u"maximum context length")) {
        return RetryErrorCategory::NonRetryable;
    }
    if (lowerBody.contains(u"model_not_found") || lowerBody.contains(u"does not exist")) {
        return RetryErrorCategory::NonRetryable;
    }
    if (lowerBody.contains(u"invalid_api_key") || lowerBody.contains(u"incorrect api key")) {
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

    // If we have an error message but couldn't classify it, treat as unknown transient
    if (!errorMessage.isEmpty() || httpStatus >= 400) {
        return RetryErrorCategory::UnknownTransient;
    }

    return RetryErrorCategory::None;
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
        // Try parsing as HTTP-date
        QDateTime retryDate = QDateTime::fromString(QString::fromLatin1(retryAfterHeader), Qt::RFC2822Date);
        if (!retryDate.isValid()) {
            retryDate = QDateTime::fromString(QString::fromLatin1(retryAfterHeader), Qt::ISODate);
        }
        if (retryDate.isValid()) {
            int secs = QDateTime::currentDateTime().secsTo(retryDate);
            if (secs > 0) {
                return std::min(secs, m_retryContext.maxDelaySeconds > 0 ? m_retryContext.maxDelaySeconds : 300);
            }
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
            int secs = QDateTime::currentDateTime().secsTo(resetDateTime);
            if (secs > 0) {
                return std::min(secs, m_retryContext.maxDelaySeconds > 0 ? m_retryContext.maxDelaySeconds : 300);
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
    int attempt = ctx.attempt; // 1-based for first retry
    int delay = 0;

    if (ctx.strategy.compare(u"fixed"_s, Qt::CaseInsensitive) == 0) {
        delay = ctx.baseDelaySeconds;
    } else {
        // Exponential backoff: base * 2^(attempt-1)
        delay = ctx.baseDelaySeconds * (1 << (attempt - 1));
    }

    // Cap at max delay if configured
    if (ctx.maxDelaySeconds > 0 && delay > ctx.maxDelaySeconds) {
        delay = ctx.maxDelaySeconds;
    }

    return qMax(1, delay);
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
    if (ctx.lastErrorCategory == RetryErrorCategory::NonRetryable) {
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

    // Start timer
    m_retryTimer.start(delaySeconds * 1000);
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