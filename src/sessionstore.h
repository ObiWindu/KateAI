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
    static constexpr int CURRENT_VERSION = 4;

    // The SQLite file backing the store. Exposed so the harness event log can
    // live in the same database without duplicating the path policy.
    static QString databasePath();

    struct SessionData {
        int version = CURRENT_VERSION;
        // The history as the model sees it. Context compaction replaces older
        // turns with a summary here, so this list can be shorter than the
        // transcript the user was reading.
        QList<ChatMessage> messages;
        // The full conversation the transcript should render. Compaction does
        // not touch it, so reopening a long thread still shows every turn.
        // Payloads written before this field exist leave it empty; callers
        // then fall back to `messages`.
        QList<ChatMessage> transcript;
        QString currentThinking;
        QJsonArray currentPlan;
        bool planShown = false;
        QString currentAssistant;
        // Identifier of the last user turn. Persisted so a restored transcript
        // and the live runtime agree on which turn the UI is describing.
        QString lastTurnId;
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
        // The editor folder active when this conversation was saved.
        QString workspace;
        // Set when this conversation was forked from another one. Additive and
        // not required for loading; old payloads leave it empty.
        QString parentConversationId;
        // Set when the stored payload could not be read or parsed. It is not
        // persisted; callers use it to avoid treating a damaged transcript as
        // an empty one and overwriting it on the next save.
        bool loadFailed = false;
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
    // The title of a single conversation, or an empty string when it does not
    // exist. Cheaper than listing the whole history for one title.
    static QString conversationTitle(const QString &conversationId);
    static SessionData loadConversation(const QString &conversationId);
    // Returns true when the conversation did not exist yet and was created by
    // this call. The caller uses it to know whether the number of stored
    // conversations changed.
    static bool saveConversation(const QString &conversationId, const SessionData &data, const QString &title = QString(), int maxConversations = 50);
    static void deleteConversation(const QString &conversationId);
    static QString createNewConversation();
    // Copies a conversation into a new active conversation with a parent link.
    // The fork keeps the full transcript; the event log is not copied (messages
    // are the fast-load path), and the source is left untouched.
    static QString forkConversation(const QString &sourceId, const QString &title = QString());
    static void setActiveConversation(const QString &conversationId);
    static QString getActiveConversationId();
    static void pruneOldConversations(int maxConversations);
    static void clearAllConversations();
};

} // namespace KateAi