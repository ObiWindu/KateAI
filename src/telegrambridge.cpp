/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "telegrambridge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

TelegramBridge::Config TelegramBridge::configFromSettings(const Settings &settings)
{
    Config config;
    const QString envToken = tokenFromEnvironment().trimmed();
    config.botToken = envToken.isEmpty() ? settings.telegramBotToken.trimmed() : envToken;
    config.allowedChatIds = parseAllowedChatIds(settings.telegramAllowedChatIds);
    config.allowWrites = settings.telegramAllowWrites;
    return config;
}

QString TelegramBridge::tokenFromEnvironment()
{
    return QString::fromUtf8(qgetenv("KATEAI_TELEGRAM_BOT_TOKEN")).trimmed();
}

TelegramBridge::TelegramBridge(QObject *parent)
    : QObject(parent)
{
    m_pollTimer.setSingleShot(true);
    connect(&m_pollTimer, &QTimer::timeout, this, &TelegramBridge::pollOnce);
    m_approvalTimer.setSingleShot(true);
    connect(&m_approvalTimer, &QTimer::timeout, this, &TelegramBridge::onApprovalTimeout);
}

TelegramBridge::~TelegramBridge()
{
    stop();
}

void TelegramBridge::setAgentLoop(AgentLoop *loop)
{
    if (m_loop) {
        disconnect(m_loop, nullptr, this, nullptr);
    }
    m_loop = loop;
    if (m_loop) {
        connect(m_loop, &AgentLoop::permissionNeeded, this, &TelegramBridge::onPermissionNeeded);
        connect(m_loop, &AgentLoop::questionAsked, this, &TelegramBridge::onQuestionAsked);
        connect(m_loop, &AgentLoop::assistantFinished, this, &TelegramBridge::onAssistantFinished);
        connect(m_loop, &AgentLoop::failed, this, &TelegramBridge::onTurnFailed);
        connect(m_loop, &AgentLoop::turnFinished, this, &TelegramBridge::onTurnFinished);
        connect(m_loop, &AgentLoop::assistantDelta, this, [this](const QString &delta) {
            // Kept bounded: long turns must not grow this without limit.
            if (m_streamBuffer.size() < 20000) {
                m_streamBuffer += delta;
            }
        });
    }
}

void TelegramBridge::setNetwork(QNetworkAccessManager *network)
{
    if (m_ownsNetwork && m_network) {
        m_network->deleteLater();
    }
    m_network = network;
    m_ownsNetwork = false;
}

QSet<qlonglong> TelegramBridge::parseAllowedChatIds(const QString &text)
{
    QSet<qlonglong> ids;
    const QString normalized = QString(text).replace(u';', u',').replace(u'\n', u',');
    for (const QString &part : normalized.split(u',')) {
        for (const QString &piece : part.split(u' ', Qt::SkipEmptyParts)) {
            bool ok = false;
            const qlonglong id = piece.trimmed().toLongLong(&ok);
            if (ok && id != 0) {
                ids.insert(id);
            }
        }
    }
    return ids;
}

bool TelegramBridge::isChatAllowed(qlonglong chatId, const QSet<qlonglong> &allowed)
{
    return chatId != 0 && allowed.contains(chatId);
}

bool TelegramBridge::isTokenWellFormed(const QString &token)
{
    // BotFather tokens look like 123456789:AAH... (35 chars after the colon).
    // Lenient on purpose: the goal is to catch empty/placeholder values, not
    // to fully validate against Telegram's generator.
    const int colon = token.indexOf(u':');
    if (colon <= 0) {
        return false;
    }
    const QString head = token.left(colon);
    const QString tail = token.mid(colon + 1);
    if (head.size() < 5 || tail.size() < 20) {
        return false;
    }
    static const QRegularExpression headRe(u"^[0-9]+$"_s);
    static const QRegularExpression tailRe(u"^[A-Za-z0-9_-]+$"_s);
    if (token.contains(u' ') || token.contains(u'/') || token.contains(u'\n')) {
        return false;
    }
    return headRe.match(head).hasMatch() && tailRe.match(tail).hasMatch();
}

