#include "harness/models.h"
#include "llmclient.h"
#include "modes.h"
#include "types.h"

#include <QHash>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace KateAi
{
class TestableLlmClient : public LlmClient
{
public:
    QJsonObject geminiBody(const QList<ChatMessage> &messages, const QString &model,
                           const QString &cachedContent)
    {
        return buildGeminiRequest(messages, model, cachedContent);
    }
    QString geminiCacheKey(const QList<ChatMessage> &messages, const QString &model)
    {
        return geminiCachePlan(messages, model).key;
    }
    QJsonObject geminiCacheBody(const QList<ChatMessage> &messages, const QString &model)
    {
        return buildGeminiCacheRequest(geminiCachePlan(messages, model), model);
    }
    QJsonObject openAiBody(const QList<ChatMessage> &messages, const QString &model)
    {
        return buildOpenAIRequest(messages, model);
    }
};
} // namespace KateAi

class TestLlmParse : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesContentDelta()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}"), &acc);
        QCOMPARE(chunk.contentDelta, u"Hello"_s);
        QVERIFY(!chunk.finished);
    }

    void geminiNativeEndpointIsDetected()
    {
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl =
            u"https://generativelanguage.googleapis.com/v1beta/openai"_s;
        QVERIFY(LlmClient::usesGeminiNativeTransport(settings));
        QCOMPARE(LlmClient::geminiNativeBaseUrl(settings),
                 u"https://generativelanguage.googleapis.com/v1beta"_s);

        settings.openaiCompatibleUrl =
            u"https://generativelanguage.googleapis.com/v1beta"_s;
        QCOMPARE(LlmClient::geminiNativeBaseUrl(settings),
                 u"https://generativelanguage.googleapis.com/v1beta"_s);

        settings.openaiCompatibleUrl = u"http://localhost:11434/v1"_s;
        QVERIFY(!LlmClient::usesGeminiNativeTransport(settings));
    }

    void geminiRequestUsesNativeShapeAndCacheReference()
    {
        TestableLlmClient client;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl =
            u"https://generativelanguage.googleapis.com/v1beta/openai"_s;
        client.setSettings(settings);

        QList<ChatMessage> messages;
        ChatMessage system;
        system.role = ChatMessage::Role::System;
        system.content = u"You are Kate."_s;
        messages.append(system);
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"Read main.cpp"_s;
        messages.append(user);

        const QJsonObject body = client.geminiBody(messages, u"gemini-2.5-pro"_s, QString());
        QCOMPARE(body.value(u"systemInstruction"_s).toObject()
                     .value(u"parts"_s).toArray().first().toObject()
                     .value(u"text"_s).toString(),
                 u"You are Kate."_s);
        QCOMPARE(body.value(u"contents"_s).toArray().first().toObject()
                     .value(u"role"_s).toString(),
                 u"user"_s);
        QVERIFY(!body.value(u"tools"_s).toArray().isEmpty());
        QVERIFY(body.contains(u"toolConfig"_s));

        const QJsonObject cached = client.geminiBody(
            messages, u"gemini-2.5-pro"_s, u"cachedContents/abc"_s);
        QCOMPARE(cached.value(u"cachedContent"_s).toString(),
                 u"cachedContents/abc"_s);
        QVERIFY(!cached.contains(u"systemInstruction"_s));
        QVERIFY(!cached.contains(u"tools"_s));
    }

    void geminiCacheKeyTracksPrefixChanges()
    {
        TestableLlmClient client;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl =
            u"https://generativelanguage.googleapis.com/v1beta"_s;
        client.setSettings(settings);

        QList<ChatMessage> messages;
        ChatMessage system;
        system.role = ChatMessage::Role::System;
        system.content = u"System one"_s;
        messages.append(system);
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"First request"_s;
        messages.append(user);
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"First answer"_s;
        messages.append(assistant);

        const QString first = client.geminiCacheKey(messages, u"gemini-2.5-pro"_s);
        QVERIFY(!first.isEmpty());
        QCOMPARE(client.geminiCacheKey(messages, u"gemini-2.5-pro"_s), first);

        messages[0].content = u"System two"_s;
        QVERIFY(client.geminiCacheKey(messages, u"gemini-2.5-pro"_s) != first);
    }

    void geminiCacheIgnoresTransientTurnContext()
    {
        // The turn-context message is rebuilt every request. If it entered the
        // cached prefix the key would change each turn, recreate the resource,
        // and the cache would never hit.
        TestableLlmClient client;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl =
            u"https://generativelanguage.googleapis.com/v1beta"_s;
        client.setSettings(settings);

        QList<ChatMessage> persistent;
        ChatMessage system;
        system.role = ChatMessage::Role::System;
        system.content = u"System prompt"_s;
        persistent.append(system);
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"First request"_s;
        persistent.append(user);
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"First answer"_s;
        persistent.append(assistant);

        QList<ChatMessage> withContext = persistent;
        ChatMessage ambient;
        ambient.role = ChatMessage::Role::User;
        ambient.content = u"Editor context: volatile"_s;
        ambient.transient = true;
        withContext.insert(1, ambient);

        const QString key = client.geminiCacheKey(persistent, u"gemini-2.5-pro"_s);
        QVERIFY(!key.isEmpty());
        QCOMPARE(client.geminiCacheKey(withContext, u"gemini-2.5-pro"_s), key);

        const QJsonObject body = client.geminiCacheBody(withContext, u"gemini-2.5-pro"_s);
        const QString json = QString::fromUtf8(
            QJsonDocument(body).toJson(QJsonDocument::Compact));
        QVERIFY2(!json.contains(u"Editor context: volatile"_s),
                 "the volatile turn context entered the cached prefix");
    }

    void unknownModelKeepsExplicitOutputLimit()
    {
        // A user-set max_tokens is only capped for a known hosted family. The
        // conservative fallback must not silently truncate a local model.
        TestableLlmClient client;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.maxTokens = 30000;
        client.setSettings(settings);

        QList<ChatMessage> messages;
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"Hello"_s;
        messages.append(user);

        QCOMPARE(client.openAiBody(messages, u"mystery-model-7b"_s)
                     .value(u"max_tokens"_s).toInt(),
                 30000);
        QCOMPARE(client.openAiBody(messages, u"my-local-model"_s)
                     .value(u"max_tokens"_s).toInt(),
                 30000);
        QCOMPARE(client.openAiBody(messages, u"gpt-4o"_s)
                     .value(u"max_tokens"_s).toInt(),
                 16384);
    }

    void geminiCacheRequestCarriesThePrefix()
    {
        TestableLlmClient client;
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl =
            u"https://generativelanguage.googleapis.com/v1beta"_s;
        client.setSettings(settings);

        QList<ChatMessage> messages;
        ChatMessage system;
        system.role = ChatMessage::Role::System;
        system.content = u"System prompt"_s;
        messages.append(system);
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"First request"_s;
        messages.append(user);
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"First answer"_s;
        messages.append(assistant);

        const QJsonObject body = client.geminiCacheBody(messages, u"gemini-2.5-flash"_s);
        QCOMPARE(body.value(u"model"_s).toString(), u"models/gemini-2.5-flash"_s);
        QCOMPARE(body.value(u"ttl"_s).toString(), u"1800s"_s);
        QVERIFY(!body.value(u"contents"_s).toArray().isEmpty());
        QCOMPARE(body.value(u"systemInstruction"_s).toObject()
                     .value(u"parts"_s).toArray().first().toObject()
                     .value(u"text"_s).toString(),
                 u"System prompt"_s);
        QVERIFY(!body.value(u"tools"_s).toArray().isEmpty());
    }

    void parsesGeminiTextAndUsage()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"candidates\":[{\"content\":{\"role\":\"model\","
                       "\"parts\":[{\"text\":\"Hello\"}]},\"finishReason\":\"STOP\"}],"
                       "\"usageMetadata\":{\"promptTokenCount\":120,"
                       "\"candidatesTokenCount\":8,\"cachedContentTokenCount\":100,"
                       "\"thoughtsTokenCount\":4}}"),
            &acc);
        QCOMPARE(chunk.contentDelta, u"Hello"_s);
        QCOMPARE(chunk.finishReason, u"stop"_s);
        QVERIFY(chunk.finished);
        QCOMPARE(chunk.inputTokens, 120);
        QCOMPARE(chunk.outputTokens, 12);
        QCOMPARE(chunk.cachedInputTokens, 100);
    }

    void parsesGeminiFunctionCall()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"candidates\":[{\"content\":{\"role\":\"model\","
                       "\"parts\":[{\"functionCall\":{\"name\":\"read_file\","
                       "\"args\":{\"path\":\"main.cpp\"}}}]},\"finishReason\":\"STOP\"}]}"),
            &acc);
        QCOMPARE(chunk.completedTools.size(), 1);
        const ToolCall tool = chunk.completedTools.first();
        QCOMPARE(tool.name, u"read_file"_s);
        QCOMPARE(tool.arguments.value(u"path"_s).toString(), u"main.cpp"_s);
        QVERIFY(!tool.id.isEmpty());
    }

    void parsesGeminiThoughtParts()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"candidates\":[{\"content\":{\"role\":\"model\","
                       "\"parts\":[{\"text\":\"thinking\",\"thought\":true}]}}]}"),
            &acc);
        QCOMPARE(chunk.thinkingDelta, u"thinking"_s);
        QVERIFY(chunk.contentDelta.isEmpty());
    }

    void geminiBlockedPromptIsAnError()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"promptFeedback\":{\"blockReason\":\"SAFETY\"}}"), &acc);
        QVERIFY(chunk.finished);
        QVERIFY(chunk.error.contains(u"Gemini blocked"_s));
    }

    void usageOnFinalChoiceIsParsed()
    {
        // Some gateways attach usage to the final choice instead of sending a
        // separate usage-only chunk.
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
                       "\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":8,"
                       "\"prompt_tokens_details\":{\"cached_tokens\":40}}}"),
            &acc);
        QCOMPARE(chunk.inputTokens, 120);
        QCOMPARE(chunk.outputTokens, 8);
        QCOMPARE(chunk.cachedInputTokens, 40);
        QCOMPARE(chunk.finishReason, u"stop"_s);
    }

    void anthropicUsageCarriesCacheHitsAndWrites()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"type\":\"message_start\",\"message\":{\"usage\":"
                       "{\"input_tokens\":90,\"output_tokens\":1,"
                       "\"cache_read_input_tokens\":1000,"
                       "\"cache_creation_input_tokens\":250}}}"),
            &acc);
        QCOMPARE(chunk.inputTokens, 90);
        QCOMPARE(chunk.outputTokens, 1);
        QCOMPARE(chunk.cachedInputTokens, 1000);
        QCOMPARE(chunk.cacheWriteTokens, 250);
    }

    void openCodeUsageCarriesBilledTokensCacheAndCost()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
                       "\"usage\":{\"prompt_tokens\":8200,\"completion_tokens\":9,"
                       "\"cost\":0.00412,"
                       "\"opencode\":{\"billing_source\":\"go\","
                       "\"list_cost\":0.01,\"allowance\":0.01,"
                       "\"tokens\":{\"input\":100,\"output\":9,\"reasoning\":2,"
                       "\"cache_read\":8000,\"cache_write_5m\":100,"
                       "\"cache_write_1h\":50}}}}"),
            &acc);
        // opencode.tokens is the authoritative billed breakdown: input
        // excludes cache reads/writes, output includes reasoning.
        QCOMPARE(chunk.inputTokens, 100);
        QCOMPARE(chunk.outputTokens, 9);
        QCOMPARE(chunk.cachedInputTokens, 8000);
        QCOMPARE(chunk.cacheWriteTokens, 150);
        QVERIFY(qAbs(chunk.reportedCostUsd - 0.00412) < 1e-12);
        QVERIFY(qAbs(chunk.listCostUsd - 0.01) < 1e-12);
        QVERIFY(qAbs(chunk.allowanceUsd - 0.01) < 1e-12);
        QCOMPARE(chunk.billingSource, u"go"_s);
    }

    void openCodeReasoningEffortIsConstrainedToCatalogue()
    {
        QJsonObject model;
        model.insert(u"id"_s, u"claude-sonnet-5-5"_s);
        model.insert(u"limit"_s, QJsonObject{{u"context"_s, 1000000}});
        model.insert(u"cost"_s, QJsonObject{{u"text"_s, QJsonObject{
            {u"input"_s, QJsonObject{{u"price"_s, 2.0}}},
            {u"output"_s, QJsonObject{{u"price"_s, 10.0}}}}}});
        model.insert(u"capabilities"_s, QJsonObject{
            {u"reasoning"_s, QJsonObject{
                {u"supported"_s, true},
                {u"effort"_s, QJsonArray{u"low"_s, u"high"_s}}}}});
        ModelProfiles::registerModels(Provider::OpenCode, QJsonArray{model});

        TestableLlmClient client;
        Settings settings;
        settings.provider = Provider::OpenCode;
        settings.reasoningEffort = u"high"_s;
        client.setSettings(settings);

        QList<ChatMessage> messages;
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"Hello"_s;
        messages.append(user);

        QJsonObject body = client.openAiBody(messages, u"claude-sonnet-5-5"_s);
        QCOMPARE(body.value(u"reasoning_effort"_s).toString(), u"high"_s);

        // A level the catalogue did not publish is not sent, so the gateway
        // cannot reject an otherwise valid request.
        settings.reasoningEffort = u"minimal"_s;
        client.setSettings(settings);
        body = client.openAiBody(messages, u"claude-sonnet-5-5"_s);
        QVERIFY(!body.contains(u"reasoning_effort"_s));

        ModelProfiles::clearRegisteredModels(Provider::OpenCode);
    }

    void nullErrorFieldIsNotAnError()
    {
        // Some providers include {"error":null} on a successful chunk.
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"error\":null,\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}"), &acc);
        QVERIFY2(chunk.error.isEmpty(), qPrintable(chunk.error));
        QCOMPARE(chunk.contentDelta, u"ok"_s);
    }

    void splitEventIsJoined()
    {
        // The SSE specification allows one event to be spread over several
        // "data:" lines. The parser reports whether the payload was complete,
        // so the caller can wait for the rest instead of dropping the event.
        QHash<int, ToolCall> acc;
        bool parsed = true;
        const CompletionChunk partial = LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"content\":"), &acc, &parsed);
        QVERIFY(!parsed);
        QVERIFY(partial.contentDelta.isEmpty());
    }

    void toolCallWithoutIndexDoesNotOverwriteTheOther()
    {
        // A provider that omits "index" sent every parallel tool call as
        // index 0, so the calls overwrote each other.
        QHash<int, ToolCall> acc;
        LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"a\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]}}]}"),
            &acc);
        LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"b\",\"function\":{\"name\":\"grep\",\"arguments\":\"{}\"}}]}}]}"),
            &acc);
        QCOMPARE(acc.size(), 2);
    }

    void toolCallWithStringIndexUsesItsSlot()
    {
        // QJsonValue::toInt() yielded 0 for "index":"1", collapsing parallel
        // calls into slot 0.
        QHash<int, ToolCall> acc;
        LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]}}]}"),
            &acc);
        LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":\"1\",\"id\":\"b\",\"function\":{\"name\":\"grep\",\"arguments\":\"{}\"}}]}}]}"),
            &acc);
        QCOMPARE(acc.size(), 2);
        QCOMPARE(acc.value(1).name, u"grep"_s);
    }

    void toolCallStreamedWithoutIndexStaysOneCall()
    {
        // A provider that omits "index" and streams the arguments in fragments
        // - the shape every OpenAI-compatible server uses - was split into one
        // call per delta, each with a fragment of the arguments and no name.
        QHash<int, ToolCall> acc;
        LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"a\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\"}}]}}]}"),
            &acc);
        LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"function\":{\"arguments\":\"\\\"main.cpp\\\"}\"}}]}}]}"),
            &acc);
        QCOMPARE(acc.size(), 1);
        const ToolCall tool = acc.value(0);
        QCOMPARE(tool.id, u"a"_s);
        QCOMPARE(tool.name, u"read_file"_s);
        QCOMPARE(tool.argumentsJson, u"{\"path\":\"main.cpp\"}"_s);
    }

    void placeholderObjectDoesNotBreakTheArguments()
    {
        // A server that opens with "{}" and follows it with the real
        // arguments produced "{}{\"suggestion\":...}", which dropped the
        // arguments before the tool ran.
        QHash<int, ToolCall> acc;
        LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"suggest\",\"arguments\":\"{}\"}}]}}]}"),
            &acc);
        LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"suggestion\\\":\\\"hi\\\"}\"}}]}}]}"),
            &acc);
        const QString arguments = acc.value(0).argumentsJson;
        QVERIFY(!arguments.contains(u"{}"_s));
        QVERIFY(arguments.contains(u"suggestion"_s));
    }

    void parsesAnthropicEvents()
    {
        // The Messages API streams its own event shapes and never the OpenAI
        // "choices" envelope or a "[DONE]" sentinel. Everything used to be
        // dropped, so a Claude turn ended with "the provider returned an
        // invalid response" instead of an answer.
        QHash<int, ToolCall> acc;
        QHash<int, ToolCall> *accPtr = &acc;

        auto event = [&accPtr](const char *data) {
            return LlmClient::parseSseLine(QByteArray("data: ") + data, accPtr);
        };

        const CompletionChunk started = event("{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\"}}");
        QVERIFY(started.contentDelta.isEmpty());

        const CompletionChunk text = event("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}");
        QCOMPARE(text.contentDelta, u"Hi"_s);

        const CompletionChunk thinking = event("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"pondering\"}}");
        QCOMPARE(thinking.thinkingDelta, u"pondering"_s);

        const CompletionChunk toolStart = event("{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read_file\",\"input\":{}}}");
        QCOMPARE(toolStart.completedTools.size(), 0);
        QCOMPARE(acc.value(1).name, u"read_file"_s);

        const CompletionChunk toolArgs = event("{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"a\\\"}\"}}");
        QVERIFY(toolArgs.completedTools.isEmpty());

        const CompletionChunk stopReason = event("{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}");
        QCOMPARE(stopReason.finishReason, u"tool_use"_s);

        // message_stop is Anthropic's terminator: it must close the turn and
        // flush the tool that was being streamed.
        const CompletionChunk finished = event("{\"type\":\"message_stop\"}");
        QVERIFY(finished.finished);
        QCOMPARE(finished.completedTools.size(), 1);
        QCOMPARE(finished.completedTools.at(0).name, u"read_file"_s);
        QCOMPARE(finished.completedTools.at(0).arguments.value(u"path"_s).toString(), u"a"_s);
        QVERIFY(acc.isEmpty());

        // A short input delivered complete in content_block_start, with no
        // delta events, must survive the same path.
        QHash<int, ToolCall> shortAcc;
        const CompletionChunk shortStart = LlmClient::parseSseLine(
            QByteArray("data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
                       "{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"list_dir\",\"input\":{\"path\":\".\"}}}"),
            &shortAcc);
        QCOMPARE(shortAcc.value(0).arguments.value(u"path"_s).toString(), u"."_s);
        const CompletionChunk shortDone = LlmClient::parseSseLine(
            QByteArray("data: {\"type\":\"message_stop\"}"), &shortAcc);
        QCOMPARE(shortDone.completedTools.size(), 1);
        QCOMPARE(shortDone.completedTools.at(0).arguments.value(u"path"_s).toString(), u"."_s);

        // Anthropic reports mid-stream failures as an error event.
        QHash<int, ToolCall> errorAcc;
        const CompletionChunk error = LlmClient::parseSseLine(
            QByteArray("data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"),
            &errorAcc);
        QCOMPARE(error.error, u"Overloaded"_s);
        QVERIFY(error.finished);

        // An OpenAI-shaped stream still parses unchanged.
        QHash<int, ToolCall> openAiAcc;
        const CompletionChunk openAi = LlmClient::parseSseLine(
            QByteArray("data: {\"type\":\"something\",\"choices\":[{\"delta\":{\"content\":\"still works\"}}]}"),
            &openAiAcc);
        QCOMPARE(openAi.contentDelta, u"still works"_s);
    }

    void anthropicRequestUsesItsOwnAuthHeader()
    {
        // The Messages API authenticates with x-api-key plus a pinned version,
        // never with an Authorization bearer token. "Authorization: x-api-key:
        // <key>" is invisible to the server, so every Claude call answered 401.
        QNetworkRequest anthropic;
        LlmClient::applyAuthHeaders(anthropic, ApiFormat::AnthropicCompatible, u"sk-ant-test"_s);
        QCOMPARE(anthropic.rawHeader("x-api-key"), QByteArray("sk-ant-test"));
        QCOMPARE(anthropic.rawHeader("anthropic-version"), QByteArray("2023-06-01"));
        QVERIFY(anthropic.rawHeader("Authorization").isEmpty());

        QNetworkRequest openai;
        LlmClient::applyAuthHeaders(openai, ApiFormat::OpenAICompatible, u"sk-oai-test"_s);
        QCOMPARE(openai.rawHeader("Authorization"), QByteArray("Bearer sk-oai-test"));
        QVERIFY(openai.rawHeader("x-api-key").isEmpty());

        QNetworkRequest acp;
        LlmClient::applyAuthHeaders(acp, ApiFormat::AcpNative, u"acp-test"_s);
        QCOMPARE(acp.rawHeader("Authorization"), QByteArray("Bearer acp-test"));
    }

    void toolCallsAreDrainedInStreamOrder()
    {
        // Sorting the drained calls by id string reorders them ("call_10" sorts
        // before "call_2"), so the model's own ordering of independent calls was
        // lost. The accumulator index is the order the provider streamed.
        QHash<int, ToolCall> acc;
        for (int index = 0; index < 2; ++index) {
            const QByteArray line = QStringLiteral(
                                        "data: {\"choices\":[{\"delta\":{\"tool_calls\":"
                                        "[{\"index\":%1,\"id\":\"call_%2\",\"function\":{\"name\":"
                                        "\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"f%3\\\"}\"}}]}}]}")
                                        .arg(index)
                                        // "call_10" sorts before "call_9", so a string
                                        // sort inverts these two.
                                        .arg(index + 9)
                                        .arg(index)
                                        .toUtf8();
            LlmClient::parseSseLine(line, &acc);
        }
        const CompletionChunk chunk = LlmClient::parseSseLine(QByteArray("data: [DONE]"), &acc);
        QCOMPARE(chunk.completedTools.size(), 2);
        QCOMPARE(chunk.completedTools.at(0).arguments.value(u"path"_s).toString(), u"f0"_s);
        QCOMPARE(chunk.completedTools.at(1).arguments.value(u"path"_s).toString(), u"f1"_s);
    }

    void parsesDone()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(QByteArray("data: [DONE]"), &acc);
        QVERIFY(chunk.finished);
    }

    void parsesToolCall()
    {
        QHash<int, ToolCall> acc;
        LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"\"}}]}}]}"),
            &acc);
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"path\\\":\\\"a.cpp\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}"),
            &acc);
        QVERIFY(chunk.finished);
        QCOMPARE(chunk.completedTools.size(), 1);
        QCOMPARE(chunk.completedTools.at(0).name, u"read_file"_s);
        QCOMPARE(chunk.completedTools.at(0).arguments.value(u"path"_s).toString(), u"a.cpp"_s);
    }

    // Ollama's OpenAI-compatible endpoint (the default this plugin points at)
    // streams the arguments as a JSON object rather than a string. Reading the
    // object as a string yields nothing, and the call reached the cards as an
    // empty object: the suggest card had no body and every other card showed
    // "Input {}".
    void parsesToolCallWithObjectArguments()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":"
                       "{\"name\":\"suggest\",\"arguments\":{\"suggestion\":\"use string_view\",\"path\":\"a.cpp\","
                       "\"line\":42}}}]},\"finish_reason\":\"tool_calls\"}]}"),
            &acc);
        QVERIFY(chunk.finished);
        QCOMPARE(chunk.completedTools.size(), 1);
        const ToolCall tool = chunk.completedTools.first();
        QCOMPARE(tool.name, u"suggest"_s);
        QCOMPARE(tool.arguments.value(u"suggestion"_s).toString(), u"use string_view"_s);
        QCOMPARE(tool.arguments.value(u"path"_s).toString(), u"a.cpp"_s);
        QCOMPARE(tool.arguments.value(u"line"_s).toInt(), 42);
    }

    void providerUrls()
    {
        QCOMPARE(providerBaseUrl(Provider::Grok), u"https://api.x.ai/v1"_s);
        QCOMPARE(providerBaseUrl(Provider::OpenAI), u"https://api.openai.com/v1"_s);
        QCOMPARE(providerBaseUrl(Provider::OpenRouter), u"https://openrouter.ai/api/v1"_s);
        QCOMPARE(providerBaseUrl(Provider::DeepSeek), u"https://api.deepseek.com"_s);
    }

    void settingsKeys()
    {
        Settings s;
        s.grokApiKey = u"xai-test"_s;
        s.openaiApiKey = u"sk-test"_s;
        s.openrouterApiKey = u"or-test"_s;
        s.deepseekApiKey = u"ds-test"_s;
        s.provider = Provider::Grok;
        QCOMPARE(apiKeyFor(s), u"xai-test"_s);
        s.provider = Provider::OpenAI;
        QCOMPARE(apiKeyFor(s), u"sk-test"_s);
        s.provider = Provider::OpenRouter;
        QCOMPARE(apiKeyFor(s), u"or-test"_s);
        s.provider = Provider::DeepSeek;
        QCOMPARE(apiKeyFor(s), u"ds-test"_s);
    }

    void planModeOnlyAdvertisesReadTools()
    {
        const QJsonArray tools = toolDefinitions(ModeRegistry::toolAccessForGroups({ToolGroup::read()}));
        QCOMPARE(tools.size(), 28);
        for (const QJsonValue &tool : tools) {
            const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
            QVERIFY(name == u"read_file"_s || name == u"read_tool_result"_s || name == u"list_dir"_s || name == u"grep"_s || name == u"glob"_s
                    || name == u"query_project_graph"_s || name == u"write_plan_file"_s || name == u"suggest"_s
                    || name == u"question"_s || name == u"skill"_s
                    || name == u"todo_write"_s || name == u"todo_read"_s || name == u"think"_s
                    || name == u"exit_plan_mode"_s
                    || name == u"lsp_diagnostics"_s || name == u"lsp_symbols"_s || name == u"lsp_hover"_s
                    || name == u"lsp_definition"_s || name == u"lsp_references"_s || name == u"lsp_code_actions"_s
                    || name == u"debug_status"_s || name == u"debug_stack"_s || name == u"debug_locals"_s
                    || name == u"debug_breakpoints"_s || name == u"debug_select_frame"_s
                    || name == u"debug_select_thread"_s || name == u"debug_snapshot"_s
                    || name == u"build_targets"_s);
        }
    }

    void malformedToolArgumentsArePreservedForAnErrorResult()
    {
        QHash<int, ToolCall> acc;
        const CompletionChunk chunk = LlmClient::parseSseLine(
            QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"not json\"}}]},\"finish_reason\":\"tool_calls\"}]}"),
            &acc);
        QVERIFY(chunk.finished);
        QCOMPARE(chunk.completedTools.size(), 1);
        QCOMPARE(chunk.completedTools.first().argumentsJson, u"not json"_s);
        QVERIFY(chunk.completedTools.first().arguments.isEmpty());
    }

    void historyCompressionKeepsTheSystemPrompt()
    {
        QList<ChatMessage> messages;
        ChatMessage system;
        system.role = ChatMessage::Role::System;
        system.content = u"system prompt"_s;
        messages.append(system);
        for (int i = 0; i < 5; ++i) {
            ChatMessage user;
            user.role = ChatMessage::Role::User;
            user.content = u"message %1"_s.arg(i);
            messages.append(user);
        }

        // A window this small still has to leave a usable conversation.
        const QList<ChatMessage> trimmed = compressMessageHistory(messages, 3, 1000000, true);
        QVERIFY(!trimmed.isEmpty());
        QCOMPARE(trimmed.first().role, ChatMessage::Role::System);
        QVERIFY(trimmed.size() <= 3);

        // 0 means "no trimming" and must never collapse the history.
        const QList<ChatMessage> untouched = compressMessageHistory(messages, 0, 1000000, true);
        QCOMPARE(untouched.size(), messages.size());
    }
};

QTEST_GUILESS_MAIN(TestLlmParse)
#include "test_llmparse.moc"
