#include "contextmanager.h"

#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestContextManager : public QObject
{
    Q_OBJECT

private:
    static ChatMessage system(const QString &text)
    {
        ChatMessage m;
        m.role = ChatMessage::Role::System;
        m.content = text;
        return m;
    }
    static ChatMessage user(const QString &text)
    {
        ChatMessage m;
        m.role = ChatMessage::Role::User;
        m.content = text;
        return m;
    }
    static ChatMessage assistant(const QString &text)
    {
        ChatMessage m;
        m.role = ChatMessage::Role::Assistant;
        m.content = text;
        return m;
    }
    static ChatMessage toolCall(const QString &name, const QString &path)
    {
        ChatMessage m;
        m.role = ChatMessage::Role::Assistant;
        m.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("c1")},
                                       {QStringLiteral("function"),
                                        QJsonObject{{QStringLiteral("name"), name},
                                                    {QStringLiteral("arguments"),
                                                     QJsonObject{{QStringLiteral("path"), path}}}}}});
        return m;
    }
    static ChatMessage toolResult(const QString &text)
    {
        ChatMessage m;
        m.role = ChatMessage::Role::Tool;
        m.toolCallId = QStringLiteral("c1");
        m.content = text;
        return m;
    }
    // A conversation long enough to force compaction.
    static QList<ChatMessage> longHistory(int turns)
    {
        QList<ChatMessage> history;
        history << system(QStringLiteral("You are a helpful agent."));
        for (int i = 0; i < turns; ++i) {
            history << user(QStringLiteral("Request number %1: please do the thing for component %1.").arg(i));
            history << toolCall(QStringLiteral("edit_file"), QStringLiteral("src/file%1.cpp").arg(i));
            history << toolResult(QStringLiteral("Edited file%1. %1").arg(i).repeated(40));
        }
        history << user(QStringLiteral("And finally, summarise everything."));
        return history;
    }