QString TelegramBridge::redactSecrets(const QString &text, const QString &botToken)
{
    QString out = text;
    if (!botToken.isEmpty()) {
        out.replace(botToken, u"[redacted-bot-token]"_s);
    }
    // Private keys first: one pattern covers the whole block.
    out.replace(QRegularExpression(u"-----BEGIN [A-Z ]*PRIVATE KEY-----[\\s\\S]{0,8000}?-----END [A-Z ]*PRIVATE KEY-----"_s),
                u"[redacted-private-key]"_s);
    const QList<QPair<QString, QString>> patterns = {
        {u"(sk-[A-Za-z0-9-_]{8,})"_s, u"[redacted-sk]"_s},
        {u"(xai-[A-Za-z0-9-_]{8,})"_s, u"[redacted-xai-key]"_s},
        {u"(gh[pousr]_[A-Za-z0-9_]{8,}|github_pat_[A-Za-z0-9_]{8,})"_s, u"[redacted-github-token]"_s},
        {u"(AKIA[0-9A-Z]{16})"_s, u"[redacted-aws-key]"_s},
        {u"(?i)\\bbearer\\s+[A-Za-z0-9\\-._~+/=]{8,}"_s, u"Bearer [redacted]"_s},
        {u"(?i)\\bapi[_-]?key\\b\\s*[:=]\\s*\\S+"_s, u"api_key=[redacted]"_s},
    };
    for (const auto &pattern : patterns) {
        out.replace(QRegularExpression(pattern.first), pattern.second);
    }
    return out;
}

QString TelegramBridge::truncateForTelegram(const QString &text, int maxChars)
{
    if (maxChars <= 0 || text.size() <= maxChars) {
        return text;
    }
    return text.left(qMax(0, maxChars - 20)) + u"\n… (truncated)"_s;
}

QJsonObject TelegramBridge::buildSendMessagePayload(qlonglong chatId, const QString &text,
                                                    const QJsonObject &replyMarkup)
{
    QJsonObject payload;
    payload.insert(u"chat_id"_s, chatId);
    payload.insert(u"text"_s, truncateForTelegram(text));
    if (!replyMarkup.isEmpty()) {
        payload.insert(u"reply_markup"_s, replyMarkup);
    }
    return payload;
}

QJsonObject TelegramBridge::buildApprovalKeyboard(const QString &toolCallId)
{
    Q_UNUSED(toolCallId);
    // Single-flight bridge: at most one pending approval exists, so the
    // callback carries no internal id (keeps it under Telegram's 64-byte
    // callback_data limit and leaks nothing about the loop).
    QJsonObject allow;
    allow.insert(u"text"_s, u"✅ Allow once"_s);
    allow.insert(u"callback_data"_s, u"tg:allow"_s);
    QJsonObject deny;
    deny.insert(u"text"_s, u"❌ Deny"_s);
    deny.insert(u"callback_data"_s, u"tg:deny"_s);
    QJsonObject markup;
    markup.insert(u"inline_keyboard"_s, QJsonArray{QJsonArray{allow, deny}});
    return markup;
}

bool TelegramBridge::parseUpdate(const QJsonObject &update, qlonglong *chatId, QString *text,
                                 QString *callbackData, QString *callbackId, qlonglong *updateId)
{
    if (chatId) {
        *chatId = 0;
    }
    if (text) {
        text->clear();
    }
    if (callbackData) {
        callbackData->clear();
    }
    if (callbackId) {
        callbackId->clear();
    }
    if (updateId) {
        *updateId = update.value(u"update_id"_s).toVariant().toLongLong();
    }
    if (update.contains(u"callback_query"_s)) {
        const QJsonObject query = update.value(u"callback_query"_s).toObject();
        if (callbackId) {
            *callbackId = query.value(u"id"_s).toString();
        }
        if (callbackData) {
            *callbackData = query.value(u"data"_s).toString();
        }
        const QJsonObject message = query.value(u"message"_s).toObject();
        const QJsonObject chat = message.value(u"chat"_s).toObject();
        qlonglong id = chat.value(u"id"_s).toVariant().toLongLong();
        if (id == 0) {
            id = query.value(u"from"_s).toObject().value(u"id"_s).toVariant().toLongLong();
        }
        if (chatId) {
            *chatId = id;
        }
        return id != 0;
    }
    if (update.contains(u"message"_s)) {
        const QJsonObject message = update.value(u"message"_s).toObject();
        const qlonglong id = message.value(u"chat"_s).toObject().value(u"id"_s).toVariant().toLongLong();
        if (chatId) {
            *chatId = id;
        }
        if (text) {
            *text = message.value(u"text"_s).toString();
        }
        return id != 0;
    }
    return false;
}

