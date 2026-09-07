#include "llmclient.h"
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
    }

    void settingsKeys()
    {
        Settings s;
        s.grokApiKey = u"xai-test"_s;
        s.openaiApiKey = u"sk-test"_s;
        s.openrouterApiKey = u"or-test"_s;
        s.provider = Provider::Grok;
        QCOMPARE(apiKeyFor(s), u"xai-test"_s);
        s.provider = Provider::OpenAI;
        QCOMPARE(apiKeyFor(s), u"sk-test"_s);
        s.provider = Provider::OpenRouter;
        QCOMPARE(apiKeyFor(s), u"or-test"_s);
    }

    void planModeOnlyAdvertisesReadTools()
    {
        const QJsonArray tools = toolDefinitions(true);
        QCOMPARE(tools.size(), 4);
        for (const QJsonValue &tool : tools) {
            const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
            QVERIFY(name == u"read_file"_s || name == u"list_dir"_s || name == u"grep"_s || name == u"glob"_s);
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
};

QTEST_GUILESS_MAIN(TestLlmParse)
#include "test_llmparse.moc"
