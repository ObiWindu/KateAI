#include "llmclient.h"
#include "modes.h"
#include "types.h"

#include <QHash>
#include <QNetworkRequest>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

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
        QCOMPARE(tools.size(), 8);
        for (const QJsonValue &tool : tools) {
            const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
            QVERIFY(name == u"read_file"_s || name == u"list_dir"_s || name == u"grep"_s || name == u"glob"_s
                    || name == u"query_project_graph"_s || name == u"write_plan_file"_s || name == u"suggest"_s
                    || name == u"question"_s);
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