bool TelegramBridge::isAllowedCommand(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (!trimmed.startsWith(u'/')) {
        return true; // Free-form prompt, judged by the agent's tool policy.
    }
    QString first = trimmed.split(u' ', Qt::SkipEmptyParts).value(0);
    const int at = first.indexOf(u'@');
    if (at > 0) {
        first = first.left(at);
    }
    static const QSet<QString> allowed = {u"/start"_s, u"/help"_s, u"/new"_s, u"/stop"_s,
                                         u"/status"_s, u"/ask"_s, u"/approve"_s, u"/deny"_s};
    return allowed.contains(first);
}

Settings TelegramBridge::hardenedSettingsForRemote(const Settings &in)
{
    Settings out = in;
    // Remote prompting never widens local permissions.
    out.permissionMode = PermissionMode::Ask;
    out.allowReadsOutsideWorkspace = false;
    out.autoApproveTools.clear();
    out.completionEnabled = false;
    if (!in.telegramAllowWrites) {
        // Read-only remote: the model only receives read-only tools, so a
        // compromised prompt cannot request a write at all.
        out.planMode = true;
        out.sandbox = SandboxProfile::Strict;
    } else if (out.sandbox == SandboxProfile::Off) {
        out.sandbox = SandboxProfile::Workspace;
    }
    // Bound remote spend even when the local config is unlimited.
    if (out.maxToolCalls <= 0 || out.maxToolCalls > 100) {
        out.maxToolCalls = 50;
    }
    if (out.maxModelRequests <= 0 || out.maxModelRequests > 100) {
        out.maxModelRequests = 25;
    }
    return out;
}

bool TelegramBridge::start(const Config &config, QString *error)
{
    if (m_running) {
        return true;
    }
    if (!m_loop) {
        if (error) {
            *error = u"Telegram bridge has no agent loop attached."_s;
        }
        return false;
    }
    if (!isTokenWellFormed(config.botToken)) {
        if (error) {
            *error = u"Telegram bot token is missing or malformed."_s;
        }
        return false;
    }
    if (config.allowedChatIds.isEmpty()) {
        if (error) {
            *error = u"Telegram allowlist is empty: set at least one chat id."_s;
        }
        return false;
    }
    m_config = config;
    m_offset = 0;
    m_activeChat = 0;
    m_awaitingAnswer = false;
    m_pendingToolCallId.clear();
    m_pendingRequest = {};
    m_streamBuffer.clear();
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
        m_ownsNetwork = true;
    }
    // Hardening is applied by the embedder (ChatWidget applies
    // hardenedSettingsForRemote() to the loop); the bridge only polls.
    m_running = true;
    scheduleNextPoll(0);
    Q_EMIT statusMessage(u"Telegram bridge polling."_s);
    return true;
}

void TelegramBridge::stop()
{
    m_running = false;
    m_pollTimer.stop();
    m_approvalTimer.stop();
    m_pendingToolCallId.clear();
    m_pendingRequest = {};
    m_awaitingAnswer = false;
    Q_EMIT statusMessage(u"Telegram bridge stopped."_s);
}

void TelegramBridge::scheduleNextPoll(int delayMs)
{
    if (!m_running) {
        return;
    }
    m_pollTimer.stop();
    m_pollTimer.start(qMax(0, delayMs));
}

QUrl TelegramBridge::updatesUrl() const
{
    QUrl url(m_config.apiBase + u"/bot"_s + m_config.botToken + u"/getUpdates"_s);
    QUrlQuery query;
    query.addQueryItem(u"timeout"_s, QString::number(qBound(1, m_config.pollTimeoutSec, 90)));
    query.addQueryItem(u"offset"_s, QString::number(m_offset));
    query.addQueryItem(u"allowed_updates"_s, u"[\"message\",\"callback_query\"]"_s);
    url.setQuery(query);
    return url;
}

QUrl TelegramBridge::methodUrl(const QString &method) const
{
    return QUrl(m_config.apiBase + u"/bot"_s + m_config.botToken + u"/"_s + method);
}

void TelegramBridge::sendText(qlonglong chatId, const QString &text, const QJsonObject &replyMarkup)
{
    if (!m_running || !m_network || chatId == 0) {
        return;
    }
    const QString safe = truncateForTelegram(redactSecrets(text, m_config.botToken));
    const QJsonObject payload = buildSendMessagePayload(chatId, safe, replyMarkup);
    QNetworkRequest request(methodUrl(u"sendMessage"_s));
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    QNetworkReply *reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
    });
}

