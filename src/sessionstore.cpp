/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sessionstore.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

static KConfigGroup sessionGroup()
{
    return KConfigGroup(KSharedConfig::openConfig(), u"KateAISession"_s);
}

static QJsonArray messagesToJson(const QList<ChatMessage> &messages)
{
    QJsonArray arr;
    for (const auto &msg : messages) {
        QJsonObject obj;
        obj[u"role"_s] = static_cast<int>(msg.role);
        obj[u"content"_s] = msg.content;
        obj[u"thinking"_s] = msg.thinking;
        obj[u"plan"_s] = msg.plan;
        obj[u"toolCallId"_s] = msg.toolCallId;
        obj[u"name"_s] = msg.name;
        obj[u"toolCalls"_s] = msg.toolCalls;
        arr.append(obj);
    }
    return arr;
}

static QList<ChatMessage> messagesFromJson(const QJsonArray &arr)
{
    QList<ChatMessage> messages;
    for (const auto &val : arr) {
        const QJsonObject obj = val.toObject();
        ChatMessage msg;
        msg.role = static_cast<ChatMessage::Role>(obj[u"role"_s].toInt(static_cast<int>(ChatMessage::Role::User)));
        msg.content = obj[u"content"_s].toString();
        msg.thinking = obj[u"thinking"_s].toString();
        msg.plan = obj[u"plan"_s].toArray();
        msg.toolCallId = obj[u"toolCallId"_s].toString();
        msg.name = obj[u"name"_s].toString();
        msg.toolCalls = obj[u"toolCalls"_s].toArray();
        messages.append(msg);
    }
    return messages;
}

static QJsonArray stringListToJson(const QList<QString> &list)
{
    QJsonArray arr;
    for (const auto &s : list) {
        arr.append(s);
    }
    return arr;
}

static QList<QString> stringListFromJson(const QJsonArray &arr)
{
    QList<QString> list;
    for (const auto &val : arr) {
        list.append(val.toString());
    }
    return list;
}

static QJsonObject hashToJson(const QHash<QString, int> &hash)
{
    QJsonObject obj;
    for (auto it = hash.constBegin(); it != hash.constEnd(); ++it) {
        obj[it.key()] = it.value();
    }
    return obj;
}

static QHash<QString, int> hashFromJson(const QJsonObject &obj)
{
    QHash<QString, int> hash;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        hash[it.key()] = it.value().toInt();
    }
    return hash;
}

SessionStore::SessionData SessionStore::load()
{
    SessionData data;
    const KConfigGroup g = sessionGroup();

    // Load messages
    const QByteArray messagesData = g.readEntry(u"Messages"_s, QByteArray());
    if (!messagesData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(messagesData);
        if (!doc.isNull() && doc.isArray()) {
            data.messages = messagesFromJson(doc.array());
        }
    }

    // Load other session state
    data.currentThinking = g.readEntry(u"CurrentThinking"_s, QString());
    const QByteArray planData = g.readEntry(u"CurrentPlan"_s, QByteArray());
    if (!planData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(planData);
        if (!doc.isNull() && doc.isArray()) {
            data.currentPlan = doc.array();
        }
    }
    data.planShown = g.readEntry(u"PlanShown"_s, false);
    data.currentAssistant = g.readEntry(u"CurrentAssistant"_s, QString());
    data.stateEpoch = g.readEntry(u"StateEpoch"_s, quint64(0));

    const QByteArray actionSigsData = g.readEntry(u"ActionSignatures"_s, QByteArray());
    if (!actionSigsData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(actionSigsData);
        if (!doc.isNull() && doc.isArray()) {
            data.actionSignatures = stringListFromJson(doc.array());
        }
    }

    const QByteArray actionCountsData = g.readEntry(u"ActionRepeatCounts"_s, QByteArray());
    if (!actionCountsData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(actionCountsData);
        if (!doc.isNull() && doc.isObject()) {
            data.actionRepeatCounts = hashFromJson(doc.object());
        }
    }

    const QByteArray changedPathsData = g.readEntry(u"ChangedPaths"_s, QByteArray());
    if (!changedPathsData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(changedPathsData);
        if (!doc.isNull() && doc.isArray()) {
            data.changedPaths = stringListFromJson(doc.array());
        }
    }

    data.changesNeedVerification = g.readEntry(u"ChangesNeedVerification"_s, false);
    data.verificationAttempted = g.readEntry(u"VerificationAttempted"_s, false);
    data.verificationPromptCount = g.readEntry(u"VerificationPromptCount"_s, 0);
    data.modelRequests = g.readEntry(u"ModelRequests"_s, 0);
    data.toolCalls = g.readEntry(u"ToolCalls"_s, 0);

    return data;
}

void SessionStore::save(const SessionData &data)
{
    KConfigGroup g = sessionGroup();

    g.writeEntry(u"Messages"_s, QJsonDocument(messagesToJson(data.messages)).toJson(QJsonDocument::Compact));
    g.writeEntry(u"CurrentThinking"_s, data.currentThinking);
    g.writeEntry(u"CurrentPlan"_s, QJsonDocument(data.currentPlan).toJson(QJsonDocument::Compact));
    g.writeEntry(u"PlanShown"_s, data.planShown);
    g.writeEntry(u"CurrentAssistant"_s, data.currentAssistant);
    g.writeEntry(u"StateEpoch"_s, data.stateEpoch);
    g.writeEntry(u"ActionSignatures"_s, QJsonDocument(stringListToJson(data.actionSignatures)).toJson(QJsonDocument::Compact));
    g.writeEntry(u"ActionRepeatCounts"_s, QJsonDocument(hashToJson(data.actionRepeatCounts)).toJson(QJsonDocument::Compact));
    g.writeEntry(u"ChangedPaths"_s, QJsonDocument(stringListToJson(data.changedPaths)).toJson(QJsonDocument::Compact));
    g.writeEntry(u"ChangesNeedVerification"_s, data.changesNeedVerification);
    g.writeEntry(u"VerificationAttempted"_s, data.verificationAttempted);
    g.writeEntry(u"VerificationPromptCount"_s, data.verificationPromptCount);
    g.writeEntry(u"ModelRequests"_s, data.modelRequests);
    g.writeEntry(u"ToolCalls"_s, data.toolCalls);

    g.sync();
}

void SessionStore::clear()
{
    KConfigGroup g = sessionGroup();
    g.deleteGroup();
    g.sync();
}

} // namespace KateAi