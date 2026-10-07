/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace KateAi
{

enum class RetryErrorCategory {
    None,
    RateLimitExceeded,
    ServerOverloaded,
    NetworkError,
    Timeout,
    ProviderOverloaded,
    QuotaExceeded,
    UnknownTransient,
    NonRetryable,
    // Same payload would fail again; AgentLoop compacts history and resends.
    ContextOverflow
};

struct RetryContext {
    int attempt = 0;              // 0 = initial attempt, 1 = first retry, etc.
    int maxAttempts = 4;
    int baseDelaySeconds = 5;
    int maxDelaySeconds = 300;
    QString strategy = u"exponential"_s; // "exponential" or "fixed"
    QString providerName;
    RetryErrorCategory lastErrorCategory = RetryErrorCategory::None;
    QString lastErrorMessage;
    QDateTime retryAfter;         // Provider-suggested retry time (if available)
    bool hasProviderRetryAfter = false;
};

class LlmClient : public QObject
{
    Q_OBJECT

    // Allow test class to access private members
    friend class TestableLlmClient;

public:
    explicit LlmClient(QObject *parent = nullptr);
    ~LlmClient() override;

    void setSettings(Settings settings)
    {
        m_settings = std::move(settings);
    }

    // Restricts which built-in tools are advertised. An unrestricted access
    // advertises every tool; the AgentLoop narrows it per mode and plan mode.
    void setToolAccess(const ToolAccess &access)
    {
        m_toolAccess = access;
    }
    // Tool definitions contributed at runtime, currently MCP server tools.
    void setExtraToolDefinitions(const QJsonArray &tools)
    {
        m_extraTools = tools;
    }
    // Built-in tools filtered by the active access, plus the runtime tools.
    QJsonArray advertisedTools() const;

    bool isBusy() const
    {
        return m_reply != nullptr || m_retryTimer.isActive();
    }

    void complete(const QList<ChatMessage> &messages);
    void fetchModels(Provider provider);
    void abort();
    void reset();
    static QJsonArray messagesToJson(const QList<ChatMessage> &messages);
    static CompletionChunk parseSseLine(const QByteArray &line, QHash<int, ToolCall> *acc);

    // Retry control
    void retryLastRequest();  // Manual retry from UI

Q_SIGNALS:
    void textDelta(const QString &delta);
    void thinkingDelta(const QString &delta);
    void finished(const QString &fullText, const QList<ToolCall> &toolCalls);
    void failed(const QString &error);
    void modelsReceived(Provider provider, const QStringList &models);
    void modelsFailed(Provider provider, const QString &error);
    // Retry status signals for UI
    void retryStatus(const QString &message, int attempt, int maxAttempts, int delaySeconds);
    void retryScheduled(int attempt, int maxAttempts, int delaySeconds);

private:
    void handleReadyRead();
    void handleFinished();
    void handleModelsFinished(QNetworkReply *reply, Provider provider);
    // Detaches the watchdog timer owned by `reply`. Must run before the reply is
    // deleted, because the timer lambda holds a pointer to it.
    void stopTimerFor(QNetworkReply *reply);
    // Appends to the bounded response-body tail used for error reporting.
    void appendResponseBody(const QByteArray &chunk);
    void resetCompletionState();
    void emitCompletedOnce();
    QList<ToolCall> completedToolsFromAccumulator();
    
    // API format handling
    QJsonObject buildOpenAIRequest(const QList<ChatMessage> &messages, const QString &model);
    QJsonObject buildAnthropicRequest(const QList<ChatMessage> &messages, const QString &model);
    QJsonObject buildAcpNativeRequest(const QList<ChatMessage> &messages, const QString &model);
    QJsonArray messagesToAnthropicJson(const QList<ChatMessage> &messages);
    
    // Retry logic
    void scheduleRetry(const RetryContext &ctx);
    void executeRetry();
    void doComplete(const QList<ChatMessage> &messages);
    RetryErrorCategory classifyError(int httpStatus, const QString &errorMessage, const QByteArray &responseBody, QNetworkReply::NetworkError networkError);
    std::optional<int> parseRetryAfterHeader(const QNetworkReply *reply) const;
    std::optional<int> parseRetryAfterFromBody(const QByteArray &body) const;
    // Narrows a provider-supplied delay in seconds to the configured cap. Takes
    // qint64 because secsTo() on a distant date overflows an int.
    std::optional<int> clampProviderDelay(qint64 seconds) const;
    // Seconds from now until an HTTP-date (IMF-fixdate) value, or nothing if
    // it is unparseable or already in the past. Static so it is testable
    // without a live QNetworkReply.
    static std::optional<qint64> secondsUntilHttpDate(const QByteArray &header);
    int calculateDelay(const RetryContext &ctx, std::optional<int> providerDelaySeconds) const;
    QString formatRetryMessage(const RetryContext &ctx, int delaySeconds) const;
    QString formatRetryExhaustedMessage(RetryErrorCategory category, const QString &providerName) const;
    bool shouldRetry(const RetryContext &ctx) const;
    void storeRequestForRetry(const QList<ChatMessage> &messages);
    void clearStoredRequest();

protected:
    // Protected accessors for testing
    Settings &testSettings() { return m_settings; }
    QList<ChatMessage> &testStoredMessages() { return m_storedMessages; }
    RetryContext &testRetryContext() { return m_retryContext; }

private:
    void abortModelFetches();

    Settings m_settings;
    ToolAccess m_toolAccess = ToolAccess::unrestricted();
    QJsonArray m_extraTools;
    QNetworkAccessManager m_nam;
    QNetworkReply *m_reply = nullptr;
    QList<QPointer<QNetworkReply>> m_modelReplies;
    QByteArray m_buffer;
    // Every byte received for the in-flight request. Error classification and
    // the provider backoff hint both need the whole body, not just the trailing
    // unterminated SSE line.
    QByteArray m_responseBody;
    // Idle watchdog for the in-flight request, keyed by reply. A streaming
    // completion has no natural deadline, but a half-open connection must not
    // keep the client busy forever.
    QHash<QNetworkReply *, QPointer<QTimer>> m_requestTimers;
    QString m_text;
    QHash<int, ToolCall> m_toolAcc;
    QList<ToolCall> m_completedTools;
    QString m_completionError;
    bool m_sawDone = false;
    bool m_finishEmitted = false;
    bool m_abortRequested = false;
    
    // Retry state
    QTimer m_retryTimer;
    QList<ChatMessage> m_storedMessages;  // Messages for retry
    RetryContext m_retryContext;
    bool m_retryScheduled = false;
};

} // namespace KateAi