void TelegramBridge::answerCallback(const QString &callbackId)
{
    if (!m_running || !m_network || callbackId.isEmpty()) {
        return;
    }
    QJsonObject payload;
    payload.insert(u"callback_query_id"_s, callbackId);
    QNetworkRequest request(methodUrl(u"answerCallbackQuery"_s));
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    QNetworkReply *reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
    });
}

void TelegramBridge::pollOnce()
{
    if (!m_running || !m_network) {
        return;
    }
    QNetworkRequest request(updatesUrl());
    request.setTransferTimeout(1000 * (qBound(1, m_config.pollTimeoutSec, 90) + 15));
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (!m_running) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            // Never include the URL (it carries the bot token).
            scheduleNextPoll(2000);
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            scheduleNextPoll(1000);
            return;
        }
        handleUpdatesDocument(doc);
        scheduleNextPoll(0);
    });
}

void TelegramBridge::handleUpdatesDocument(const QJsonDocument &doc)
{
    const QJsonObject root = doc.object();
    if (root.value(u"ok"_s).toBool() == false) {
        return;
    }
    const QJsonArray updates = root.value(u"result"_s).toArray();
    for (const QJsonValue &value : updates) {
        if (!value.isObject()) {
            continue;
        }
        qlonglong chatId = 0;
        QString text;
        QString callbackData;
        QString callbackId;
        qlonglong updateId = 0;
        if (!parseUpdate(value.toObject(), &chatId, &text, &callbackData, &callbackId, &updateId)) {
            if (updateId >= m_offset) {
                m_offset = updateId + 1;
            }
            continue;
        }
        if (updateId >= m_offset) {
            m_offset = updateId + 1;
        }
        if (!isChatAllowed(chatId, m_config.allowedChatIds)) {
            continue; // Silent: no oracle for scanners.
        }
        if (!callbackData.isEmpty()) {
            handleCallback(chatId, callbackId, callbackData);
        } else {
            handleMessage(chatId, text);
        }
    }
}

void TelegramBridge::handleMessage(qlonglong chatId, const QString &text)
{
    if (!m_loop) {
        return;
    }
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        sendText(chatId, u"Send a text prompt, or /help for commands."_s);
        return;
    }
    if (!isAllowedCommand(trimmed)) {
        sendText(chatId, u"Unknown command. Available: /ask, /new, /stop, /status, /approve, /deny."_s);
        return;
    }
    const QString lower = trimmed.toLower();
    if (lower == u"/start"_s || lower.startsWith(u"/start "_s) || lower.startsWith(u"/start@"_s) || lower == u"/help"_s
        || lower.startsWith(u"/help "_s)) {
        sendText(chatId,
                 u"KateAI remote.\n/ask <prompt> — run a turn\n/new — new conversation\n/stop — stop the turn\n/status — busy or idle\n"
                 u"Tool writes ask for approval (Allow once / Deny, 2 min default-deny)."_s);
        return;
    }
    if (lower == u"/status"_s) {
        sendText(chatId, m_loop->isBusy() ? u"Busy."_s : u"Idle."_s);
        return;
    }
    if (lower == u"/stop"_s) {
        m_loop->abort();
        m_awaitingAnswer = false;
        sendText(chatId, u"Stopped."_s);
        return;
    }
    if (lower == u"/new"_s) {
        if (m_loop->isBusy()) {
            sendText(chatId, u"A turn is running. /stop first."_s);
            return;
        }
        m_loop->resetConversation();
        m_streamBuffer.clear();
        sendText(chatId, u"New conversation."_s);
        return;
    }
    if (lower == u"/approve"_s || lower == u"/deny"_s) {
        if (m_pendingToolCallId.isEmpty()) {
            sendText(chatId, u"Nothing awaiting approval."_s);
            return;
        }
        if (chatId != m_activeChat) {
            return;
        }
        m_approvalTimer.stop();
        const bool allow = lower.startsWith(u"/approve"_s);
        m_pendingToolCallId.clear();
        m_loop->resolvePermission(allow ? PermissionDecision::AllowOnce : PermissionDecision::Deny);
        return;
    }
    // An answer to the agent's question arrives as the next message.
    if (m_awaitingAnswer && chatId == m_activeChat) {
        m_awaitingAnswer = false;
        m_approvalTimer.stop();
        m_loop->answerQuestion(trimmed);
        return;
    }
    QString prompt = trimmed;
    if (prompt.startsWith(u"/ask "_s)) {
        prompt = prompt.mid(5).trimmed();
    } else if (prompt.startsWith(u"/ask@"_s)) {
        const int space = prompt.indexOf(u' ');
        prompt = space > 0 ? prompt.mid(space + 1).trimmed() : QString();
    }
    if (prompt.isEmpty() || prompt == u"/ask"_s) {
        sendText(chatId, u"Usage: /ask <prompt>"_s);
        return;
    }
    if (m_loop->isBusy()) {
        sendText(chatId, u"A turn is already running. /stop to interrupt."_s);
        return;
    }
    m_activeChat = chatId;
    m_streamBuffer.clear();
    m_loop->createCheckpoint(u"telegram prompt"_s);
    // Provenance prefix keeps the shared transcript attributable.
    m_loop->start(u"[telegram "_s + QString::number(chatId) + u"] "_s + prompt);
}

