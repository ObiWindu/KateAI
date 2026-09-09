#include "llmclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

LlmClient::LlmClient(QObject *parent)
    : QObject(parent)
{
}

LlmClient::~LlmClient()
{
    abort();
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
    if (m_reply) {
        Q_EMIT failed(u"A model request is already in progress."_s);
        return;
    }

    resetCompletionState();

    const QString key = apiKeyFor(m_settings).trimmed();
    if (key.isEmpty()) {
        Q_EMIT failed(u"No API key configured for %1."_s.arg(providerLabel(m_settings.provider)));
        return;
    }

    const QString model = modelFor(m_settings);
    if (model.isEmpty()) {
        Q_EMIT failed(u"No model selected."_s);
        return;
    }

    QJsonObject body;
    body.insert(u"model"_s, model);
    body.insert(u"messages"_s, messagesToJson(messages));
    body.insert(u"tools"_s, toolDefinitions(m_settings.planMode));
    body.insert(u"tool_choice"_s, u"auto"_s);
    body.insert(u"stream"_s, true);
    body.insert(u"temperature"_s, 0.2);

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
}

void LlmClient::reset()
{
    abort();
}

void LlmClient::emitCompletedOnce()
{
    if (m_finishEmitted) {
        return;
    }
    m_finishEmitted = true;
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
    const QNetworkReply::NetworkError error = reply->error();
    const QString errorString = reply->errorString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (m_abortRequested) {
        resetCompletionState();
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

    if (!m_completionError.isEmpty()) {
        Q_EMIT failed(u"HTTP %1: %2"_s.arg(status).arg(m_completionError));
        resetCompletionState();
        return;
    }

    if (error != QNetworkReply::NoError) {
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
        Q_EMIT failed(u"HTTP %1: %2"_s.arg(status).arg(message));
        resetCompletionState();
        return;
    }

    // Some compatible providers may ignore streaming and return ordinary JSON.
    if (!m_sawDone && m_text.isEmpty() && m_completedTools.isEmpty() && !leftover.trimmed().isEmpty()) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(leftover, &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject root = doc.object();
            const QJsonValue apiError = root.value(u"error"_s);
            if (!apiError.isUndefined() && !apiError.isNull()) {
                const QString message = apiError.isObject() ? apiError.toObject().value(u"message"_s).toString() : apiError.toString();
                Q_EMIT failed(message.isEmpty() ? u"The provider returned an unknown error."_s : message);
                resetCompletionState();
                return;
            }
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
            }
        }
    }

    emitCompletedOnce();
    resetCompletionState();
}

} // namespace KateAi
