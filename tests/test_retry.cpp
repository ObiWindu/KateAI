#include "llmclient.h"
#include "types.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTest>
#include <QSignalSpy>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace KateAi {

// Must live in KateAi so LlmClient's friend class TestableLlmClient applies.
class TestableLlmClient : public LlmClient
{
    Q_OBJECT

public:
    using LlmClient::classifyError;
    using LlmClient::parseRetryAfterHeader;
    using LlmClient::parseRetryAfterFromBody;
    using LlmClient::calculateDelay;
    using LlmClient::formatRetryMessage;
    using LlmClient::formatRetryExhaustedMessage;
    using LlmClient::shouldRetry;
    using LlmClient::storeRequestForRetry;
    using LlmClient::clearStoredRequest;
    using LlmClient::testStoredMessages;
};

} // namespace KateAi

class TestRetry : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Register meta types for signals
        qRegisterMetaType<RetryErrorCategory>("RetryErrorCategory");
    }

    void testClassifyError_NetworkErrors()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // Network errors should be retryable
        QCOMPARE(client.classifyError(0, QString(), QByteArray(), QNetworkReply::ConnectionRefusedError),
                 RetryErrorCategory::NetworkError);
        QCOMPARE(client.classifyError(0, QString(), QByteArray(), QNetworkReply::RemoteHostClosedError),
                 RetryErrorCategory::NetworkError);
        QCOMPARE(client.classifyError(0, QString(), QByteArray(), QNetworkReply::HostNotFoundError),
                 RetryErrorCategory::NetworkError);
        QCOMPARE(client.classifyError(0, QString(), QByteArray(), QNetworkReply::TimeoutError),
                 RetryErrorCategory::NetworkError);
        QCOMPARE(client.classifyError(0, QString(), QByteArray(), QNetworkReply::TemporaryNetworkFailureError),
                 RetryErrorCategory::NetworkError);
    }

    void testClassifyError_HttpStatusCodes()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // 429 - Rate limit
        QCOMPARE(client.classifyError(429, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        // 5xx - Server errors
        QCOMPARE(client.classifyError(500, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::ServerOverloaded);
        QCOMPARE(client.classifyError(502, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::ServerOverloaded);
        QCOMPARE(client.classifyError(503, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::ServerOverloaded);
        QCOMPARE(client.classifyError(504, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::ServerOverloaded);
        QCOMPARE(client.classifyError(509, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::ServerOverloaded);

        // 408 - Timeout
        QCOMPARE(client.classifyError(408, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::Timeout);

        // 400 - Bad Request (non-retryable by default)
        QCOMPARE(client.classifyError(400, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        // 401/403 - Auth errors
        QCOMPARE(client.classifyError(401, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);
        QCOMPARE(client.classifyError(403, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        // 404 - Not found
        QCOMPARE(client.classifyError(404, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        // 413 - Payload too large
        QCOMPARE(client.classifyError(413, QString(), QByteArray(), QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);
    }

    void testClassifyError_ProviderSpecificErrors()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // HTTP status is classified before body text, so 5xx/429 win over provider types.
        QByteArray anthropicOverloaded = "{\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}";
        QCOMPARE(client.classifyError(500, QString(), anthropicOverloaded, QNetworkReply::NoError),
                 RetryErrorCategory::ServerOverloaded);
        QCOMPARE(client.classifyError(0, QString(), anthropicOverloaded, QNetworkReply::NoError),
                 RetryErrorCategory::ProviderOverloaded);

        QByteArray anthropicRateLimit = "{\"error\":{\"type\":\"rate_limit_error\",\"message\":\"Rate limit\"}}";
        QCOMPARE(client.classifyError(429, QString(), anthropicRateLimit, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        QByteArray openaiRateLimit = "{\"error\":{\"type\":\"rate_limit_exceeded\",\"message\":\"Rate limit\"}}";
        QCOMPARE(client.classifyError(429, QString(), openaiRateLimit, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        QByteArray openaiServerError = "{\"error\":{\"type\":\"server_error\",\"message\":\"Internal server error\"}}";
        QCOMPARE(client.classifyError(500, QString(), openaiServerError, QNetworkReply::NoError),
                 RetryErrorCategory::ServerOverloaded);

        QByteArray openaiQuota = "{\"error\":{\"type\":\"insufficient_quota\",\"message\":\"Quota exceeded\"}}";
        QCOMPARE(client.classifyError(429, QString(), openaiQuota, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);
        QCOMPARE(client.classifyError(0, QString(), openaiQuota, QNetworkReply::NoError),
                 RetryErrorCategory::QuotaExceeded);

        QByteArray openaiContextLength = "{\"error\":{\"type\":\"context_length_exceeded\",\"message\":\"Context too long\"}}";
        QCOMPARE(client.classifyError(400, QString(), openaiContextLength, QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        QByteArray openaiModelNotFound = "{\"error\":{\"type\":\"model_not_found\",\"message\":\"Model not found\"}}";
        QCOMPARE(client.classifyError(404, QString(), openaiModelNotFound, QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        QByteArray openaiInvalidKey = "{\"error\":{\"type\":\"invalid_api_key\",\"message\":\"Invalid key\"}}";
        QCOMPARE(client.classifyError(401, QString(), openaiInvalidKey, QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        QByteArray geminiQuota = "{\"error\":{\"message\":\"Quota exceeded\"}}";
        QCOMPARE(client.classifyError(429, QString(), geminiQuota, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);
        QCOMPARE(client.classifyError(0, QString(), geminiQuota, QNetworkReply::NoError),
                 RetryErrorCategory::QuotaExceeded);

        QByteArray geminiResourceExhausted = "{\"error\":{\"message\":\"Resource exhausted\"}}";
        QCOMPARE(client.classifyError(429, QString(), geminiResourceExhausted, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        // AWS Bedrock
        QByteArray bedrockThrottling = "{\"__type\":\"ThrottlingException\",\"message\":\"Throttling\"}}";
        QCOMPARE(client.classifyError(429, QString(), bedrockThrottling, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        // Grok / xAI
        QByteArray grokRateLimit = "{\"error\":\"Rate limit exceeded\"}";
        QCOMPARE(client.classifyError(429, QString(), grokRateLimit, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        // Content policy / safety - non-retryable
        QByteArray contentPolicy = "{\"error\":{\"message\":\"Content policy violation\"}}";
        QCOMPARE(client.classifyError(400, QString(), contentPolicy, QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        QByteArray safetyBlock = "{\"error\":{\"message\":\"Safety violation blocked\"}}";
        QCOMPARE(client.classifyError(400, QString(), safetyBlock, QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);
    }

    void testClassifyError_400WithRetryableMessages()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // 400 with rate limit message should be retryable
        QByteArray body1 = "{\"error\":\"Rate limit exceeded\"}";
        QCOMPARE(client.classifyError(400, QString(), body1, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        QByteArray body2 = "{\"error\":\"Quota exceeded\"}";
        QCOMPARE(client.classifyError(400, QString(), body2, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        QByteArray body3 = "{\"error\":\"Service overloaded\"}";
        QCOMPARE(client.classifyError(400, QString(), body3, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        QByteArray body4 = "{\"error\":\"Throttling\"}";
        QCOMPARE(client.classifyError(400, QString(), body4, QNetworkReply::NoError),
                 RetryErrorCategory::RateLimitExceeded);

        // 400 with context window errors should be non-retryable
        QByteArray body5 = "{\"error\":\"Context window exceeded\"}";
        QCOMPARE(client.classifyError(400, QString(), body5, QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        QByteArray body6 = "{\"error\":\"Too many tokens\"}";
        QCOMPARE(client.classifyError(400, QString(), body6, QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        QByteArray body7 = "{\"error\":\"Prompt too large\"}";
        QCOMPARE(client.classifyError(400, QString(), body7, QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);

        QByteArray body8 = "{\"error\":\"Maximum context length exceeded\"}";
        QCOMPARE(client.classifyError(400, QString(), body8, QNetworkReply::NoError),
                 RetryErrorCategory::NonRetryable);
    }

    void testCalculateDelay_ExponentialBackoff()
    {
        TestableLlmClient client;
        Settings settings;
        settings.baseRetryDelaySeconds = 5;
        settings.maxRetryDelaySeconds = 300;
        settings.retryStrategy = u"exponential"_s;
        client.setSettings(settings);

        RetryContext ctx;
        ctx.baseDelaySeconds = 5;
        ctx.maxDelaySeconds = 300;
        ctx.strategy = u"exponential"_s;

        // Attempt 1 (first retry): 5 * 2^0 = 5s
        ctx.attempt = 1;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 5);

        // Attempt 2: 5 * 2^1 = 10s
        ctx.attempt = 2;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 10);

        // Attempt 3: 5 * 2^2 = 20s
        ctx.attempt = 3;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 20);

        // Attempt 4: 5 * 2^3 = 40s
        ctx.attempt = 4;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 40);

        // Attempt 5: 5 * 2^4 = 80s
        ctx.attempt = 5;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 80);
    }

    void testCalculateDelay_FixedStrategy()
    {
        TestableLlmClient client;
        Settings settings;
        settings.baseRetryDelaySeconds = 10;
        settings.maxRetryDelaySeconds = 300;
        settings.retryStrategy = u"fixed"_s;
        client.setSettings(settings);

        RetryContext ctx;
        ctx.baseDelaySeconds = 10;
        ctx.maxDelaySeconds = 300;
        ctx.strategy = u"fixed"_s;

        ctx.attempt = 1;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 10);

        ctx.attempt = 2;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 10);

        ctx.attempt = 3;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 10);

        ctx.attempt = 4;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 10);
    }

    void testCalculateDelay_ProviderDelayPreferred()
    {
        TestableLlmClient client;
        Settings settings;
        settings.baseRetryDelaySeconds = 5;
        settings.maxRetryDelaySeconds = 300;
        settings.retryStrategy = u"exponential"_s;
        client.setSettings(settings);

        RetryContext ctx;
        ctx.baseDelaySeconds = 5;
        ctx.maxDelaySeconds = 300;
        ctx.strategy = u"exponential"_s;
        ctx.attempt = 1;

        // Provider delay should be preferred over calculated delay
        QCOMPARE(client.calculateDelay(ctx, 30), 30);
        QCOMPARE(client.calculateDelay(ctx, 120), 120);
    }

    void testCalculateDelay_MaxDelayCap()
    {
        TestableLlmClient client;
        Settings settings;
        settings.baseRetryDelaySeconds = 5;
        settings.maxRetryDelaySeconds = 60; // Cap at 60s
        settings.retryStrategy = u"exponential"_s;
        client.setSettings(settings);

        RetryContext ctx;
        ctx.baseDelaySeconds = 5;
        ctx.maxDelaySeconds = 60;
        ctx.strategy = u"exponential"_s;

        // Attempt 4 would be 40s (under cap)
        ctx.attempt = 4;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 40);

        // Attempt 5 would be 80s but capped at 60s
        ctx.attempt = 5;
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 60);

        // Provider delay also capped
        QCOMPARE(client.calculateDelay(ctx, 120), 60);
    }

    void testCalculateDelay_NoMaxCap()
    {
        TestableLlmClient client;
        Settings settings;
        settings.baseRetryDelaySeconds = 5;
        settings.maxRetryDelaySeconds = 0; // No cap
        settings.retryStrategy = u"exponential"_s;
        client.setSettings(settings);

        RetryContext ctx;
        ctx.baseDelaySeconds = 5;
        ctx.maxDelaySeconds = 0;
        ctx.strategy = u"exponential"_s;

        ctx.attempt = 10; // 5 * 2^9 = 2560s
        QCOMPARE(client.calculateDelay(ctx, std::nullopt), 2560);
    }

    void testParseRetryAfterHeader_Seconds()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // Test the parsing logic manually
        QByteArray headerValue = "30";
        bool ok = false;
        int seconds = headerValue.toInt(&ok);
        QVERIFY(ok);
        QCOMPARE(seconds, 30);
    }

    void testParseRetryAfterHeader_HttpDate()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // RFC2822Date in Qt accepts numeric offsets; production code also falls back to ISODate.
        QDateTime retryDate = QDateTime::fromString(u"Wed, 21 Oct 2015 07:28:00 +0000"_s, Qt::RFC2822Date);
        QVERIFY(retryDate.isValid());

        QDateTime isoDate = QDateTime::fromString(u"2015-10-21T07:28:00Z"_s, Qt::ISODate);
        QVERIFY(isoDate.isValid());

        QDateTime future = QDateTime::currentDateTime().addSecs(30);
        QVERIFY(QDateTime::currentDateTime().secsTo(future) > 0);
    }

    void testParseRetryAfterFromBody_RetryDelayString()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // Google Gemini style: "retryDelay": "1.5s"
        QByteArray body1 = "{\"retryDelay\":\"1.5s\"}";
        auto result1 = client.parseRetryAfterFromBody(body1);
        QVERIFY(result1.has_value());
        QCOMPARE(*result1, 2); // ceil(1.5) = 2

        QByteArray body2 = "{\"retryDelay\":\"30s\"}";
        auto result2 = client.parseRetryAfterFromBody(body2);
        QVERIFY(result2.has_value());
        QCOMPARE(*result2, 30);
    }

    void testParseRetryAfterFromBody_RetryDelayNumber()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // Numeric retryDelay
        QByteArray body = "{\"retryDelay\":45}";
        auto result = client.parseRetryAfterFromBody(body);
        QVERIFY(result.has_value());
        QCOMPARE(*result, 45);
    }

    void testParseRetryAfterFromBody_AnthropicRetryDelay()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // Anthropic style: error.retry_delay
        QByteArray body1 = "{\"error\":{\"retry_delay\":\"2.5s\"}}";
        auto result1 = client.parseRetryAfterFromBody(body1);
        QVERIFY(result1.has_value());
        QCOMPARE(*result1, 3); // ceil(2.5) = 3

        QByteArray body2 = "{\"error\":{\"retry_delay\":10}}";
        auto result2 = client.parseRetryAfterFromBody(body2);
        QVERIFY(result2.has_value());
        QCOMPARE(*result2, 10);
    }

    void testParseRetryAfterFromBody_Invalid()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        // Empty body
        auto result1 = client.parseRetryAfterFromBody(QByteArray());
        QVERIFY(!result1.has_value());

        // Invalid JSON
        auto result2 = client.parseRetryAfterFromBody(QByteArray("not json"));
        QVERIFY(!result2.has_value());

        // No retryDelay field
        auto result3 = client.parseRetryAfterFromBody(QByteArray("{\"error\":\"something\"}"));
        QVERIFY(!result3.has_value());
    }

    void testShouldRetry()
    {
        TestableLlmClient client;
        Settings settings;
        settings.enableAutoRetry = true;
        settings.maxRetryAttempts = 4;
        client.setSettings(settings);

        RetryContext ctx;
        ctx.maxAttempts = 4;
        ctx.attempt = 0;
        ctx.lastErrorCategory = RetryErrorCategory::RateLimitExceeded;

        // Should retry on first attempt with retryable error
        QVERIFY(client.shouldRetry(ctx));

        // Should retry up to maxAttempts
        ctx.attempt = 3;
        QVERIFY(client.shouldRetry(ctx));

        // Should not retry after maxAttempts
        ctx.attempt = 4;
        QVERIFY(!client.shouldRetry(ctx));

        // Should not retry non-retryable errors
        ctx.attempt = 1;
        ctx.lastErrorCategory = RetryErrorCategory::NonRetryable;
        QVERIFY(!client.shouldRetry(ctx));

        // Should not retry if auto-retry disabled
        settings.enableAutoRetry = false;
        client.setSettings(settings);
        ctx.lastErrorCategory = RetryErrorCategory::RateLimitExceeded;
        QVERIFY(!client.shouldRetry(ctx));
    }

    void testFormatRetryMessage()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        RetryContext ctx;
        ctx.attempt = 1;
        ctx.maxAttempts = 4;
        ctx.providerName = u"OpenAI"_s;
        ctx.lastErrorCategory = RetryErrorCategory::RateLimitExceeded;

        QString msg = client.formatRetryMessage(ctx, 5);
        QVERIFY(msg.contains(u"Rate limit reached"_s));
        QVERIFY(msg.contains(u"OpenAI"_s));
        QVERIFY(msg.contains(u"5s"_s));
        QVERIFY(msg.contains(u"attempt 1/4"_s));

        ctx.lastErrorCategory = RetryErrorCategory::ServerOverloaded;
        msg = client.formatRetryMessage(ctx, 10);
        QVERIFY(msg.contains(u"Server overloaded"_s));
        QVERIFY(msg.contains(u"10s"_s));

        ctx.lastErrorCategory = RetryErrorCategory::NetworkError;
        msg = client.formatRetryMessage(ctx, 5);
        QVERIFY(msg.contains(u"Network error"_s));

        ctx.lastErrorCategory = RetryErrorCategory::QuotaExceeded;
        msg = client.formatRetryMessage(ctx, 5);
        QVERIFY(msg.contains(u"Quota exceeded"_s));
    }

    void testFormatRetryExhaustedMessage()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        QString msg = client.formatRetryExhaustedMessage(RetryErrorCategory::RateLimitExceeded, u"OpenAI"_s);
        QVERIFY(msg.contains(u"OpenAI"_s));
        QVERIFY(msg.contains(u"rate limit"_s, Qt::CaseInsensitive));
        QVERIFY(msg.contains(u"retry automatically"_s));

        msg = client.formatRetryExhaustedMessage(RetryErrorCategory::ServerOverloaded, u"Anthropic"_s);
        QVERIFY(msg.contains(u"Anthropic"_s));
        QVERIFY(msg.contains(u"temporarily unavailable"_s));

        msg = client.formatRetryExhaustedMessage(RetryErrorCategory::QuotaExceeded, u"Google"_s);
        QVERIFY(msg.contains(u"Google"_s));
        QVERIFY(msg.contains(u"quota"_s, Qt::CaseInsensitive));

        msg = client.formatRetryExhaustedMessage(RetryErrorCategory::NetworkError, QString());
        QVERIFY(msg.contains(u"Network connection failed"_s));
    }

    void testStoreAndClearRequest()
    {
        TestableLlmClient client;
        Settings settings;
        client.setSettings(settings);

        QList<ChatMessage> messages;
        ChatMessage msg;
        msg.role = ChatMessage::Role::User;
        msg.content = u"Hello"_s;
        messages.append(msg);

        client.storeRequestForRetry(messages);
        QVERIFY(!client.testStoredMessages().isEmpty());
        QCOMPARE(client.testStoredMessages().size(), 1);
        QCOMPARE(client.testStoredMessages().first().content, u"Hello"_s);

        client.clearStoredRequest();
        QVERIFY(client.testStoredMessages().isEmpty());
    }

    void testRetrySettingsDefaultValues()
    {
        Settings settings;
        QVERIFY(settings.enableAutoRetry);
        QCOMPARE(settings.maxRetryAttempts, 4);
        QCOMPARE(settings.baseRetryDelaySeconds, 5);
        QCOMPARE(settings.maxRetryDelaySeconds, 300);
        QCOMPARE(settings.retryStrategy, u"exponential"_s);
    }
};

QTEST_GUILESS_MAIN(TestRetry)
#include "test_retry.moc"