/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace KateAi
{

class TelegramClient : public QObject
{
    Q_OBJECT

public:
    explicit TelegramClient(QObject *parent = nullptr);
    ~TelegramClient() override;

    void setSettings(const Settings &settings);
    Settings settings() const { return m_settings; }

    // Check if bot token is valid
    bool isConfigured() const;

    // Send a message to a specific chat/user
    void sendMessage(qint64 chatId, const QString &text);

    // Send a message to multiple recipients (broadcast)
    void broadcastMessage(const QStringList &recipientIds, const QString &text);
    void broadcastMessage(const QString &text); // Uses configured broadcast recipients

    // Proactive messaging - bot contacts user on its own
    void sendProactiveMessage(qint64 chatId, const QString &text);

    // Reply to a message (for conversation context)
    void sendReply(qint64 chatId, qint64 messageId, const QString &text);

    // Get bot information
    void getMe();

    // Get updates (for receiving messages)
    void startPollingUpdates(int timeoutSeconds = 30);
    void stopPollingUpdates();

    // Manage broadcast recipients
    void addBroadcastRecipient(qint64 chatId);
    void addBroadcastRecipient(const QString &chatIdStr);
    void removeBroadcastRecipient(qint64 chatId);
    void removeBroadcastRecipient(const QString &chatIdStr);
    QStringList broadcastRecipients() const;

    bool isBusy() const;
    void abort();

Q_SIGNALS:
    // Message sending results
    void messageSent(qint64 chatId, const QString &messageId, const QString &text);
    void messageSendFailed(qint64 chatId, const QString &error);
    
    // Broadcast results
    void broadcastComplete(int successCount, int failCount, const QString &errorSummary);
    void broadcastProgress(int current, int total, qint64 currentChatId);
    
    // Proactive messaging
    void proactiveMessageSent(qint64 chatId, const QString &messageId, const QString &text);
    
    // Incoming messages (from polling)
    void messageReceived(qint64 chatId, qint64 messageId, const QString &text, const QString &sender);
    void updateReceived(const QJsonObject &update);
    
    // Bot info
    void botInfoReceived(const QJsonObject &botInfo);
    void botInfoFailed(const QString &error);
    
    // Status signals
    void pollingStarted();
    void pollingStopped();
    void pollingError(const QString &error);

private:

    QString buildApiUrl(const QString &method) const;
    void processUpdates();
    void handleNetworkReply();
    void handleSendMessageReply(QNetworkReply *reply, qint64 chatId, const QString &text, bool isReply, bool isProactive);
    void handleBroadcastReply(QNetworkReply *reply, qint64 chatId, const QString &text);
    void handleGetMeReply(QNetworkReply *reply);
    void handleUpdatesReply(QNetworkReply *reply);
    void sendBroadcastToSingleRecipient(qint64 chatId, const QString &text);
    
    QJsonObject buildSendMessageRequest(qint64 chatId, const QString &text, qint64 replyToMessageId = 0) const;
    QJsonObject buildGetUpdatesRequest(qint64 offset = 0, int timeout = 0, int limit = 100) const;

    Settings m_settings;
    QNetworkAccessManager m_nam;
    QList<QPointer<QNetworkReply>> m_pendingReplies;
    QTimer m_pollingTimer;
    qint64 m_lastUpdateId = 0;
    bool m_pollingActive = false;
    int m_pollingTimeout = 30;

    // Track pending broadcast messages
    int m_broadcastTotal = 0;
    int m_broadcastSuccess = 0;
    int m_broadcastFail = 0;
    QStringList m_broadcastErrors;
    int m_currentBroadcastIndex = 0;

    void cleanupReply(QNetworkReply *reply);
};

} // namespace KateAi
