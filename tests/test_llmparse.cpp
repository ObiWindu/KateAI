#include "llmclient.h"
#include "modes.h"
#include "types.h"

#include <QHash>
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
        QCOMPARE(tools.size(), 5);
        for (const QJsonValue &tool : tools) {
            const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
            QVERIFY(name == u"read_file"_s || name == u"list_dir"_s || name == u"grep"_s || name == u"glob"_s
                    || name == u"query_project_graph"_s);
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
