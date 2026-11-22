/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "llmclient.h"

#include "harness/models.h"
#include "harness/tokens.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QDateTime>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

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

// OpenAI-compatible gateways that accept stream_options.include_usage. Local
// servers often reject unknown request fields, so they are left alone.
bool providerSupportsStreamUsage(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
    case Provider::OpenRouter:
    case Provider::DeepSeek:
    case Provider::Grok:
    case Provider::OpenCode:
        return true;
    default:
        return false;
    }
}

// The OpenAI-compatible Gemini facade lives on the same host as the native
// API. The native base is derived from it so cached-content resources and the
// native streaming endpoint can be used without a separate provider setting.
bool isGoogleGenerativeLanguageHost(const QString &host)
{
    return host.compare(u"generativelanguage.googleapis.com"_s, Qt::CaseInsensitive) == 0;
}

QString geminiBaseUrlFor(const Settings &settings)
{
    QUrl url(settings.openaiCompatibleUrl.trimmed());
    if (!url.isValid() || url.host().isEmpty()) {
        url = QUrl(u"https://generativelanguage.googleapis.com/v1beta"_s);
    }
    QString path = url.path();
    // Strip the compatibility suffix; native resources live under /v1beta.
    const int beta = path.indexOf(u"/v1beta"_s);
    if (beta >= 0) {
        path = path.left(beta + 7);
    } else {
        const int v1 = path.indexOf(u"/v1"_s);
        if (v1 >= 0) {
            path = path.left(v1 + 3) + u"beta"_s;
        } else {
            path = u"/v1beta"_s;
        }
    }
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString();
}

QString geminiModelResource(const QString &model)
{
    const QString name = model.trimmed();
    if (name.startsWith(u"models/"_s) || name.startsWith(u"tunedModels/"_s)) {
        return name;
    }
    return u"models/"_s + name;
}

QJsonObject geminiTextPart(const QString &text)
{
    return QJsonObject{{u"text"_s, text}};
}

QJsonObject geminiContent(const QString &role, const QJsonArray &parts)
{
    return QJsonObject{{u"role"_s, role}, {u"parts"_s, parts}};
}

QJsonValue sanitizeGeminiSchema(const QJsonValue &value)
{
    if (value.isArray()) {
        QJsonArray out;
        for (const QJsonValue &entry : value.toArray()) {
            out.append(sanitizeGeminiSchema(entry));
        }
        return out;
    }
    if (!value.isObject()) {
        return value;
    }
    const QJsonObject source = value.toObject();
    QJsonObject out;
    // Gemini's Schema is an OpenAPI subset. Unknown keywords (strict,
    // additionalProperties, $schema, anyOf, …) are rejected by the native API.
    static const QSet<QString> allowed{
        u"type"_s, u"description"_s, u"properties"_s, u"required"_s,
        u"items"_s, u"enum"_s, u"format"_s};
    for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            continue;
        }
        if (it.key() == u"properties"_s && it.value().isObject()) {
            QJsonObject properties;
            const QJsonObject sourceProperties = it.value().toObject();
            for (auto property = sourceProperties.constBegin();
                 property != sourceProperties.constEnd(); ++property) {
                properties.insert(property.key(), sanitizeGeminiSchema(property.value()));
            }
            out.insert(it.key(), properties);
        } else {
            out.insert(it.key(), sanitizeGeminiSchema(it.value()));
        }
    }
    return out;
}

QJsonArray geminiTools(const QJsonArray &openAiTools)
{
    QJsonArray declarations;
    for (const QJsonValue &value : openAiTools) {
        const QJsonObject function = value.toObject().value(u"function"_s).toObject();
        const QString name = function.value(u"name"_s).toString();
        if (name.isEmpty()) {
            continue;
        }
        QJsonObject declaration;
        declaration.insert(u"name"_s, name);
        const QString description = function.value(u"description"_s).toString();
        if (!description.isEmpty()) {
            declaration.insert(u"description"_s, description);
        }
        const QJsonValue parameters = function.value(u"parameters"_s);
        if (parameters.isObject()) {
            declaration.insert(u"parameters"_s, sanitizeGeminiSchema(parameters));
        }
        declarations.append(declaration);
    }
    if (declarations.isEmpty()) {
        return {};
    }
    return QJsonArray{QJsonObject{{u"functionDeclarations"_s, declarations}}};
}

QJsonObject geminiFunctionResponse(const ChatMessage &message)
{
    QString text = message.content;
    QJsonObject response;
    QJsonDocument parsed = QJsonDocument::fromJson(text.toUtf8());
    if (parsed.isObject()) {
        response = parsed.object();
    } else {
        response.insert(u"result"_s, text);
    }
    return QJsonObject{
        {u"functionResponse"_s,
         QJsonObject{{u"name"_s, message.name.isEmpty() ? u"tool"_s : message.name},
                     {u"response"_s, response}}}};
}

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
            // Hidden reasoning is deliberately not replayed. DeepSeek returns
            // 400 when reasoning_content is present in an input message, and
            // strict OpenAI-compatible gateways reject unknown message fields.
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

