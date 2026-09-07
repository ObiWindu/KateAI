#include "llmclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

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
    const QJsonObject delta = choice.contains(u"delta"_s) ? choice.value(u"delta"_s).toObject() : choice.value(u"message"_s).toObject();
    chunk.contentDelta = delta.value(u"content"_s).toString();
    chunk.finishReason = choice.value(u"finish_reason"_s).toString();

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

    if (!chunk.finishReason.isEmpty() && chunk.finishReason != u"null"_s) {
        chunk.finished = true;
        for (auto it = acc->begin(); it != acc->end(); ++it) {
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

void LlmClient::complete(const QList<ChatMessage> &messages)
{
    abort();
    m_buffer.clear();
    m_text.clear();
    m_toolAcc.clear();
    m_sawDone = false;

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
    if (!m_reply) {
        return;
    }
    m_reply->disconnect(this);
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
}

void LlmClient::reset()
{
    if (m_reply) {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
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
    if (!m_reply) {
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
            Q_EMIT failed(chunk.error);
            abort();
            return;
        }
        if (!chunk.contentDelta.isEmpty()) {
            m_text += chunk.contentDelta;
            Q_EMIT textDelta(chunk.contentDelta);
        }
        if (chunk.finished && !m_sawDone) {
            m_sawDone = true;
            Q_EMIT finished(m_text, chunk.completedTools);
        }
    }
}

void LlmClient::handleFinished()
{
    if (!m_reply) {
        return;
    }
    const QByteArray leftover = m_buffer + m_reply->readAll();
    const QNetworkReply::NetworkError error = m_reply->error();
    const QString errorString = m_reply->errorString();
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_reply->deleteLater();
    m_reply = nullptr;

    if (m_sawDone) {
        return;
    }

    if (error != QNetworkReply::NoError && error != QNetworkReply::OperationCanceledError) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(leftover, &parseError);
        QString message;
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            message = doc.object().value(u"error"_s).toObject().value(u"message"_s).toString();
        }
        if (message.isEmpty()) {
            message = errorString;
            if (!leftover.isEmpty()) {
                message += u": "_s + QString::fromUtf8(leftover.left(500));
            }
        }
        Q_EMIT failed(u"HTTP %1: %2"_s.arg(status).arg(message));
        return;
    }

    if (error == QNetworkReply::OperationCanceledError) {
        return;
    }

    // A final SSE event is allowed to arrive without a trailing newline.  It
    // has not been handled by readyRead(), so consume it before considering a
    // non-stream response. Keep m_text and m_toolAcc: earlier events may have
    // already contributed text and partial tool arguments.
    if (leftover.trimmed().startsWith("data:")) {
        QList<ToolCall> tools;
        for (const QByteArray &line : leftover.split('\n')) {
            const CompletionChunk chunk = parseSseLine(line, &m_toolAcc);
            if (!chunk.error.isEmpty()) {
                Q_EMIT failed(chunk.error);
                return;
            }
            if (!chunk.contentDelta.isEmpty()) {
                m_text += chunk.contentDelta;
                Q_EMIT textDelta(chunk.contentDelta);
            }
            if (chunk.finished) {
                tools = chunk.completedTools;
            }
        }
        Q_EMIT finished(m_text, tools);
        return;
    }

    // Non-stream JSON fallback.
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(leftover, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonValue apiError = root.value(u"error"_s);
        if (!apiError.isUndefined() && !apiError.isNull()) {
            const QString message = apiError.isObject() ? apiError.toObject().value(u"message"_s).toString() : apiError.toString();
            Q_EMIT failed(message.isEmpty() ? u"The provider returned an unknown error."_s : message);
            return;
        }
        const QJsonArray choices = root.value(u"choices"_s).toArray();
        if (choices.isEmpty()) {
            Q_EMIT failed(u"The provider response did not contain a completion."_s);
            return;
        }
        const QJsonObject choice = choices.first().toObject();
        const QJsonObject message = choice.value(u"message"_s).toObject();
        const QString content = message.value(u"content"_s).toString();
        QList<ToolCall> tools;
        const QJsonArray toolCalls = message.value(u"tool_calls"_s).toArray();
        for (int i = 0; i < toolCalls.size(); ++i) {
            const QJsonObject obj = toolCalls.at(i).toObject();
            ToolCall tool;
            tool.id = obj.value(u"id"_s).toString();
            tool.name = obj.value(u"function"_s).toObject().value(u"name"_s).toString();
            tool.argumentsJson = obj.value(u"function"_s).toObject().value(u"arguments"_s).toString();
            const QJsonDocument argsDoc = QJsonDocument::fromJson(tool.argumentsJson.toUtf8());
            if (argsDoc.isObject()) {
                tool.arguments = argsDoc.object();
            }
            tools.append(tool);
        }
        if (!content.isEmpty()) {
            Q_EMIT textDelta(content);
        }
        Q_EMIT finished(content, tools);
        return;
    }

    if (!leftover.trimmed().isEmpty()) {
        QHash<int, ToolCall> acc;
        QList<ToolCall> tools;
        QString text;
        for (const QByteArray &line : leftover.split('\n')) {
            const CompletionChunk chunk = parseSseLine(line, &acc);
            text += chunk.contentDelta;
            if (chunk.finished) {
                tools = chunk.completedTools;
            }
            if (!chunk.error.isEmpty()) {
                Q_EMIT failed(chunk.error);
                return;
            }
        }
        Q_EMIT finished(m_text + text, tools);
        return;
    }

    Q_EMIT finished(m_text, {});
}

} // namespace KateAi
