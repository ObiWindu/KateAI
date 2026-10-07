/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QList>
#include <QJsonObject>
#include <QDateTime>

namespace KateAi
{

class SessionStore
{
public:
    static constexpr int CURRENT_VERSION = 2;

    struct SessionData {
        int version = CURRENT_VERSION;
        QList<ChatMessage> messages;
        QString currentThinking;
        QJsonArray currentPlan;
        bool planShown = false;
        QString currentAssistant;
        quint64 stateEpoch = 0;
        QList<QString> actionSignatures;
        QHash<QString, int> actionRepeatCounts;
        QList<QString> changedPaths;
        bool changesNeedVerification = false;
        bool verificationAttempted = false;
        int verificationPromptCount = 0;
        int modelRequests = 0;
        int toolCalls = 0;
        // ACP session id, used to resume the agent conversation.
        QString acpSessionId;
    };

    struct ConversationInfo {
        QString id;
        QString title;
        QDateTime createdAt;
        QDateTime updatedAt;
        int messageCount = 0;
        bool isActive = false;
    };

    // Load the active (most recent) conversation
    static SessionData load();

    // Save the active conversation
    static void save(const SessionData &data, int maxConversations = 50);

    // Deletes the active conversation from storage, history included.
    // Callers that only mean "forget the current state" should not use this.
    static void clear();

    // Conversation history management
    static QList<ConversationInfo> listConversations(int maxConversations = 50);
    static SessionData loadConversation(const QString &conversationId);
    static void saveConversation(const QString &conversationId, const SessionData &data, const QString &title = QString(), int maxConversations = 50);
    static void deleteConversation(const QString &conversationId);
    static QString createNewConversation();
    static void setActiveConversation(const QString &conversationId);
    static QString getActiveConversationId();
    static void pruneOldConversations(int maxConversations);
    static void clearAllConversations();
};

} // namespace KateAi