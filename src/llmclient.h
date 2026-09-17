#pragma once

#include "types.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
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
    NonRetryable
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
    void resetCompletionState();
    void emitCompletedOnce();
    QList<ToolCall> completedToolsFromAccumulator();
    
    // Retry logic
    void scheduleRetry(const RetryContext &ctx);
    void executeRetry();
    void doComplete(const QList<ChatMessage> &messages);
    RetryErrorCategory classifyError(int httpStatus, const QString &errorMessage, const QByteArray &responseBody, QNetworkReply::NetworkError networkError);
    std::optional<int> parseRetryAfterHeader(const QNetworkReply *reply) const;
    std::optional<int> parseRetryAfterFromBody(const QByteArray &body) const;
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
    Settings m_settings;
    QNetworkAccessManager m_nam;
    QNetworkReply *m_reply = nullptr;
    QByteArray m_buffer;
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
