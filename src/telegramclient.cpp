/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "telegramclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QDebug>
#include <QDateTime>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

TelegramClient::TelegramClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_nam, &QNetworkAccessManager::finished, this, &TelegramClient::handleNetworkReply);
    connect(&m_pollingTimer, &QTimer::timeout, this, &TelegramClient::processUpdates);
}

TelegramClient::~TelegramClient()
{
    stopPollingUpdates();
    abort();
}

void TelegramClient::setSettings(const Settings &settings)
{
    m_settings = settings;
}

bool TelegramClient::isConfigured() const
{
    return !m_settings.telegramBotToken.isEmpty();
}

QString TelegramClient::buildApiUrl(const QString &method) const
{
    return u"https://api.telegram.org/bot%1/%2"_s.arg(m_settings.telegramBotToken, method);
}

void TelegramClient::sendMessage(qint64 chatId, const QString &text)
{
    if (!isConfigured()) {
        Q_EMIT messageSendFailed(chatId, u"Telegram bot token not configured"_s);
        return;
    }

    QJsonObject request = buildSendMessageRequest(chatId, text);
    QNetworkRequest req(QUrl(buildApiUrl(u"sendMessage"_s)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(request).toJson());
    m_pendingReplies.append(reply);
    
    // Track this as a regular message
    reply->setProperty("chatId", chatId);
    reply->setProperty("text", text);
    reply->setProperty("isBroadcast", false);
    reply->setProperty("isProactive", false);
    reply->setProperty("isReply", false);
    reply->setProperty("requestType", u"sendMessage"_s);
}

void TelegramClient::broadcastMessage(const QStringList &recipientIds, const QString &text)
{
    if (!isConfigured()) {
        Q_EMIT broadcastComplete(0, recipientIds.size(), u"Telegram bot token not configured"_s);
        return;
    }

    if (recipientIds.isEmpty()) {
        Q_EMIT broadcastComplete(0, 0, u"No recipients specified"_s);
        return;
    }

    // Reset broadcast tracking
    m_broadcastTotal = recipientIds.size();
    m_broadcastSuccess = 0;
    m_broadcastFail = 0;
    m_broadcastErrors.clear();
    m_currentBroadcastIndex = 0;

    Q_EMIT broadcastProgress(0, m_broadcastTotal, 0);

    // Send to all recipients
    for (const QString &recipientId : recipientIds) {
        bool ok;
        qint64 chatId = recipientId.toLongLong(&ok);
        if (ok && chatId != 0) {
            sendBroadcastToSingleRecipient(chatId, text);
        } else {
            m_broadcastFail++;
            m_broadcastErrors << u"Invalid chat ID: %1"_s.arg(recipientId);
            Q_EMIT broadcastProgress(m_currentBroadcastIndex + 1, m_broadcastTotal, chatId);
        }
        m_currentBroadcastIndex++;
    }
}

void TelegramClient::broadcastMessage(const QString &text)
{
    broadcastMessage(m_settings.telegramBroadcastRecipients, text);
}

void TelegramClient::sendProactiveMessage(qint64 chatId, const QString &text)
{
    if (!isConfigured()) {
        Q_EMIT proactiveMessageSent(chatId, QString(), u"Failed: Bot token not configured"_s);
        return;
    }

    QJsonObject request = buildSendMessageRequest(chatId, text);
    QNetworkRequest req(QUrl(buildApiUrl(u"sendMessage"_s)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(request).toJson());
    m_pendingReplies.append(reply);
    
    // Track this as a proactive message
    reply->setProperty("chatId", chatId);
    reply->setProperty("text", text);
    reply->setProperty("isBroadcast", false);
    reply->setProperty("isProactive", true);
    reply->setProperty("isReply", false);
    reply->setProperty("requestType", u"sendMessage"_s);
}

void TelegramClient::sendReply(qint64 chatId, qint64 messageId, const QString &text)
{
    if (!isConfigured()) {
        Q_EMIT messageSendFailed(chatId, u"Telegram bot token not configured"_s);
        return;
    }

    QJsonObject request = buildSendMessageRequest(chatId, text, messageId);
    QNetworkRequest req(QUrl(buildApiUrl(u"sendMessage"_s)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(request).toJson());
    m_pendingReplies.append(reply);
    
    // Track this as a reply
    reply->setProperty("chatId", chatId);
    reply->setProperty("text", text);
    reply->setProperty("isBroadcast", false);
    reply->setProperty("isProactive", false);
    reply->setProperty("isReply", true);
    reply->setProperty("replyToMessageId", messageId);
    reply->setProperty("requestType", u"sendMessage"_s);
}

void TelegramClient::getMe()
{
    if (!isConfigured()) {
        Q_EMIT botInfoFailed(u"Telegram bot token not configured"_s);
        return;
    }

    QNetworkRequest req(QUrl(buildApiUrl(u"getMe"_s)));
    QNetworkReply *reply = m_nam.get(req);
    m_pendingReplies.append(reply);
    reply->setProperty("requestType", u"getMe"_s);
}

void TelegramClient::startPollingUpdates(int timeoutSeconds)
{
    if (m_pollingActive) {
        return;
    }

    m_pollingActive = true;
    m_pollingTimeout = timeoutSeconds;
    m_lastUpdateId = 0;
    
    Q_EMIT pollingStarted();
    processUpdates();
}

void TelegramClient::stopPollingUpdates()
{
    if (!m_pollingActive) {
        return;
    }

    m_pollingActive = false;
    m_pollingTimer.stop();
    
    // Cancel any pending update request
    for (QPointer<QNetworkReply> reply : m_pendingReplies) {
        if (reply && reply->property("requestType").toString() == u"getUpdates"_s) {
            reply->abort();
            cleanupReply(reply);
        }
    }
    
    Q_EMIT pollingStopped();
}

void TelegramClient::processUpdates()
{
    if (!m_pollingActive || !isConfigured()) {
        return;
    }

    QJsonObject request = buildGetUpdatesRequest(m_lastUpdateId, m_pollingTimeout, 100);
    QNetworkRequest req(QUrl(buildApiUrl(u"getUpdates"_s)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(request).toJson());
    m_pendingReplies.append(reply);
    reply->setProperty("requestType", u"getUpdates"_s);
    
    // Set up the polling timer for the next round
    if (m_pollingTimeout > 0) {
        m_pollingTimer.start(m_pollingTimeout * 1000);
    }
}

void TelegramClient::addBroadcastRecipient(qint64 chatId)
{
    QString chatIdStr = QString::number(chatId);
    if (!m_settings.telegramBroadcastRecipients.contains(chatIdStr)) {
        // Note: This modifies a copy, the actual settings need to be saved separately
        m_settings.telegramBroadcastRecipients.append(chatIdStr);
    }
}

void TelegramClient::addBroadcastRecipient(const QString &chatIdStr)
{
    if (!chatIdStr.isEmpty() && !m_settings.telegramBroadcastRecipients.contains(chatIdStr)) {
        m_settings.telegramBroadcastRecipients.append(chatIdStr);
    }
}

void TelegramClient::removeBroadcastRecipient(qint64 chatId)
{
    QString chatIdStr = QString::number(chatId);
    m_settings.telegramBroadcastRecipients.removeAll(chatIdStr);
}

void TelegramClient::removeBroadcastRecipient(const QString &chatIdStr)
{
    m_settings.telegramBroadcastRecipients.removeAll(chatIdStr);
}

QStringList TelegramClient::broadcastRecipients() const
{
    return m_settings.telegramBroadcastRecipients;
}

bool TelegramClient::isBusy() const
{
    for (const QPointer<QNetworkReply> &reply : m_pendingReplies) {
        if (reply && !reply->isFinished()) {
            return true;
        }
    }
    return false;
}

void TelegramClient::abort()
{
    for (QPointer<QNetworkReply> reply : m_pendingReplies) {
        if (reply) {
            reply->abort();
            cleanupReply(reply);
        }
    }
    m_pendingReplies.clear();
    
    stopPollingUpdates();
}

void TelegramClient::sendBroadcastToSingleRecipient(qint64 chatId, const QString &text)
{
    QJsonObject request = buildSendMessageRequest(chatId, text);
    QNetworkRequest req(QUrl(buildApiUrl(u"sendMessage"_s)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(request).toJson());
    m_pendingReplies.append(reply);
    
    // Track this as a broadcast message
    reply->setProperty("chatId", chatId);
    reply->setProperty("text", text);
    reply->setProperty("isBroadcast", true);
    reply->setProperty("isProactive", false);
    reply->setProperty("isReply", false);
    reply->setProperty("requestType", u"broadcastMessage"_s);
}

QJsonObject TelegramClient::buildSendMessageRequest(qint64 chatId, const QString &text, qint64 replyToMessageId) const
{
    QJsonObject request;
    request[u"chat_id"_s] = chatId;
    request[u"text"_s] = text;
    
    // Add parse mode for better formatting
    request[u"parse_mode"_s] = u"HTML"_s;
    
    if (replyToMessageId != 0) {
        request[u"reply_to_message_id"_s] = replyToMessageId;
    }
    
    return request;
}

QJsonObject TelegramClient::buildGetUpdatesRequest(qint64 offset, int timeout, int limit) const
{
    QJsonObject request;
    if (offset > 0) {
        request[u"offset"_s] = offset;
    }
    if (timeout > 0) {
        request[u"timeout"_s] = timeout;
    }
    if (limit > 0) {
        request[u"limit"_s] = limit;
    }
    return request;
}

void TelegramClient::handleNetworkReply()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }

    QString requestType = reply->property("requestType").toString();
    
    if (requestType == u"getMe"_s) {
        handleGetMeReply(reply);
    } else if (requestType == u"getUpdates"_s) {
        handleUpdatesReply(reply);
    } else if (requestType == u"sendMessage"_s || requestType == u"broadcastMessage"_s) {
        // Handle message send requests
        qint64 chatId = reply->property("chatId").toLongLong();
        QString text = reply->property("text").toString();
        bool isBroadcast = reply->property("isBroadcast").toBool();
        bool isProactive = reply->property("isProactive").toBool();
        bool isReply = reply->property("isReply").toBool();
        
        if (isBroadcast) {
            handleBroadcastReply(reply, chatId, text);
        } else if (isProactive) {
            handleSendMessageReply(reply, chatId, text, isReply, true);
        } else {
            handleSendMessageReply(reply, chatId, text, isReply, false);
        }
    }

    cleanupReply(reply);
}

void TelegramClient::handleSendMessageReply(QNetworkReply *reply, qint64 chatId, const QString &text, bool isReply, bool isProactive)
{
    if (reply->error() != QNetworkReply::NoError) {
        QString error = reply->errorString();
        if (error.isEmpty()) {
            error = u"Unknown network error"_s;
        }
        
        if (isProactive) {
            Q_EMIT proactiveMessageSent(chatId, QString(), u"Failed: %1"_s.arg(error));
        } else {
            Q_EMIT messageSendFailed(chatId, error);
        }
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        QString error = u"Invalid JSON response: %1"_s.arg(QString::fromUtf8(data));
        if (isProactive) {
            Q_EMIT proactiveMessageSent(chatId, QString(), u"Failed: %1"_s.arg(error));
        } else {
            Q_EMIT messageSendFailed(chatId, error);
        }
        return;
    }

    QJsonObject response = doc.object();
    if (!response.contains(u"ok"_s) || !response[u"ok"_s].toBool()) {
        QString error = response[u"description"_s].toString();
        if (error.isEmpty()) {
            error = u"Telegram API error"_s;
        }
        
        if (isProactive) {
            Q_EMIT proactiveMessageSent(chatId, QString(), u"Failed: %1"_s.arg(error));
        } else {
            Q_EMIT messageSendFailed(chatId, error);
        }
        return;
    }

    QJsonObject result = response[u"result"_s].toObject();
    QString messageId = QString::number(result[u"message_id"_s].toVariant().toLongLong());
    
    if (isProactive) {
        Q_EMIT proactiveMessageSent(chatId, messageId, text);
    } else {
        Q_EMIT messageSent(chatId, messageId, text);
    }
}

void TelegramClient::handleBroadcastReply(QNetworkReply *reply, qint64 chatId, const QString &text)
{
    if (reply->error() != QNetworkReply::NoError) {
        QString error = reply->errorString();
        if (error.isEmpty()) {
            error = u"Unknown network error"_s;
        }
        m_broadcastFail++;
        m_broadcastErrors << u"Chat %1: %2"_s.arg(QString::number(chatId), error);
        Q_EMIT broadcastProgress(m_currentBroadcastIndex, m_broadcastTotal, chatId);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        m_broadcastFail++;
        m_broadcastErrors << u"Chat %1: Invalid JSON response"_s.arg(QString::number(chatId));
        Q_EMIT broadcastProgress(m_currentBroadcastIndex, m_broadcastTotal, chatId);
        return;
    }

    QJsonObject response = doc.object();
    if (!response.contains(u"ok"_s) || !response[u"ok"_s].toBool()) {
        QString error = response[u"description"_s].toString();
        if (error.isEmpty()) {
            error = u"Telegram API error"_s;
        }
        m_broadcastFail++;
        m_broadcastErrors << u"Chat %1: %2"_s.arg(QString::number(chatId), error);
        Q_EMIT broadcastProgress(m_currentBroadcastIndex, m_broadcastTotal, chatId);
        return;
    }

    m_broadcastSuccess++;
    Q_EMIT broadcastProgress(m_currentBroadcastIndex, m_broadcastTotal, chatId);

    // Check if this was the last broadcast message
    if (m_currentBroadcastIndex >= m_broadcastTotal) {
        QString errorSummary = m_broadcastErrors.join(u"\n"_s);
        Q_EMIT broadcastComplete(m_broadcastSuccess, m_broadcastFail, errorSummary);
    }
}

void TelegramClient::handleGetMeReply(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        QString error = reply->errorString();
        if (error.isEmpty()) {
            error = u"Unknown network error"_s;
        }
        Q_EMIT botInfoFailed(error);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        Q_EMIT botInfoFailed(u"Invalid JSON response"_s);
        return;
    }

    QJsonObject response = doc.object();
    if (!response.contains(u"ok"_s) || !response[u"ok"_s].toBool()) {
        QString error = response[u"description"_s].toString();
        if (error.isEmpty()) {
            error = u"Telegram API error"_s;
        }
        Q_EMIT botInfoFailed(error);
        return;
    }

    QJsonObject result = response[u"result"_s].toObject();
    Q_EMIT botInfoReceived(result);
}

void TelegramClient::handleUpdatesReply(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        QString error = reply->errorString();
        if (error.isEmpty()) {
            error = u"Unknown network error"_s;
        }
        Q_EMIT pollingError(error);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        Q_EMIT pollingError(u"Invalid JSON response"_s);
        return;
    }

    QJsonObject response = doc.object();
    if (!response.contains(u"ok"_s) || !response[u"ok"_s].toBool()) {
        QString error = response[u"description"_s].toString();
        if (error.isEmpty()) {
            error = u"Telegram API error"_s;
        }
        Q_EMIT pollingError(error);
        return;
    }

    QJsonArray updates = response[u"result"_s].toArray();
    
    for (const QJsonValue &updateVal : updates) {
        QJsonObject update = updateVal.toObject();
        Q_EMIT updateReceived(update);
        
        // Process message updates
        if (update.contains(u"message"_s)) {
            QJsonObject message = update[u"message"_s].toObject();
            if (message.contains(u"chat"_s) && message.contains(u"text"_s)) {
                QJsonObject chat = message[u"chat"_s].toObject();
                qint64 chatId = chat[u"id"_s].toVariant().toLongLong();
                qint64 messageId = message[u"message_id"_s].toVariant().toLongLong();
                QString text = message[u"text"_s].toString();
                
                // Extract sender information
                QString sender;
                if (message.contains(u"from"_s)) {
                    QJsonObject from = message[u"from"_s].toObject();
                    if (from.contains(u"username"_s)) {
                        sender = from[u"username"_s].toString();
                    } else if (from.contains(u"first_name"_s)) {
                        sender = from[u"first_name"_s].toString();
                    }
                }
                
                Q_EMIT messageReceived(chatId, messageId, text, sender);
            }
        }
        
        // Update the last update ID for the next poll
        if (update.contains(u"update_id"_s)) {
            qint64 updateId = update[u"update_id"_s].toVariant().toLongLong();
            if (updateId > m_lastUpdateId) {
                m_lastUpdateId = updateId;
            }
        }
    }
    
    // If we got any updates and polling is active, schedule the next poll
    if (!updates.isEmpty() && m_pollingActive && m_pollingTimeout == 0) {
        processUpdates();
    }
}

void TelegramClient::cleanupReply(QNetworkReply *reply)
{
    if (!reply) {
        return;
    }

    reply->deleteLater();
    m_pendingReplies.removeAll(reply);
}

} // namespace KateAi