// True when the accumulated argument text is a complete JSON value. A tool
// call that is still being streamed has incomplete arguments, which is how a
// continuation delta is told apart from the start of a new call.
static bool isCompleteToolArguments(const QString &arguments)
{
    if (arguments.trimmed().isEmpty()) {
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(arguments.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && !document.isNull();
}

// The accumulator slot an index-less tool-call delta belongs to.
//
// A provider that omits "index" cannot be handled by falling back to the next
// free slot: a call that is streamed over several deltas - which is how every
// OpenAI-compatible server sends one - would be split into one call per delta,
// each with a fragment of the arguments and no name. The slot is therefore
// derived from the delta itself:
//
//  - A delta that names a call continues that call.
//  - A delta without an id is a continuation of the call whose arguments are
//    still incomplete.
//
// Only when neither applies is a new slot opened.
static int slotForIndexLessDelta(const QHash<int, ToolCall> &acc, const QJsonObject &delta)
{
    const QString deltaId = delta.value(u"id"_s).toString();
    if (!deltaId.isEmpty()) {
        for (auto it = acc.constBegin(); it != acc.constEnd(); ++it) {
            if (it.value().id == deltaId) {
                return it.key();
            }
        }
    } else {
        int openSlot = -1;
        int openCalls = 0;
        for (auto it = acc.constBegin(); it != acc.constEnd(); ++it) {
            if (isCompleteToolArguments(it.value().argumentsJson)) {
                continue;
            }
            ++openCalls;
            // The highest slot is the call that was started last.
            openSlot = qMax(openSlot, it.key());
        }
        if (openCalls > 0) {
            return openSlot;
        }
    }
    // A new call: the next free slot. The keys of a hash are not ordered, so
    // the highest one is looked for explicitly.
    int next = 0;
    for (auto it = acc.constBegin(); it != acc.constEnd(); ++it) {
        next = qMax(next, it.key() + 1);
    }
    return next;
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

bool LlmClient::usesGeminiNativeTransport(const Settings &settings)
{
    if (settings.provider != Provider::OpenAICompatible) {
        return false;
    }
    const QUrl url(settings.openaiCompatibleUrl.trimmed());
    return url.isValid() && isGoogleGenerativeLanguageHost(url.host());
}

QString LlmClient::geminiNativeBaseUrl(const Settings &settings)
{
    return geminiBaseUrlFor(settings);
}

QString LlmClient::geminiModelName(const QString &model)
{
    QString name = model.trimmed();
    if (name.startsWith(u"models/"_s)) {
        name = name.mid(7);
    }
    return name;
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
    const QJsonValue streamError = root.value(u"error"_s);
    if (!streamError.isUndefined() && !streamError.isNull()) {
        const QJsonValue error = streamError;
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
                const QJsonValue indexValue = root.value(u"index"_s);
                const int index = indexValue.isDouble()
                    ? indexValue.toInt()
                    : (indexValue.isString() ? indexValue.toString().toInt() : 0);
                ToolCall &tool = (*acc)[index];
                const QString frag = delta.value(u"partial_json"_s).toString();
                constexpr qsizetype kMaxToolArgsChars = 1024 * 1024;
                if (tool.argumentsJson.size() + frag.size() <= kMaxToolArgsChars) {
                    tool.argumentsJson += frag;
                }
            }
        } else if (anthropicType == u"content_block_start"_s) {
            const QJsonObject block = root.value(u"content_block"_s).toObject();
            if (block.value(u"type"_s).toString() == u"tool_use"_s && acc) {
                const QJsonValue indexValue = root.value(u"index"_s);
                const int index = indexValue.isDouble()
                    ? indexValue.toInt()
                    : (indexValue.isString() ? indexValue.toString().toInt() : 0);
                ToolCall &tool = (*acc)[index];
                tool.id = block.value(u"id"_s).toString();
                tool.name = block.value(u"name"_s).toString();
                // A short input can arrive complete here, with no delta events.
                // The protocol makes it an object, but an agent that sends it as
                // a JSON string would otherwise lose every argument to
                // QJsonValue::toObject()'s empty result on a string.
                const QJsonValue input = block.value(u"input"_s);
                if (input.isString() && !input.toString().isEmpty()) {
                    constexpr qsizetype kMaxToolArgsChars = 1024 * 1024;
                    if (tool.argumentsJson.size() + input.toString().size() <= kMaxToolArgsChars) {
                        tool.argumentsJson += input.toString();
                    }
                } else if (input.isObject()) {
                    tool.arguments = input.toObject();
                }
            }
        } else if (anthropicType == u"message_start"_s) {
            const QJsonObject usage = root.value(u"message"_s).toObject().value(u"usage"_s).toObject();
            chunk.inputTokens = usage.value(u"input_tokens"_s).toInt();
            chunk.outputTokens = usage.value(u"output_tokens"_s).toInt();
            chunk.cachedInputTokens = usage.value(u"cache_read_input_tokens"_s).toInt();
        } else if (anthropicType == u"message_delta"_s) {
            chunk.finishReason = root.value(u"delta"_s).toObject().value(u"stop_reason"_s).toString();
            const QJsonObject usage = root.value(u"usage"_s).toObject();
            chunk.outputTokens = usage.value(u"output_tokens"_s).toInt();
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

    // Gemini native streaming uses candidates/parts rather than the OpenAI
    // choices/delta envelope. It has no "[DONE]" sentinel; finishReason is the
    // terminator.
    const QJsonArray candidates = root.value(u"candidates"_s).toArray();
    if (!candidates.isEmpty() || root.contains(u"usageMetadata"_s)
        || root.contains(u"promptFeedback"_s)) {
        const QJsonObject feedback = root.value(u"promptFeedback"_s).toObject();
        const QString blockReason = feedback.value(u"blockReason"_s).toString();
        if (!blockReason.isEmpty()) {
            chunk.error = u"Gemini blocked the response (%1)."_s.arg(blockReason);
            chunk.finished = true;
            return chunk;
        }
        if (!candidates.isEmpty()) {
            const QJsonObject candidate = candidates.first().toObject();
            const QJsonArray parts = candidate.value(u"content"_s).toObject()
                                         .value(u"parts"_s).toArray();
            for (const QJsonValue &value : parts) {
                const QJsonObject part = value.toObject();
                const QJsonObject functionCall = part.value(u"functionCall"_s).toObject();
                if (!functionCall.isEmpty()) {
                    ToolCall tool;
                    tool.id = u"call_"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
                    tool.name = functionCall.value(u"name"_s).toString();
                    const QJsonValue args = functionCall.value(u"args"_s);
                    if (args.isObject()) {
                        tool.arguments = args.toObject();
                        tool.argumentsJson = QString::fromUtf8(
                            QJsonDocument(tool.arguments).toJson(QJsonDocument::Compact));
                    } else if (args.isString()) {
                        tool.argumentsJson = args.toString();
                        const QJsonDocument parsed = QJsonDocument::fromJson(
                            tool.argumentsJson.toUtf8());
                        if (parsed.isObject()) {
                            tool.arguments = parsed.object();
                        }
                    }
                    if (!tool.name.isEmpty()) {
                        chunk.completedTools.append(tool);
                    }
                    continue;
                }
                const QString text = part.value(u"text"_s).toString();
                if (!text.isEmpty()) {
                    if (part.value(u"thought"_s).toBool()) {
                        chunk.thinkingDelta += text;
                    } else {
                        chunk.contentDelta += text;
                    }
                }
            }
            QString finish = candidate.value(u"finishReason"_s).toString();
            if (finish == u"STOP"_s) {
                finish = u"stop"_s;
            } else if (finish == u"MAX_TOKENS"_s) {
                finish = u"max_tokens"_s;
            } else if (finish == u"SAFETY"_s || finish == u"RECITATION"_s
                       || finish == u"BLOCKLIST"_s || finish == u"PROHIBITED_CONTENT"_s
                       || finish == u"SPII"_s) {
                chunk.error = u"Gemini stopped the response (%1)."_s.arg(finish);
                chunk.finished = true;
                chunk.finishReason = u"content_filter"_s;
                return chunk;
            }
            if (!finish.isEmpty()) {
                chunk.finishReason = finish;
                chunk.finished = true;
            }
        }
        const QJsonObject geminiUsage = root.value(u"usageMetadata"_s).toObject();
        if (!geminiUsage.isEmpty()) {
            chunk.inputTokens = geminiUsage.value(u"promptTokenCount"_s).toInt();
            chunk.outputTokens = geminiUsage.value(u"candidatesTokenCount"_s).toInt()
                + geminiUsage.value(u"thoughtsTokenCount"_s).toInt();
            chunk.cachedInputTokens =
                geminiUsage.value(u"cachedContentTokenCount"_s).toInt();
            if (candidates.isEmpty()) {
                chunk.finished = true;
            }
        }
        return chunk;
    }

    const QJsonObject usage = root.value(u"usage"_s).toObject();
    if (!usage.isEmpty()) {
        // Most providers send a final usage-only chunk (choices is empty),
        // but some attach the same object to the final choice. Parse it in
        // both cases so calibration does not silently depend on the gateway.
        chunk.inputTokens = usage.value(u"prompt_tokens"_s).toInt();
        chunk.outputTokens = usage.value(u"completion_tokens"_s).toInt();
        const QJsonObject details = usage.value(u"prompt_tokens_details"_s).toObject();
        chunk.cachedInputTokens = details.value(u"cached_tokens"_s).toInt();
        if (chunk.cachedInputTokens == 0) {
            chunk.cachedInputTokens = usage.value(u"prompt_cache_hit_tokens"_s).toInt();
        }
    }

    const QJsonArray choices = root.value(u"choices"_s).toArray();
    if (choices.isEmpty()) {
        if (!usage.isEmpty()) {
            // The usage-only chunk is the end of an OpenAI-compatible stream.
            chunk.finished = true;
        }
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
            const QJsonValue indexValue = obj.value(u"index"_s);
            int index = 0;
            bool haveIndex = false;
            if (indexValue.isDouble()) {
                index = indexValue.toInt();
                haveIndex = true;
            } else if (indexValue.isString()) {
                bool ok = false;
                const int parsed = indexValue.toString().trimmed().toInt(&ok);
                if (ok) {
                    index = parsed;
                    haveIndex = true;
                }
            }
            // A provider that omits (or sends a non-numeric) "index" would
            // otherwise send every parallel tool call to slot 0. The slot
            // comes from the delta instead.
            if (!haveIndex) {
                index = slotForIndexLessDelta(*acc, obj);
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
                        // Bound memory: attacker-controlled JSON fragments must
                        // not grow without limit before drainToolAccumulator().
                        constexpr qsizetype kMaxToolArgsChars = 1024 * 1024;
                        if (tool.argumentsJson.size() + fragment.size() <= kMaxToolArgsChars) {
                            tool.argumentsJson += fragment;
                        }
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
    m_pendingEventData.clear();
    m_responseBody.clear();
    m_text.clear();
    m_toolAcc.clear();
    m_completedTools.clear();
    m_completionError.clear();
    m_sawDone = false;
    m_finishEmitted = false;
    m_usageEmitted = false;
    m_usageInput = 0;
    m_usageOutput = 0;
    m_usageCached = 0;
    m_geminiRequestUsedCache = false;
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
    QString requestUrl;
    QByteArray requestBody;
    QString acceptHeader = u"text/event-stream"_s;

    const bool geminiNative = usesGeminiNativeTransport(m_settings);
    GeminiCachePlan geminiPlan;
    if (geminiNative) {
        geminiPlan = geminiCachePlan(messages, model);
        const bool expired = !m_geminiCacheKey.isEmpty()
            && (!m_geminiCacheExpires.isValid()
                || m_geminiCacheExpires <= QDateTime::currentDateTimeUtc());
        if (!m_geminiCacheKey.isEmpty()
            && (m_geminiCacheKey != geminiPlan.key || expired)) {
            // The cache is for a different system prompt, tool set, model, or
            // first user turn. Drop it before the request so an in-flight
            // request cannot observe a resource being deleted underneath it.
            clearGeminiCache();
        }
        body = buildGeminiRequest(geminiPlan.requestMessages, model,
                                  geminiPlan.cachedContent);
        QUrl url(geminiNativeBaseUrl(m_settings));
        QString path = url.path();
        if (!path.endsWith(u'/')) {
            path += u'/';
        }
        path += QString::fromLatin1(QUrl::toPercentEncoding(geminiModelResource(model),
                                                            QByteArray("/")))
            + u":streamGenerateContent"_s;
        url.setPath(path);
        QUrlQuery query;
        query.addQueryItem(u"alt"_s, u"sse"_s);
        url.setQuery(query);
        requestUrl = url.toString();
        requestBody = QJsonDocument(body).toJson(QJsonDocument::Compact);
        acceptHeader = u"text/event-stream"_s;
    } else {
        QString endpoint;
        switch (apiFormat) {
        case ApiFormat::AnthropicCompatible:
            body = buildAnthropicRequest(messages, model);
            endpoint = u"/v1/messages"_s;
            break;
        case ApiFormat::AcpNative:
            body = buildAcpNativeRequest(messages, model);
            endpoint = u"/acp/v1/chat/completions"_s;
            break;
        case ApiFormat::OpenAICompatible:
        default:
            body = buildOpenAIRequest(messages, model);
            endpoint = u"/chat/completions"_s;
            break;
        }
        requestUrl = joinApiEndpoint(providerBaseUrl(m_settings), endpoint);
        requestBody = QJsonDocument(body).toJson(QJsonDocument::Compact);
    }

    QNetworkRequest request{QUrl(requestUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    // Force a fresh connection per request. Reusing a keep-alive connection to
    // the same host (notably localhost on macOS) serialised the sub-agents'
    // requests and made them run one after another instead of in parallel.
    request.setRawHeader("Connection", "close");
    // Idle watchdog: a provider that accepts the connection and then goes quiet
    // would otherwise leave the turn pending forever, because nothing else here
    // sets a deadline. Qt treats this as an *idle* timeout -- it only fires when
    // no bytes arrive -- so a long generation that keeps streaming is never cut
    // short, while a half-open connection still ends with a retryable error.
    request.setTransferTimeout(kRequestIdleTimeoutMs);
    if (geminiNative) {
        request.setRawHeader("x-goog-api-key", key.toUtf8());
    } else {
        applyAuthHeaders(request, apiFormat, key);
    }
    request.setRawHeader("Accept", acceptHeader.toUtf8());
    if (m_settings.provider == Provider::OpenRouter) {
        request.setRawHeader("HTTP-Referer", "https://kate-editor.org");
        request.setRawHeader("X-Title", "Kate AI");
    }

    m_geminiRequestUsedCache = geminiNative && !geminiPlan.cachedContent.isEmpty();
    m_reply = m_nam.post(request, requestBody);
    if (geminiNative) {
        // Cache creation happens alongside the completion. It is used by the
        // next request, never by this one, so a cold cache does not add a
        // round-trip to the user's first turn.
        startGeminiCache(messages, model, geminiPlan);
    }

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
    if (!m_toolDefinitionsOverride.isEmpty()) {
        return m_toolDefinitionsOverride;
    }
    QJsonArray tools = toolDefinitions(m_toolAccess);
    for (const QJsonValue &tool : m_extraTools) {
        const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
        // An extra tool still has to be permitted by the active mode. The
        // registry bridge tools are the exception: they only route a call to a
        // tool that is checked again when it executes.
        if (name == u"tool_search"_s || name == u"tool_describe"_s || name == u"tool_call"_s
            || m_toolAccess.isEmpty() || m_toolAccess.allows(name)) {
            tools.append(tool);
        }
    }
    return tools;
}

QJsonObject LlmClient::buildOpenAIRequest(const QList<ChatMessage> &messages, const QString &model)
{
    const ModelProfile profile = ModelProfiles::resolve(m_settings.provider, model);
    QJsonObject body;
    body.insert(u"model"_s, model);
    body.insert(u"messages"_s, messagesToJson(messages));
    body.insert(u"tools"_s, advertisedTools());
    // "none" is not "no parallelism": it forbids tool calls altogether, so the
    // agent could not act at all when the user turned batching off. Batching is
    // controlled by parallel_tool_calls instead.
    if (profile.supportsToolChoice) {
        body.insert(u"tool_choice"_s, u"auto"_s);
    }
    body.insert(u"parallel_tool_calls"_s,
                m_settings.parallelToolCalls && profile.supportsParallelToolCalls);
    body.insert(u"stream"_s, true);
    if (providerSupportsStreamUsage(m_settings.provider)) {
        body.insert(u"stream_options"_s,
                    QJsonObject{{u"include_usage"_s, true}});
    }
    body.insert(u"temperature"_s, m_settings.temperature);
    body.insert(u"top_p"_s, m_settings.topP);
    if (m_settings.maxTokens > 0) {
        int maxTokens = m_settings.maxTokens;
        if (profile.maxOutputTokens > 0) {
            maxTokens = qMin(maxTokens, profile.maxOutputTokens);
        }
        body.insert(u"max_tokens"_s, maxTokens);
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
    // Enhanced intelligence parameters. These are OpenRouter extensions, not
    // part of the OpenAI schema: sending them to OpenAI, DeepSeek or a local
    // server can be rejected as an unknown request field.
    if (m_settings.provider == Provider::OpenRouter) {
        if (m_settings.structuredThinking) {
            body.insert(u"include_reasoning"_s, true);
        }
        if (m_settings.maxThinkingTokens > 0) {
            body.insert(u"max_reasoning_tokens"_s, m_settings.maxThinkingTokens);
        }
    }
    return body;
}

QJsonObject LlmClient::buildAnthropicRequest(const QList<ChatMessage> &messages, const QString &model)
{
    const ModelProfile profile = ModelProfiles::resolve(m_settings.provider, model);
    const bool cache = profile.cacheStyle == CacheStyle::Anthropic;

    QJsonObject body;
    body.insert(u"model"_s, model);

    // The system prompt is a top-level parameter, never a system-role message.
    const QStringList systemParts = anthropicSystemParts(messages);
    if (!systemParts.isEmpty()) {
        const QString systemText = systemParts.join(u"\n\n"_s);
        if (cache) {
            // One cache breakpoint after the whole static prompt. Tool schemas
            // are stable too, so the breakpoint covers the prefix up to the
            // first turn message; editor state travels as a turn message.
            QJsonObject block;
            block.insert(u"type"_s, u"text"_s);
            block.insert(u"text"_s, systemText);
            block.insert(u"cache_control"_s, QJsonObject{{u"type"_s, u"ephemeral"_s}});
            body.insert(u"system"_s, QJsonArray{block});
        } else {
            body.insert(u"system"_s, systemText);
        }
    }

    body.insert(u"messages"_s, messagesToAnthropicJson(messages));
    body.insert(u"stream"_s, true);
    // max_tokens is required by the Messages API; fall back to a sane default.
    int maxTokens = m_settings.maxTokens > 0 ? m_settings.maxTokens : 4096;
    if (profile.maxOutputTokens > 0) {
        maxTokens = qMin(maxTokens, profile.maxOutputTokens);
    }
    body.insert(u"max_tokens"_s, maxTokens);

    // Extended thinking is the Messages API counterpart of the OpenAI
    // include_reasoning/max_reasoning_tokens pair; without it the thinking
    // Tools must be in Anthropic's {name, input_schema} shape, not OpenAI's.
    // Anthropic has no parallel_tool_calls flag; it always decides on its own
    // which tools to batch, and "none" would forbid tool calls entirely.
    QJsonArray tools = anthropicTools(advertisedTools());
    if (cache && !tools.isEmpty()) {
        // A breakpoint on the last tool makes the tool block part of the
        // cached prefix. The editor state follows as a turn message, so a
        // changing workspace does not invalidate it.
        QJsonObject last = tools.last().toObject();
        last.insert(u"cache_control"_s, QJsonObject{{u"type"_s, u"ephemeral"_s}});
        tools.replace(tools.size() - 1, last);
    }

    // Extended thinking blocks must be replayed with their signatures when a
    // tool_use turn is sent back. Kate does not retain those signatures, so
    // enabling thinking on a tool-using turn would make the follow-up request
    // fail with "Expected thinking ... but found tool_use". Keep thinking for
    // plain chat turns, where no replay is required, and disable it whenever
    // tools are advertised or the budget cannot satisfy budget < max_tokens.
    const bool thinkingEnabled = tools.isEmpty() && m_settings.structuredThinking
        && m_settings.maxThinkingTokens > 0 && maxTokens > 1024;
    body.insert(u"temperature"_s, thinkingEnabled ? 1.0 : qBound(0.0, m_settings.temperature, 1.0));
    if (!thinkingEnabled && m_settings.topP > 0.0) {
        body.insert(u"top_p"_s, qBound(0.0, m_settings.topP, 1.0));
    }
    if (thinkingEnabled) {
        QJsonObject thinking;
        thinking.insert(u"type"_s, u"enabled"_s);
        const int ceiling = maxTokens - 1;
        thinking.insert(u"budget_tokens"_s, qBound(1024, m_settings.maxThinkingTokens, ceiling));
        body.insert(u"thinking"_s, thinking);
    }

    if (!tools.isEmpty()) {
        body.insert(u"tools"_s, tools);
        if (profile.supportsToolChoice) {
            body.insert(u"tool_choice"_s, u"auto"_s);
        }
    }
    return body;
}

QJsonArray LlmClient::messagesToGeminiJson(const QList<ChatMessage> &messages,
                                           QString *systemInstruction) const
{
    QJsonArray contents;
    QStringList systemParts;
    auto appendContent = [&contents](const QString &role, const QJsonArray &parts) {
        if (parts.isEmpty()) {
            return;
        }
        if (!contents.isEmpty()
            && contents.last().toObject().value(u"role"_s).toString() == role) {
            QJsonObject content = contents.last().toObject();
            QJsonArray existing = content.value(u"parts"_s).toArray();
            for (const QJsonValue &part : parts) {
                existing.append(part);
            }
            content.insert(u"parts"_s, existing);
            contents.replace(contents.size() - 1, content);
            return;
        }
        contents.append(geminiContent(role, parts));
    };

    for (const ChatMessage &message : messages) {
        switch (message.role) {
        case ChatMessage::Role::System:
            if (!message.content.trimmed().isEmpty()) {
                systemParts.append(message.content.trimmed());
            }
            break;
        case ChatMessage::Role::User: {
            if (!message.content.isEmpty()) {
                appendContent(u"user"_s,
                              QJsonArray{geminiTextPart(message.content)});
            }
            break;
        }
        case ChatMessage::Role::Assistant: {
            QJsonArray parts;
            if (!message.content.isEmpty()) {
                parts.append(geminiTextPart(message.content));
            }
            for (const QJsonValue &value : message.toolCalls) {
                const QJsonObject object = value.toObject();
                const QJsonObject function = object.value(u"function"_s).toObject();
                const QString name = function.value(u"name"_s).toString();
                if (name.isEmpty()) {
                    continue;
                }
                QJsonObject args = function.value(u"arguments"_s).toObject();
                if (args.isEmpty() && function.value(u"arguments"_s).isString()) {
                    const QJsonDocument parsed = QJsonDocument::fromJson(
                        function.value(u"arguments"_s).toString().toUtf8());
                    if (parsed.isObject()) {
                        args = parsed.object();
                    }
                }
                parts.append(QJsonObject{
                    {u"functionCall"_s,
                     QJsonObject{{u"name"_s, name}, {u"args"_s, args}}}});
            }
            appendContent(u"model"_s, parts);
            break;
        }
        case ChatMessage::Role::Tool:
            appendContent(u"user"_s, QJsonArray{geminiFunctionResponse(message)});
            break;
        }
    }
    if (systemInstruction) {
        *systemInstruction = systemParts.join(u"\n\n"_s);
    }
    return contents;
}

QJsonObject LlmClient::buildGeminiRequest(const QList<ChatMessage> &messages,
                                          const QString &model,
                                          const QString &cachedContent)
{
    const ModelProfile profile = ModelProfiles::resolve(m_settings.provider, model);
    QJsonObject body;
    QString systemInstruction;
    QJsonArray contents = messagesToGeminiJson(messages, &systemInstruction);
    if (contents.isEmpty()) {
        contents.append(geminiContent(u"user"_s,
                                      QJsonArray{geminiTextPart(u" "_s)}));
    }
    body.insert(u"contents"_s, contents);

    if (cachedContent.isEmpty()) {
        if (!systemInstruction.trimmed().isEmpty()) {
            body.insert(u"systemInstruction"_s,
                        QJsonObject{{u"parts"_s,
                                     QJsonArray{geminiTextPart(systemInstruction)}}});
        }
        const QJsonArray tools = geminiTools(advertisedTools());
        if (!tools.isEmpty()) {
            body.insert(u"tools"_s, tools);
            body.insert(u"toolConfig"_s,
                        QJsonObject{{u"functionCallingConfig"_s,
                                     QJsonObject{{u"mode"_s, u"AUTO"_s}}}});
        }
    } else {
        body.insert(u"cachedContent"_s, cachedContent);
    }

    QJsonObject generation;
    generation.insert(u"temperature"_s, m_settings.temperature);
    if (m_settings.topP > 0.0) {
        generation.insert(u"topP"_s, m_settings.topP);
    }
    if (m_settings.maxTokens > 0) {
        int maxTokens = m_settings.maxTokens;
        if (profile.maxOutputTokens > 0) {
            maxTokens = qMin(maxTokens, profile.maxOutputTokens);
        }
        generation.insert(u"maxOutputTokens"_s, maxTokens);
    }
    body.insert(u"generationConfig"_s, generation);
    return body;
}

LlmClient::GeminiCachePlan LlmClient::geminiCachePlan(
    const QList<ChatMessage> &messages, const QString &model) const
{
    GeminiCachePlan plan;
    plan.requestMessages = messages;

    int firstUser = -1;
    int prefixEnd = -1;
    QStringList systems;
    for (int i = 0; i < messages.size(); ++i) {
        if (messages.at(i).role == ChatMessage::Role::System) {
            if (!messages.at(i).content.trimmed().isEmpty()) {
                systems.append(messages.at(i).content.trimmed());
            }
            continue;
        }
        if (firstUser < 0 && messages.at(i).role == ChatMessage::Role::User) {
            firstUser = i;
            continue;
        }
        // Cache the first exchange, not just the first user turn. The suffix
        // then starts with a user/tool response, which keeps Gemini's content
        // roles valid and gives the cache a useful amount of content.
        if (firstUser >= 0 && messages.at(i).role == ChatMessage::Role::Assistant) {
            prefixEnd = i;
            break;
        }
    }
    if (firstUser < 0 || prefixEnd < 0) {
        return plan;
    }
    plan.systemText = systems.join(u"\n\n"_s);
    plan.prefixContents = messagesToGeminiJson(messages.mid(0, prefixEnd + 1), nullptr);

    const QJsonArray tools = geminiTools(advertisedTools());
    const QByteArray material = (model + u'\n' + plan.systemText + u'\n'
                                  + QString::fromUtf8(QJsonDocument(plan.prefixContents)
                                                          .toJson(QJsonDocument::Compact))
                                  + u'\n'
                                  + QString::fromUtf8(QJsonDocument(tools).toJson(
                                        QJsonDocument::Compact)))
                                     .toUtf8();
    plan.key = QString::fromLatin1(
        QCryptographicHash::hash(material, QCryptographicHash::Sha1).toHex());

    const bool cacheValid = !m_geminiCacheKey.isEmpty()
        && m_geminiCacheKey == plan.key
        && !m_geminiCacheName.isEmpty()
        && m_geminiCacheExpires > QDateTime::currentDateTimeUtc();
    // A cache without a suffix would leave the request without contents.
    if (cacheValid && prefixEnd + 1 < messages.size()) {
        plan.cachedContent = m_geminiCacheName;
        plan.requestMessages = messages.mid(prefixEnd + 1);
    }
    return plan;
}

QJsonObject LlmClient::buildGeminiCacheRequest(const GeminiCachePlan &plan,
                                               const QString &model) const
{
    QJsonObject payload;
    payload.insert(u"model"_s, geminiModelResource(model));
    payload.insert(u"displayName"_s, QString(u"kateai-"_s + plan.key.left(12)));
    payload.insert(u"ttl"_s, u"1800s"_s);
    payload.insert(u"contents"_s, plan.prefixContents);
    if (!plan.systemText.trimmed().isEmpty()) {
        payload.insert(u"systemInstruction"_s,
                       QJsonObject{{u"parts"_s,
                                    QJsonArray{geminiTextPart(plan.systemText)}}});
    }
    const QJsonArray tools = geminiTools(advertisedTools());
    if (!tools.isEmpty()) {
        payload.insert(u"tools"_s, tools);
    }
    return payload;
}

void LlmClient::clearGeminiCache()
{
    if (m_geminiCacheReply) {
        m_geminiCacheReply->disconnect(this);
        m_geminiCacheReply->abort();
        m_geminiCacheReply->deleteLater();
        m_geminiCacheReply = nullptr;
    }
    m_geminiCachePendingKey.clear();
    if (!m_geminiCacheName.isEmpty()) {
        deleteGeminiCacheResource(m_geminiCacheBaseUrl, m_geminiCacheName);
    }
    m_geminiCacheKey.clear();
    m_geminiCacheName.clear();
    m_geminiCacheBaseUrl.clear();
    m_geminiCacheExpires = QDateTime();
    m_geminiCacheFailedKey.clear();
}

void LlmClient::deleteGeminiCacheResource(const QString &baseUrl, const QString &name)
{
    if (baseUrl.isEmpty() || name.isEmpty()) {
        return;
    }
    QUrl url(baseUrl);
    QString path = url.path();
    if (!path.endsWith(u'/')) {
        path += u'/';
    }
    path += name;
    url.setPath(path);
    QNetworkRequest request(url);
    request.setRawHeader("x-goog-api-key", apiKeyFor(m_settings).toUtf8());
    request.setTransferTimeout(30000);
    if (QNetworkReply *reply = m_nam.deleteResource(request)) {
        connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    }
}

void LlmClient::startGeminiCache(const QList<ChatMessage> &messages,
                                 const QString &model,
                                 const GeminiCachePlan &plan)
{
    Q_UNUSED(messages)
    if (plan.key.isEmpty() || !plan.cachedContent.isEmpty()) {
        return;
    }
    if (m_geminiCacheFailedKey == plan.key) {
        return;
    }
    if (m_geminiCacheKey == plan.key && !m_geminiCacheName.isEmpty()
        && m_geminiCacheExpires > QDateTime::currentDateTimeUtc()) {
        return;
    }
    if (m_geminiCachePendingKey == plan.key && m_geminiCacheReply) {
        return;
    }
    const QString toolsJson = QString::fromUtf8(
        QJsonDocument(geminiTools(advertisedTools())).toJson(QJsonDocument::Compact));
    const QString prefixJson = QString::fromUtf8(
        QJsonDocument(plan.prefixContents).toJson(QJsonDocument::Compact));
    const int prefixTokens = Tokens::estimate(plan.systemText)
        + Tokens::estimate(prefixJson)
        + Tokens::estimate(toolsJson);
    // Gemini rejects caches below its model-specific minimum. Pro models use a
    // 4K floor; Flash and smaller use 1K.
    const int minimumTokens =
        geminiModelName(model).contains(u"pro"_s, Qt::CaseInsensitive) ? 4096 : 1024;
    if (prefixTokens < minimumTokens) {
        return;
    }

    const QJsonObject payload = buildGeminiCacheRequest(plan, model);

    if (m_geminiCacheReply) {
        m_geminiCacheReply->disconnect(this);
        m_geminiCacheReply->abort();
        m_geminiCacheReply->deleteLater();
        m_geminiCacheReply = nullptr;
    }

    const QString base = geminiNativeBaseUrl(m_settings);
    QNetworkRequest request(QUrl(base + u"/cachedContents"_s));
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setRawHeader("x-goog-api-key", apiKeyFor(m_settings).toUtf8());
    request.setRawHeader("Connection", "close");
    request.setTransferTimeout(45000);

    m_geminiCachePendingKey = plan.key;
    m_geminiCacheReply = m_nam.post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QNetworkReply *reply = m_geminiCacheReply;
    const QString cacheKey = plan.key;
    connect(reply, &QNetworkReply::finished, this, [this, reply, cacheKey, base] {
        if (m_geminiCacheReply != reply && m_geminiCachePendingKey != cacheKey) {
            reply->deleteLater();
            return;
        }
        if (m_geminiCacheReply == reply) {
            m_geminiCacheReply = nullptr;
        }
        if (m_geminiCachePendingKey == cacheKey) {
            m_geminiCachePendingKey.clear();
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError networkError = reply->error();
        const QString errorString = reply->errorString();
        reply->deleteLater();
        QJsonDocument document = QJsonDocument::fromJson(body);
        const QJsonObject root = document.isObject() ? document.object() : QJsonObject();
        const QString name = root.value(u"name"_s).toString();
        if (networkError != QNetworkReply::NoError || status >= 400 || name.isEmpty()) {
            // A local network failure may succeed on the next turn; a server
            // rejection (bad schema, below minimum, unsupported model) will
            // not, so remember only the latter.
            if (networkError == QNetworkReply::NoError) {
                m_geminiCacheFailedKey = cacheKey;
            }
            qWarning() << "KateAI: Gemini cache creation failed"
                       << status << errorString << body.left(400);
            return;
        }
        const QString oldName = m_geminiCacheName;
        const QString oldBase = m_geminiCacheBaseUrl;
        m_geminiCacheKey = cacheKey;
        m_geminiCacheName = name;
        m_geminiCacheBaseUrl = base;
        // Leave a minute of headroom before the server-side expiry so a request
        // does not cross the boundary mid-flight.
        m_geminiCacheExpires = QDateTime::currentDateTimeUtc().addSecs(1800 - 60);
        if (!oldName.isEmpty() && oldName != name) {
            deleteGeminiCacheResource(oldBase, oldName);
        }
    });
}

QJsonObject LlmClient::buildAcpNativeRequest(const QList<ChatMessage> &messages, const QString &model)
{
    // ACP Native format - similar to OpenAI but with ACP-specific extensions
    const ModelProfile profile = ModelProfiles::resolve(m_settings.provider, model);
    QJsonObject body;
    body.insert(u"model"_s, model);
    body.insert(u"messages"_s, messagesToJson(messages));
    body.insert(u"tools"_s, advertisedTools());
    // "none" is not "no parallelism": it forbids tool calls altogether, so the
    // agent could not act at all when the user turned batching off. Batching is
    // controlled by parallel_tool_calls instead.
    if (profile.supportsToolChoice) {
        body.insert(u"tool_choice"_s, u"auto"_s);
    }
    body.insert(u"parallel_tool_calls"_s,
                m_settings.parallelToolCalls && profile.supportsParallelToolCalls);
    body.insert(u"stream"_s, true);
    body.insert(u"temperature"_s, m_settings.temperature);
    body.insert(u"top_p"_s, m_settings.topP);
    if (m_settings.maxTokens > 0) {
        int maxTokens = m_settings.maxTokens;
        if (profile.maxOutputTokens > 0) {
            maxTokens = qMin(maxTokens, profile.maxOutputTokens);
        }
        body.insert(u"max_tokens"_s, maxTokens);
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

    const bool geminiNative = usesGeminiNativeTransport(providerSettings);
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
    // custom Ollama/LocalAI/ACP URL actually takes effect. The Gemini native
    // catalogue lives under the native base, not the /openai compatibility
    // suffix.
    QString modelsUrl;
    if (geminiNative) {
        QUrl url(geminiNativeBaseUrl(providerSettings));
        QString path = url.path();
        if (!path.endsWith(u'/')) {
            path += u'/';
        }
        path += u"models"_s;
        url.setPath(path);
        QUrlQuery query;
        query.addQueryItem(u"pageSize"_s, u"1000"_s);
        url.setQuery(query);
        modelsUrl = url.toString();
    } else {
        modelsUrl = joinApiEndpoint(providerBaseUrl(providerSettings), modelsEndpoint);
    }
    QNetworkRequest request{QUrl(modelsUrl)};
    if (geminiNative) {
        request.setRawHeader("x-goog-api-key", key.toUtf8());
    } else {
        applyAuthHeaders(request, apiFormat, key);
    }
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

    if (m_geminiCacheReply) {
        m_geminiCacheReply->disconnect(this);
        m_geminiCacheReply->abort();
        m_geminiCacheReply->deleteLater();
        m_geminiCacheReply = nullptr;
    }
    m_geminiCachePendingKey.clear();

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

    // Gemini's native catalogue uses {"models":[{"name":"models/gemini-…"}]}
    // instead of the OpenAI {"data":[…]} envelope.
    if (models.isEmpty()) {
        for (const QJsonValue &value : root.value(u"models"_s).toArray()) {
            QString id = value.toObject().value(u"name"_s).toString().trimmed();
            if (id.startsWith(u"models/"_s)) {
                id = id.mid(7);
            }
            if (!id.isEmpty() && !models.contains(id)) {
                models.append(id);
            }
        }
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
    if (chunk.inputTokens > 0) {
        m_usageInput = qMax(m_usageInput, chunk.inputTokens);
    }
    if (chunk.outputTokens > 0) {
        m_usageOutput = qMax(m_usageOutput, chunk.outputTokens);
    }
    if (chunk.cachedInputTokens > 0) {
        m_usageCached = qMax(m_usageCached, chunk.cachedInputTokens);
    }
    if (chunk.finished) {
        m_sawDone = true;
        if (!m_usageEmitted && (m_usageInput > 0 || m_usageOutput > 0)) {
            m_usageEmitted = true;
            Q_EMIT usageReceived(m_usageInput, m_usageOutput, m_usageCached);
        }
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
    // Bound memory: a non-SSE single-line body would otherwise grow m_buffer
    // forever (only m_responseBody was capped). Drop the head; SSE framing
    // resyncs on the next newline.
    constexpr qsizetype kMaxStreamBufferBytes = 4 * 1024 * 1024;
    if (m_buffer.size() > kMaxStreamBufferBytes) {
        m_buffer.remove(0, m_buffer.size() - kMaxStreamBufferBytes);
        m_pendingEventData.clear();
    }
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

    // Some streams report usage in a trailing event without a finishReason.
    // Emit it at stream end so calibration still sees the provider's count.
    if (!m_usageEmitted && (m_usageInput > 0 || m_usageOutput > 0)) {
        m_usageEmitted = true;
        Q_EMIT usageReceived(m_usageInput, m_usageOutput, m_usageCached);
    }

    // Classify the error. Only trust the body as provider error text when the
    // request actually failed: on a 200 the body is the model's own output, and
    // an answer mentioning "blocked" or "safety" must not read as an error.
    // A stream that already delivered [DONE] is complete even if the socket
    // was reset afterwards. Treating the late transport error as a failure
    // threw away the full answer and re-ran the model.
    const bool requestFailed = !m_sawDone
        && (networkError != QNetworkReply::NoError || status >= 400 || !m_completionError.isEmpty());
    if (requestFailed) {
        // The body reaches the user verbatim in the error text, and providers
        // echo request details in it: a key leaked into a 400 body would be
        // shown as-is. Redact before anything reads the bodies below.
        m_completionError = sanitizeResponseBody(m_completionError.toUtf8());
        m_responseBody = sanitizeResponseBody(m_responseBody).toUtf8();
    }
    if (requestFailed && m_geminiRequestUsedCache
        && (status == 400 || status == 404)
        && m_responseBody.toLower().contains(QByteArray("cached"))) {
        // The server no longer knows the resource (expired or deleted out of
        // band). Forget it so the next request goes out uncached.
        clearGeminiCache();
    }
    const QByteArray errorBody = requestFailed ? m_responseBody : QByteArray();
    RetryErrorCategory errorCategory = classifyError(status, m_completionError, errorBody, networkError);
    m_retryContext.lastErrorCategory = errorCategory;

    // Check if we should retry
    if (!m_sawDone
        && errorCategory != RetryErrorCategory::NonRetryable && errorCategory != RetryErrorCategory::None
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
    // Permanent billing/authorisation failures. Retrying these only delays the
    // message the user has to act on.
    if (lowerError.contains(u"credit balance") || lowerError.contains(u"insufficient")
        || lowerError.contains(u"billing") || lowerError.contains(u"payment required")
        || lowerBody.contains(u"credit balance") || lowerBody.contains(u"insufficient_quota")) {
        return RetryErrorCategory::NonRetryable;
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
            // Any other 4xx is the client's fault (validation, method, media
            // type, payment). It will not become valid on a retry.
            return RetryErrorCategory::NonRetryable;
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
    // 0 means "no configured cap"; use the same 24-hour ceiling the retry
    // timer applies instead of silently turning it into five minutes.
    const qint64 cap = m_retryContext.maxDelaySeconds > 0
        ? m_retryContext.maxDelaySeconds
        : 24 * 60 * 60;
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
            return clampProviderDelay(seconds);
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
                    // Clamp in double space: casting an out-of-range double to
                    // int is undefined and usually yields a negative delay.
                    const qint64 rounded = static_cast<qint64>(std::ceil(std::min(seconds, 1.0e12)));
                    return clampProviderDelay(rounded);
                }
            }
        } else if (val.isDouble()) {
            double seconds = val.toDouble();
            if (seconds > 0) {
                const qint64 rounded = static_cast<qint64>(std::ceil(std::min(seconds, 1.0e12)));
                return clampProviderDelay(rounded);
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
    // The last retry in the budget is the one with attempt == maxAttempts.
    // shouldRetry() refuses to schedule anything beyond that, but it must not
    // be used here: it refuses this final attempt too, which wasted a full
    // backoff and made maxRetryAttempts = 1 run no retry at all.
    const bool mayRun = m_settings.enableAutoRetry && !m_abortRequested
        && m_retryContext.attempt <= m_retryContext.maxAttempts;
    if (!mayRun) {
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
