/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "events.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

bool isSecretKey(const QString &rawKey)
{
    const QString key = rawKey.toLower();
    static const QSet<QString> exact = {
        u"authorization"_s,
        u"api_key"_s,
        u"apikey"_s,
        u"access_token"_s,
        u"refresh_token"_s,
        u"id_token"_s,
        u"password"_s,
        u"passwd"_s,
        u"secret"_s,
        u"client_secret"_s,
        u"private_key"_s,
        u"credential"_s,
        u"credentials"_s,
        u"cookie"_s,
        u"set-cookie"_s,
    };
    if (exact.contains(key)) {
        return true;
    }
    if (key.endsWith(u"_api_key"_s) || key.endsWith(u"_token"_s) || key.endsWith(u"_secret"_s)
        || key.endsWith(u"_password"_s) || key.endsWith(u"_credential"_s)) {
        return true;
    }
    return false;
}

const QString &redactedMarker()
{
    static const QString marker = QStringLiteral("[redacted]");
    return marker;
}

} // namespace

HarnessEvent Events::make(const QString &type, const QJsonObject &payload, const QString &turnId,
                          const QString &stepId)
{
    return make(type, QDateTime::currentMSecsSinceEpoch(), payload, turnId, stepId);
}

HarnessEvent Events::make(const QString &type, qint64 timestampMs, const QJsonObject &payload,
                          const QString &turnId, const QString &stepId)
{
    HarnessEvent event;
    event.type = type;
    event.timestampMs = timestampMs;
    event.turnId = turnId;
    event.stepId = stepId;
    event.payload = payload;
    return event;
}

QJsonObject Events::redact(const QJsonObject &payload)
{
    QJsonObject out;
    for (auto it = payload.constBegin(); it != payload.constEnd(); ++it) {
        if (isSecretKey(it.key())) {
            out.insert(it.key(), redactedMarker());
            continue;
        }
        const QJsonValue value = it.value();
        if (value.isObject()) {
            out.insert(it.key(), redact(value.toObject()));
        } else if (value.isArray()) {
            QJsonArray array;
            for (const QJsonValue &item : value.toArray()) {
                if (item.isObject()) {
                    array.append(redact(item.toObject()));
                } else if (item.isString()) {
                    array.append(redactText(item.toString()));
                } else {
                    array.append(item);
                }
            }
            out.insert(it.key(), array);
        } else if (value.isString()) {
            out.insert(it.key(), redactText(value.toString()));
        } else {
            out.insert(it.key(), value);
        }
    }
    return out;
}

QString Events::redactText(const QString &text)
{
    static const QRegularExpression bearer(uR"((?i)\bBearer\s+[A-Za-z0-9._~+/\-=]{8,})"_s);
    static const QRegularExpression providerKey(
        uR"(\b(?:sk-[A-Za-z0-9_-]{12,}|gh[pousr]_[A-Za-z0-9]{20,}|xox[baprs]-[A-Za-z0-9-]{10,}|AKIA[0-9A-Z]{16})\b)"_s);
    static const QRegularExpression assignment(
        uR"((?i)\b([A-Z0-9_]*(?:API_KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL)[A-Z0-9_]*)\s*=\s*("[^"]*"|'[^']*'|\S+))"_s);

    QString out = text;
    out.replace(bearer, u"Bearer [redacted]"_s);
    out.replace(providerKey, redactedMarker());
    out.replace(assignment, u"\\1=[redacted]"_s);
    return out;
}

QByteArray Events::encode(const QJsonObject &payload)
{
    return QJsonDocument(redact(payload)).toJson(QJsonDocument::Compact);
}

QJsonObject Events::decode(const QByteArray &data)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    return document.isObject() ? document.object() : QJsonObject();
}

void EventProjection::apply(const HarnessEvent &event, QList<ChatMessage> *messages)
{
    if (!messages) {
        return;
    }

    if (event.type == EventTypes::TurnStart) {
        const QString text = event.payload.value(u"userText"_s).toString();
        if (!text.isEmpty()) {
            ChatMessage message;
            message.role = ChatMessage::Role::User;
            message.content = text;
            messages->append(message);
        }
        return;
    }

    if (event.type == EventTypes::AssistantMessage) {
        ChatMessage message;
        message.role = ChatMessage::Role::Assistant;
        message.content = event.payload.value(u"text"_s).toString();
        message.thinking = event.payload.value(u"thinking"_s).toString();
        const QJsonArray calls = event.payload.value(u"toolCalls"_s).toArray();
        for (const QJsonValue &value : calls) {
            const QJsonObject call = value.toObject();
            const QString id = call.value(u"id"_s).toString();
            const QString name = call.value(u"name"_s).toString();
            if (name.isEmpty()) {
                continue;
            }
            QJsonObject function;
            function.insert(u"name"_s, name);
            function.insert(u"arguments"_s,
                            call.value(u"arguments"_s).isString()
                                ? call.value(u"arguments"_s).toString()
                                : QString::fromUtf8(QJsonDocument(call.value(u"arguments"_s).toObject())
                                                        .toJson(QJsonDocument::Compact)));
            QJsonObject encoded;
            encoded.insert(u"id"_s, id);
            encoded.insert(u"type"_s, u"function"_s);
            encoded.insert(u"function"_s, function);
            message.toolCalls.append(encoded);
        }
        messages->append(message);
        return;
    }

    if (event.type == EventTypes::ToolResult) {
        ChatMessage message;
        message.role = ChatMessage::Role::Tool;
        message.toolCallId = event.payload.value(u"toolCallId"_s).toString();
        message.name = event.payload.value(u"name"_s).toString();
        message.content = event.payload.value(u"output"_s).toString();
        messages->append(message);
        return;
    }

    if (event.type == EventTypes::Message) {
        ChatMessage message;
        const int role = event.payload.value(u"role"_s).toInt(static_cast<int>(ChatMessage::Role::User));
        if (role >= static_cast<int>(ChatMessage::Role::System)
            && role <= static_cast<int>(ChatMessage::Role::Tool)) {
            message.role = static_cast<ChatMessage::Role>(role);
        }
        message.content = event.payload.value(u"content"_s).toString();
        message.thinking = event.payload.value(u"thinking"_s).toString();
        message.toolCallId = event.payload.value(u"toolCallId"_s).toString();
        message.name = event.payload.value(u"name"_s).toString();
        if (event.payload.value(u"toolCalls"_s).isArray()) {
            message.toolCalls = event.payload.value(u"toolCalls"_s).toArray();
        }
        messages->append(message);
    }
}

QList<ChatMessage> EventProjection::build(const QList<HarnessEvent> &events)
{
    QList<ChatMessage> messages;
    for (const HarnessEvent &event : events) {
        apply(event, &messages);
    }
    return messages;
}

} // namespace KateAi
