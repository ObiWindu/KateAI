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
#include <QUuid>
#include <QDateTime>
#include <QDir>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

// Helper functions (defined first so they can be used by static methods)
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

static QString generateConversationId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static QString generateTitleFromMessages(const QList<ChatMessage> &messages)
{
    for (const auto &msg : messages) {
        if (msg.role == ChatMessage::Role::User && !msg.content.isEmpty()) {
            QString title = msg.content.trimmed().split(u'\n').first();
            if (title.length() > 50) {
                title = title.left(47) + u"...";
            }
            return title;
        }
    }
    return QDateTime::currentDateTime().toString(u"yyyy-MM-dd hh:mm"_s);
}

static KConfigGroup conversationsGroup()
{
    return KConfigGroup(KSharedConfig::openConfig(), u"KateAIConversations"_s);
}

static KConfigGroup conversationGroup(const QString &id)
{
    return KConfigGroup(KSharedConfig::openConfig(), u"KateAIConversation_"_s + id);
}

QString SessionStore::conversationsGroupName()
{
    return u"KateAIConversations"_s;
}

QString SessionStore::conversationGroupName(const QString &id)
{
    return u"KateAIConversation_"_s + id;
}

SessionStore::SessionData SessionStore::load()
{
    // Load the active conversation
    const QString activeId = getActiveConversationId();
    if (!activeId.isEmpty()) {
        return loadConversation(activeId);
    }
    return SessionData();
}

void SessionStore::save(const SessionData &data)
{
    const QString activeId = getActiveConversationId();
    if (!activeId.isEmpty()) {
        saveConversation(activeId, data);
    } else {
        // Create a new conversation if none active
        const QString newId = createNewConversation();
        saveConversation(newId, data);
    }
}

void SessionStore::clear()
{
    const QString activeId = getActiveConversationId();
    if (!activeId.isEmpty()) {
        deleteConversation(activeId);
    }
}

QList<SessionStore::ConversationInfo> SessionStore::listConversations(int maxConversations)
{
    QList<ConversationInfo> conversations;
    const KConfigGroup group = conversationsGroup();
    const QStringList keys = group.keyList();

    for (const QString &key : keys) {
        if (key.startsWith(u"conv_"_s)) {
            const QString id = key.mid(5); // Remove "conv_" prefix
            const KConfigGroup convGroup = conversationGroup(id);

            ConversationInfo info;
            info.id = id;
            info.title = convGroup.readEntry(u"Title"_s, u"Untitled"_s);
            info.createdAt = QDateTime::fromString(convGroup.readEntry(u"CreatedAt"_s, QString()), Qt::ISODate);
            info.updatedAt = QDateTime::fromString(convGroup.readEntry(u"UpdatedAt"_s, QString()), Qt::ISODate);
            info.messageCount = convGroup.readEntry(u"MessageCount"_s, 0);
            info.isActive = (id == getActiveConversationId());

            conversations.append(info);
        }
    }

    // Sort by updatedAt descending (most recent first)
    std::sort(conversations.begin(), conversations.end(),
              [](const ConversationInfo &a, const ConversationInfo &b) {
                  return a.updatedAt > b.updatedAt;
              });

    // Limit to maxConversations
    if (maxConversations > 0 && conversations.size() > maxConversations) {
        conversations = conversations.mid(0, maxConversations);
    }

    return conversations;
}