private Q_SLOTS:
    void tokenEstimateIsRoughButOrdered()
    {
        QVERIFY(ContextManager::estimateTokens(QString()) == 0);
        QVERIFY(ContextManager::estimateTokens(QStringLiteral("a")) >= 1);
        QVERIFY(ContextManager::estimateTokens(QString(400, QLatin1Char('x')))
                > ContextManager::estimateTokens(QString(40, QLatin1Char('x'))));
    }

    void contextWindowFallsBackWithoutAModelNameTable()
    {
        // Deliberately no per-model table any more: it went stale on every
        // model release and could not know about new ones. Every name must now
        // return the same safe default, which is what settings.contextWindow or
        // the provider catalogue overrides.
        const int fallback = ContextManager::contextWindowFor(QStringLiteral("anything"));
        QVERIFY(fallback > 0);
        for (const QString &name : {QStringLiteral("gpt-4o-mini"), QStringLiteral("claude-sonnet-4-5"),
                                    QStringLiteral("grok-4.5"), QStringLiteral("some-unreleased-model")}) {
            QCOMPARE(ContextManager::contextWindowFor(name), fallback);
        }
        QCOMPARE(ContextManager::contextWindowFor(QString()), fallback);
    }

    void shortConversationIsSentUntouched()
    {
        QList<ChatMessage> history;
        history << system(QStringLiteral("system"));
        history << user(QStringLiteral("hello"));
        history << assistant(QStringLiteral("hi"));

        ContextManager::Options options;
        options.contextWindow = 200000;
        const auto result = ContextManager::build(history, options);
        // Compacting a conversation that fits throws detail away for nothing.
        QCOMPARE(result.size(), history.size());
        QCOMPARE(result.at(1).content, QStringLiteral("hello"));
    }

    void longConversationIsCompactedIntoBudget()
    {
        ContextManager::Options options;
        options.contextWindow = 8192;
        options.reserveForResponse = 2048;
        options.keepRecentTokens = 1500;
        const auto result = ContextManager::build(longHistory(40), options);

        QVERIFY(result.size() < 40 * 3);
        QVERIFY(ContextManager::estimateTokens(result)
                <= options.contextWindow - options.reserveForResponse);
    }

    void summaryPreservesUserIntentAndFiles()
    {
        ContextManager::Options options;
        options.contextWindow = 8192;
        options.reserveForResponse = 2048;
        options.keepRecentTokens = 1200;
        const auto result = ContextManager::build(longHistory(40), options);

        QString joined;
        for (const ChatMessage &m : result) {
            joined += m.content + QLatin1Char('\n');
        }
        QVERIFY2(joined.contains(ContextManager::summaryHeader()),
                 "the compacted span must be marked, or the model reads it as real history");
        // The oldest request and an early file must both survive.
        QVERIFY(joined.contains(QStringLiteral("Request number 0")));
        QVERIFY(joined.contains(QStringLiteral("src/file0.cpp")));
    }

    void toolCallsAreNeverSplitFromResults()
    {
        ContextManager::Options options;
        options.contextWindow = 8192;
        options.reserveForResponse = 2048;
        // A range of tail sizes, because the cut point moves with them.
        for (int keep : {300, 700, 1200, 2500, 4000, 6000}) {
            options.keepRecentTokens = keep;
            const auto result = ContextManager::build(longHistory(40), options);
            // Every Tool message must be preceded by the assistant turn that
            // issued its call; an orphan makes providers reject the request.
            for (int i = 0; i < result.size(); ++i) {
                if (result.at(i).role != ChatMessage::Role::Tool) {
                    continue;
                }
                QVERIFY2(i > 0, "a tool result with nothing before it");
                const ChatMessage &previous = result.at(i - 1);
                const bool issuedCall = previous.role == ChatMessage::Role::Assistant
                    && !previous.toolCalls.isEmpty();
                QVERIFY2(issuedCall, "orphaned tool result in the compacted history");
            }
        }
    }

    void systemPromptIsNeverCompacted()
    {
        ContextManager::Options options;
        options.contextWindow = 4096;
        options.reserveForResponse = 1024;
        options.keepRecentTokens = 500;
        const auto result = ContextManager::build(longHistory(40), options);
        QVERIFY(!result.isEmpty());
        QCOMPARE(result.first().role, ChatMessage::Role::System);
        QVERIFY(result.first().content.contains(QStringLiteral("helpful agent")));
    }

    void lastUserMessageIsAlwaysKept()
    {
        ContextManager::Options options;
        options.contextWindow = 4096;
        options.reserveForResponse = 1024;
        options.keepRecentTokens = 200;
        const auto result = ContextManager::build(longHistory(40), options);
        QVERIFY(!result.isEmpty());
        QCOMPARE(result.last().content, QStringLiteral("And finally, summarise everything."));
    }

    void rejectedToolCallsAreFlagged()
    {
        QList<ChatMessage> history;
        history << system(QStringLiteral("sys"));
        for (int i = 0; i < 30; ++i) {
            history << user(QStringLiteral("do %1").arg(i));
            history << assistant(QStringLiteral("ok"));
        }
        ChatMessage rejected;
        rejected.role = ChatMessage::Role::Tool;
        rejected.toolCallId = QStringLiteral("c9");
        rejected.content = QStringLiteral("Tool rejected: edit_file was blocked.");
        history << rejected;
        history << user(QStringLiteral("carry on"));

        ContextManager::Options options;
        options.contextWindow = 4096;
        options.reserveForResponse = 1024;
        options.keepRecentTokens = 200;
        const auto result = ContextManager::build(history, options);
        QString joined;
        for (const ChatMessage &m : result) {
            joined += m.content + QLatin1Char('\n');
        }
        // Otherwise the model assumes the rejected edit landed.
        QVERIFY2(joined.contains(QStringLiteral("rejected")), "a rejected edit must be called out");
    }

    void emptyAndTinyInputsAreSafe()
    {
        ContextManager::Options options;
        QVERIFY(ContextManager::build({}, options).isEmpty());
        const auto one = ContextManager::build({user(QStringLiteral("hi"))}, options);
        QCOMPARE(one.size(), 1);
    }

    void noMessageCountCapDropsHistoryThatFits()
    {
        // A conversation that fits the window is sent whole. There is no
        // parallel "max messages" cutoff that would drop prompting mid-task.
        ContextManager::Options options;
        options.contextWindow = 200000;
        options.reserveForResponse = 1024;
        const auto history = longHistory(8);
        const auto result = ContextManager::build(history, options);
        QCOMPARE(result.size(), history.size());
        QCOMPARE(result.last().content, QStringLiteral("And finally, summarise everything."));
    }

    void tinyWindowStillKeepsTheLatestUser()
    {
        ContextManager::Options options;
        options.contextWindow = 2048;
        options.reserveForResponse = 512;
        options.keepRecentTokens = 200;
        options.compactionLevel = 3;
        const auto result = ContextManager::build(longHistory(40), options);
        QVERIFY(!result.isEmpty());
        QVERIFY(ContextManager::estimateTokens(result)
                <= options.contextWindow - options.reserveForResponse);
        bool sawLatest = false;
        for (const ChatMessage &m : result) {
            if (m.content.contains(QStringLiteral("And finally, summarise everything."))) {
                sawLatest = true;
                break;
            }
        }
        QVERIFY2(sawLatest, "compaction must not drop the live user request");
    }

    void extraCompactionPressureShrinksFurther()
    {
        ContextManager::Options mild;
        mild.contextWindow = 8192;
        mild.reserveForResponse = 2048;
        mild.keepRecentTokens = 4000;
        mild.compactionLevel = 0;
        ContextManager::Options hard = mild;
        hard.compactionLevel = 4;
        const auto history = longHistory(40);
        const auto mildResult = ContextManager::build(history, mild);
        const auto hardResult = ContextManager::build(history, hard);
        QVERIFY(ContextManager::estimateTokens(hardResult)
                <= ContextManager::estimateTokens(mildResult));
        QVERIFY(ContextManager::estimateTokens(hardResult)
                <= hard.contextWindow - hard.reserveForResponse);
    }
};

QTEST_GUILESS_MAIN(TestContextManager)
#include "test_contextmanager.moc"