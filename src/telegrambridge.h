/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "agentloop.h"
#include "types.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QTimer>

namespace KateAi
{

// Secure bridge between the local AgentLoop and Telegram's Bot API.
//
// Design: outbound long-polling only (getUpdates), no inbound webhook port.
// Fail-closed: start() refuses when the token is missing/malformed or the
// chat allowlist is empty. While running, the loop is forced into a hardened
// configuration (Ask permissions, Strict/Workspace sandbox, no outside
// reads, read-only plan mode unless writes were explicitly allowed), and
// every mutating tool still waits for an explicit Telegram approval with a
// default-deny timeout. The token never appears in logs.
class TelegramBridge : public QObject
{
    Q_OBJECT

public:
    struct Config {
        // Bot token from BotFather. May come from Settings or the
        // KATEAI_TELEGRAM_BOT_TOKEN environment (env wins, never saved).
        QString botToken;
        // Numeric Telegram chat ids allowed to prompt. Empty = refuse to start.
        QSet<qlonglong> allowedChatIds;
        // When false (default) remote turns are forced read-only.
        bool allowWrites = false;
        QString apiBase = QStringLiteral("https://api.telegram.org");
        int pollTimeoutSec = 30;
        int approvalTimeoutSec = 120;
    };

    static Config configFromSettings(const Settings &settings);
    static QString tokenFromEnvironment();

    explicit TelegramBridge(QObject *parent = nullptr);
    ~TelegramBridge() override;

    void setAgentLoop(AgentLoop *loop);
    void setNetwork(QNetworkAccessManager *network);

    bool start(const Config &config, QString *error = nullptr);
    void stop();
    bool isRunning() const { return m_running; }
    qlonglong activeChat() const { return m_activeChat; }

    // --- Pure helpers (unit-tested, no network) ---------------------------
    static QSet<qlonglong> parseAllowedChatIds(const QString &text);
    static bool isChatAllowed(qlonglong chatId, const QSet<qlonglong> &allowed);
    static bool isTokenWellFormed(const QString &token);
    // Removes secret-looking spans before anything is sent to Telegram.
    static QString redactSecrets(const QString &text, const QString &botToken = QString());
    static QString truncateForTelegram(const QString &text, int maxChars = 3500);
    static QJsonObject buildSendMessagePayload(qlonglong chatId, const QString &text,
                                               const QJsonObject &replyMarkup = QJsonObject());
    static QJsonObject buildApprovalKeyboard(const QString &toolCallId);
    // Extracts chat/text/callback from a getUpdates entry. Returns false for
    // entries without a usable chat (edited messages, channel posts, ...).
    static bool parseUpdate(const QJsonObject &update, qlonglong *chatId, QString *text,
                            QString *callbackData, QString *callbackId, qlonglong *updateId);
    static bool isAllowedCommand(const QString &text);
    // Hardened copy of the local settings applied while the bridge runs.
    static Settings hardenedSettingsForRemote(const Settings &in);

Q_SIGNALS:
    void statusMessage(const QString &message);
    void failed(const QString &error);

private Q_SLOTS:
    void pollOnce();
    void onPermissionNeeded(const PermissionRequest &request);
    void onQuestionAsked(const PermissionRequest &request);
    void onAssistantFinished(const QString &text);
    void onTurnFailed(const QString &error);
    void onTurnFinished();
    void onApprovalTimeout();

private:
    void scheduleNextPoll(int delayMs = 0);
    void sendText(qlonglong chatId, const QString &text, const QJsonObject &replyMarkup = QJsonObject());
    void answerCallback(const QString &callbackId);
    void handleMessage(qlonglong chatId, const QString &text);
    void handleCallback(qlonglong chatId, const QString &callbackId, const QString &data);
    void handleUpdatesDocument(const QJsonDocument &doc);
    QUrl updatesUrl() const;
    QUrl methodUrl(const QString &method) const;

    AgentLoop *m_loop = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    bool m_ownsNetwork = false;
    Config m_config;
    bool m_running = false;
    qlonglong m_offset = 0;
    QTimer m_pollTimer;
    QTimer m_approvalTimer;
    // Single-flight: one remote turn at a time, one pending approval.
    qlonglong m_activeChat = 0;
    bool m_awaitingAnswer = false;
    QString m_pendingToolCallId;
    PermissionRequest m_pendingRequest;
    QString m_streamBuffer;
};

} // namespace KateAi