SessionStore::SessionData SessionStore::loadConversation(const QString &conversationId)
{
    SessionData data;
    const KConfigGroup g = conversationGroup(conversationId);

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

void SessionStore::saveConversation(const QString &conversationId, const SessionData &data, const QString &title)
{
    KConfigGroup convGroup = conversationGroup(conversationId);
    KConfigGroup listGroup = conversationsGroup();

    // Generate title if not provided
    QString convTitle = title;
    if (convTitle.isEmpty()) {
        convTitle = generateTitleFromMessages(data.messages);
    }

    // Save conversation data
    convGroup.writeEntry(u"Title"_s, convTitle);
    convGroup.writeEntry(u"UpdatedAt"_s, QDateTime::currentDateTime().toString(Qt::ISODate));
    convGroup.writeEntry(u"MessageCount"_s, data.messages.size());

    // Preserve creation time
    if (convGroup.readEntry(u"CreatedAt"_s, QString()).isEmpty()) {
        convGroup.writeEntry(u"CreatedAt"_s, QDateTime::currentDateTime().toString(Qt::ISODate));
    }

    // Save session data
    convGroup.writeEntry(u"Messages"_s, QJsonDocument(messagesToJson(data.messages)).toJson(QJsonDocument::Compact));
    convGroup.writeEntry(u"CurrentThinking"_s, data.currentThinking);
    convGroup.writeEntry(u"CurrentPlan"_s, QJsonDocument(data.currentPlan).toJson(QJsonDocument::Compact));
    convGroup.writeEntry(u"PlanShown"_s, data.planShown);
    convGroup.writeEntry(u"CurrentAssistant"_s, data.currentAssistant);
    convGroup.writeEntry(u"StateEpoch"_s, data.stateEpoch);
    convGroup.writeEntry(u"ActionSignatures"_s, QJsonDocument(stringListToJson(data.actionSignatures)).toJson(QJsonDocument::Compact));
    convGroup.writeEntry(u"ActionRepeatCounts"_s, QJsonDocument(hashToJson(data.actionRepeatCounts)).toJson(QJsonDocument::Compact));
    convGroup.writeEntry(u"ChangedPaths"_s, QJsonDocument(stringListToJson(data.changedPaths)).toJson(QJsonDocument::Compact));
    convGroup.writeEntry(u"ChangesNeedVerification"_s, data.changesNeedVerification);
    convGroup.writeEntry(u"VerificationAttempted"_s, data.verificationAttempted);
    convGroup.writeEntry(u"VerificationPromptCount"_s, data.verificationPromptCount);
    convGroup.writeEntry(u"ModelRequests"_s, data.modelRequests);
    convGroup.writeEntry(u"ToolCalls"_s, data.toolCalls);

    convGroup.sync();

    // Update conversation list
    listGroup.writeEntry(u"conv_"_s + conversationId, true);
    listGroup.sync();

    // Prune old conversations if needed
    pruneOldConversations(50); // Default, will be overridden by config
}

void SessionStore::deleteConversation(const QString &conversationId)
{
    KConfigGroup convGroup = conversationGroup(conversationId);
    KConfigGroup listGroup = conversationsGroup();

    convGroup.deleteGroup();
    listGroup.deleteEntry(u"conv_"_s + conversationId);

    // If this was the active conversation, clear active
    if (getActiveConversationId() == conversationId) {
        KConfigGroup activeGroup = conversationsGroup();
        activeGroup.deleteEntry(u"ActiveConversation"_s);
        activeGroup.sync();
    }

    convGroup.sync();
    listGroup.sync();
}

QString SessionStore::createNewConversation()
{
    const QString newId = generateConversationId();

    // Create empty conversation
    SessionData emptyData;
    saveConversation(newId, emptyData, u"New Conversation"_s);

    // Set as active
    setActiveConversation(newId);

    return newId;
}

void SessionStore::setActiveConversation(const QString &conversationId)
{
    KConfigGroup group = conversationsGroup();
    group.writeEntry(u"ActiveConversation"_s, conversationId);
    group.sync();
}

QString SessionStore::getActiveConversationId()
{
    const KConfigGroup group = conversationsGroup();
    return group.readEntry(u"ActiveConversation"_s, QString());
}

void SessionStore::pruneOldConversations(int maxConversations)
{
    if (maxConversations <= 0) {
        return; // Unlimited
    }

    auto conversations = listConversations(0); // Get all without limit
    if (conversations.size() <= maxConversations) {
        return;
    }

    // Delete oldest conversations beyond the limit
    for (int i = maxConversations; i < conversations.size(); ++i) {
        deleteConversation(conversations[i].id);
    }
}

void SessionStore::clearAllConversations()
{
    auto conversations = listConversations(0);
    for (const auto &conv : conversations) {
        deleteConversation(conv.id);
    }
}

} // namespace KateAi