void TelegramBridge::handleCallback(qlonglong chatId, const QString &callbackId, const QString &data)
{
    answerCallback(callbackId);
    if (!m_loop || m_pendingToolCallId.isEmpty() || chatId != m_activeChat) {
        return;
    }
    if (data == u"tg:allow"_s) {
        m_approvalTimer.stop();
        m_pendingToolCallId.clear();
        m_loop->resolvePermission(PermissionDecision::AllowOnce);
    } else if (data == u"tg:deny"_s) {
        m_approvalTimer.stop();
        m_pendingToolCallId.clear();
        m_loop->resolvePermission(PermissionDecision::Deny);
    }
}

void TelegramBridge::onPermissionNeeded(const PermissionRequest &request)
{
    if (!m_running || m_activeChat == 0 || !isChatAllowed(m_activeChat, m_config.allowedChatIds)) {
        return; // Local turn: the Kate panel approves it.
    }
    m_pendingToolCallId = request.toolCallId;
    m_pendingRequest = request;
    QString message = u"🔐 Approval needed\n"_s + request.toolName;
    if (!request.summary.isEmpty()) {
        message += u"\n"_s + request.summary;
    }
    if (!request.path.isEmpty()) {
        message += u"\n"_s + request.path;
    }
    if (!request.details.isEmpty()) {
        message += u"\n"_s + request.details.left(800);
    }
    sendText(m_activeChat, message, buildApprovalKeyboard(request.toolCallId));
    m_approvalTimer.stop();
    m_approvalTimer.start(1000 * qBound(10, m_config.approvalTimeoutSec, 600));
}

void TelegramBridge::onQuestionAsked(const PermissionRequest &request)
{
    if (!m_running || m_activeChat == 0 || !isChatAllowed(m_activeChat, m_config.allowedChatIds)) {
        return;
    }
    m_awaitingAnswer = true;
    QString message = u"❓ "_s + request.questionText;
    if (!request.questionOptions.isEmpty()) {
        message += u"\n"_s + request.questionOptions.join(u" / "_s);
    }
    sendText(m_activeChat, message);
    m_approvalTimer.stop();
    m_approvalTimer.start(1000 * qBound(10, m_config.approvalTimeoutSec, 600));
}

void TelegramBridge::onAssistantFinished(const QString &text)
{
    // Buffer the latest model answer; it is delivered on turnFinished so a
    // multi-step turn sends one message instead of one per model round-trip.
    if (m_running && m_activeChat != 0) {
        m_streamBuffer = text;
    }
}

void TelegramBridge::onTurnFailed(const QString &error)
{
    if (!m_running || m_activeChat == 0) {
        return;
    }
    sendText(m_activeChat, u"❌ "_s + error);
}

void TelegramBridge::onTurnFinished()
{
    if (!m_running || m_activeChat == 0) {
        m_pendingToolCallId.clear();
        m_awaitingAnswer = false;
        return;
    }
    m_approvalTimer.stop();
    m_pendingToolCallId.clear();
    m_awaitingAnswer = false;
    if (!m_streamBuffer.trimmed().isEmpty()) {
        sendText(m_activeChat, m_streamBuffer);
    } else {
        sendText(m_activeChat, u"Done."_s);
    }
    m_streamBuffer.clear();
    m_activeChat = 0;
}

void TelegramBridge::onApprovalTimeout()
{
    if (!m_loop) {
        return;
    }
    if (!m_pendingToolCallId.isEmpty()) {
        m_pendingToolCallId.clear();
        sendText(m_activeChat, u"Approval timed out — denied."_s);
        m_loop->resolvePermission(PermissionDecision::Deny);
    } else if (m_awaitingAnswer) {
        m_awaitingAnswer = false;
        sendText(m_activeChat, u"Answer timed out — cancelled."_s);
        m_loop->answerQuestion(QString());
    }
}

} // namespace KateAi
