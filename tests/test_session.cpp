#include "sessionstore.h"
#include "agentloop.h"
#include "types.h"

#include <QTest>
#include <QSignalSpy>
#include <QJsonArray>
#include <QJsonObject>
#include <KConfigGroup>
#include <KSharedConfig>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

class TestSession : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Clear any existing session data
        SessionStore::clear();
    }

    void cleanupTestCase()
    {
        SessionStore::clear();
    }

    void testEmptySession()
    {
        const auto data = SessionStore::load();
        QVERIFY(data.messages.isEmpty());
        QVERIFY(data.currentThinking.isEmpty());
        QVERIFY(data.currentPlan.isEmpty());
        QVERIFY(!data.planShown);
        QVERIFY(data.currentAssistant.isEmpty());
        QCOMPARE(data.stateEpoch, quint64(0));
        QVERIFY(data.actionSignatures.isEmpty());
        QVERIFY(data.actionRepeatCounts.isEmpty());
        QVERIFY(data.changedPaths.isEmpty());
        QVERIFY(!data.changesNeedVerification);
        QVERIFY(!data.verificationAttempted);
        QCOMPARE(data.verificationPromptCount, 0);
        QCOMPARE(data.modelRequests, 0);
        QCOMPARE(data.toolCalls, 0);
    }

    void testSaveAndLoadMessages()
    {
        SessionStore::SessionData data;
        
        // Create test messages
        ChatMessage systemMsg;
        systemMsg.role = ChatMessage::Role::System;
        systemMsg.content = u"System prompt"_s;
        
        ChatMessage userMsg;
        userMsg.role = ChatMessage::Role::User;
        userMsg.content = u"Hello, world!"_s;
        
        ChatMessage assistantMsg;
        assistantMsg.role = ChatMessage::Role::Assistant;
        assistantMsg.content = u"Hi there!"_s;
        assistantMsg.thinking = u"User said hello"_s;
        assistantMsg.plan = QJsonArray::fromStringList({u"Step 1"_s, u"Step 2"_s});
        
        ChatMessage toolMsg;
        toolMsg.role = ChatMessage::Role::Tool;
        toolMsg.content = u"Tool result"_s;
        toolMsg.toolCallId = u"call_123"_s;
        toolMsg.name = u"read_file"_s;
        
        data.messages = {systemMsg, userMsg, assistantMsg, toolMsg};
        data.currentThinking = u"Current thinking"_s;
        data.currentPlan = QJsonArray::fromStringList({u"Plan step 1"_s});
        data.planShown = true;
        data.currentAssistant = u"Current assistant text"_s;
        data.stateEpoch = 42;
        data.actionSignatures = {u"sig1"_s, u"sig2"_s};
        data.actionRepeatCounts = {{u"sig1"_s, 3}, {u"sig2"_s, 1}};
        data.changedPaths = {u"/path/to/file1.cpp"_s, u"/path/to/file2.cpp"_s};
        data.changesNeedVerification = true;
        data.verificationAttempted = false;
        data.verificationPromptCount = 1;
        data.modelRequests = 5;
        data.toolCalls = 10;
        
        SessionStore::save(data);
        
        const auto loaded = SessionStore::load();
        
        QCOMPARE(loaded.messages.size(), 4);
        QCOMPARE(loaded.messages[0].role, ChatMessage::Role::System);
        QCOMPARE(loaded.messages[0].content, u"System prompt"_s);
        QCOMPARE(loaded.messages[1].role, ChatMessage::Role::User);
        QCOMPARE(loaded.messages[1].content, u"Hello, world!"_s);
        QCOMPARE(loaded.messages[2].role, ChatMessage::Role::Assistant);
        QCOMPARE(loaded.messages[2].content, u"Hi there!"_s);
        QCOMPARE(loaded.messages[2].thinking, u"User said hello"_s);
        QCOMPARE(loaded.messages[2].plan.size(), 2);
        QCOMPARE(loaded.messages[3].role, ChatMessage::Role::Tool);
        QCOMPARE(loaded.messages[3].toolCallId, u"call_123"_s);
        QCOMPARE(loaded.messages[3].name, u"read_file"_s);
        
        QCOMPARE(loaded.currentThinking, u"Current thinking"_s);
        QCOMPARE(loaded.currentPlan.size(), 1);
        QVERIFY(loaded.planShown);
        QCOMPARE(loaded.currentAssistant, u"Current assistant text"_s);
        QCOMPARE(loaded.stateEpoch, quint64(42));
        QCOMPARE(loaded.actionSignatures.size(), 2);
        QVERIFY(loaded.actionSignatures.contains(u"sig1"_s));
        QVERIFY(loaded.actionSignatures.contains(u"sig2"_s));
        QCOMPARE(loaded.actionRepeatCounts[u"sig1"_s], 3);
        QCOMPARE(loaded.actionRepeatCounts[u"sig2"_s], 1);
        QCOMPARE(loaded.changedPaths.size(), 2);
        QVERIFY(loaded.changedPaths.contains(u"/path/to/file1.cpp"_s));
        QVERIFY(loaded.changedPaths.contains(u"/path/to/file2.cpp"_s));
        QVERIFY(loaded.changesNeedVerification);
        QVERIFY(!loaded.verificationAttempted);
        QCOMPARE(loaded.verificationPromptCount, 1);
        QCOMPARE(loaded.modelRequests, 5);
        QCOMPARE(loaded.toolCalls, 10);
    }

    void testClearSession()
    {
        SessionStore::SessionData data;
        data.messages.append(ChatMessage{ChatMessage::Role::User, u"Test"_s});
        SessionStore::save(data);
        
        QVERIFY(!SessionStore::load().messages.isEmpty());
        
        SessionStore::clear();
        
        const auto loaded = SessionStore::load();
        QVERIFY(loaded.messages.isEmpty());
    }

    void testAgentLoopSessionData()
    {
        AgentLoop agent;
        
        // Initially empty
        auto sessionData = agent.sessionData();
        QVERIFY(sessionData.messages.isEmpty());
        
        // Test that sessionData() returns a valid structure
        QVERIFY(sessionData.messages.isEmpty());
        QVERIFY(sessionData.currentThinking.isEmpty());
        QVERIFY(sessionData.currentPlan.isEmpty());
        QVERIFY(!sessionData.planShown);
        QCOMPARE(sessionData.modelRequests, 0);
        QCOMPARE(sessionData.toolCalls, 0);
    }

    void testAgentLoopRestoreSession()
    {
        AgentLoop agent;
        
        SessionStore::SessionData data;
        ChatMessage userMsg;
        userMsg.role = ChatMessage::Role::User;
        userMsg.content = u"Test message"_s;
        data.messages = {userMsg};
        data.currentThinking = u"Restored thinking"_s;
        data.currentPlan = QJsonArray::fromStringList({u"Restored plan"_s});
        data.planShown = true;
        data.stateEpoch = 100;
        data.modelRequests = 3;
        data.toolCalls = 5;
        
        agent.restoreSession(data);
        
        const auto restored = agent.sessionData();
        QCOMPARE(restored.messages.size(), 1);
        QCOMPARE(restored.messages[0].content, u"Test message"_s);
        QCOMPARE(restored.currentThinking, u"Restored thinking"_s);
        QCOMPARE(restored.currentPlan.size(), 1);
        QVERIFY(restored.planShown);
        QCOMPARE(restored.stateEpoch, quint64(100));
        QCOMPARE(restored.modelRequests, 3);
        QCOMPARE(restored.toolCalls, 5);
    }

    void testAgentLoopClearSession()
    {
        AgentLoop agent;
        
        SessionStore::SessionData data;
        ChatMessage userMsg;
        userMsg.role = ChatMessage::Role::User;
        userMsg.content = u"Test message"_s;
        data.messages = {userMsg};
        data.modelRequests = 3;
        data.toolCalls = 5;
        
        agent.restoreSession(data);
        QCOMPARE(agent.sessionData().messages.size(), 1);
        
        agent.clearSession();
        
        const auto cleared = agent.sessionData();
        QVERIFY(cleared.messages.isEmpty());
        QCOMPARE(cleared.modelRequests, 0);
        QCOMPARE(cleared.toolCalls, 0);
        
        // Also verify SessionStore was cleared
        QVERIFY(SessionStore::load().messages.isEmpty());
    }

    void testSessionPersistenceRoundTrip()
    {
        // Test full round-trip: AgentLoop -> SessionStore -> AgentLoop
        AgentLoop agent1;
        
        SessionStore::SessionData originalData;
        ChatMessage userMsg;
        userMsg.role = ChatMessage::Role::User;
        userMsg.content = u"Round trip test"_s;
        originalData.messages = {userMsg};
        originalData.currentThinking = u"Thinking content"_s;
        originalData.currentPlan = QJsonArray::fromStringList({u"Plan 1"_s, u"Plan 2"_s});
        originalData.planShown = false;
        originalData.currentAssistant = u"Assistant response"_s;
        originalData.stateEpoch = 999;
        originalData.actionSignatures = {u"action1"_s, u"action2"_s};
        originalData.actionRepeatCounts = {{u"action1"_s, 2}};
        originalData.changedPaths = {u"/changed/file.cpp"_s};
        originalData.changesNeedVerification = true;
        originalData.verificationAttempted = true;
        originalData.verificationPromptCount = 2;
        originalData.modelRequests = 10;
        originalData.toolCalls = 20;
        
        agent1.restoreSession(originalData);
        
        // Save from agent1
        const auto savedData = agent1.sessionData();
        SessionStore::save(savedData);
        
        // Create new agent and restore
        AgentLoop agent2;
        const auto loadedData = SessionStore::load();
        agent2.restoreSession(loadedData);
        
        const auto finalData = agent2.sessionData();
        
        QCOMPARE(finalData.messages.size(), 1);
        QCOMPARE(finalData.messages[0].content, u"Round trip test"_s);
        QCOMPARE(finalData.currentThinking, u"Thinking content"_s);
        QCOMPARE(finalData.currentPlan.size(), 2);
        QVERIFY(!finalData.planShown);
        QCOMPARE(finalData.currentAssistant, u"Assistant response"_s);
        QCOMPARE(finalData.stateEpoch, quint64(999));
        QCOMPARE(finalData.actionSignatures.size(), 2);
        QCOMPARE(finalData.actionRepeatCounts[u"action1"_s], 2);
        QCOMPARE(finalData.changedPaths.size(), 1);
        QVERIFY(finalData.changesNeedVerification);
        QVERIFY(finalData.verificationAttempted);
        QCOMPARE(finalData.verificationPromptCount, 2);
        QCOMPARE(finalData.modelRequests, 10);
        QCOMPARE(finalData.toolCalls, 20);
    }

    void testSessionWithToolCalls()
    {
        SessionStore::SessionData data;
        
        ChatMessage toolMsg;
        toolMsg.role = ChatMessage::Role::Tool;
        toolMsg.content = u"File content here"_s;
        toolMsg.toolCallId = u"call_abc123"_s;
        toolMsg.name = u"read_file"_s;
        toolMsg.toolCalls = QJsonArray();
        
        data.messages = {toolMsg};
        SessionStore::save(data);
        
        const auto loaded = SessionStore::load();
        QCOMPARE(loaded.messages.size(), 1);
        QCOMPARE(loaded.messages[0].role, ChatMessage::Role::Tool);
        QCOMPARE(loaded.messages[0].toolCallId, u"call_abc123"_s);
        QCOMPARE(loaded.messages[0].name, u"read_file"_s);
    }

    void testSessionWithAssistantToolCalls()
    {
        SessionStore::SessionData data;
        
        ChatMessage assistantMsg;
        assistantMsg.role = ChatMessage::Role::Assistant;
        assistantMsg.content = u"I'll read that file"_s;
        assistantMsg.toolCalls = QJsonArray();
        
        QJsonObject toolCall;
        toolCall[u"id"_s] = u"call_123"_s;
        toolCall[u"name"_s] = u"read_file"_s;
        toolCall[u"arguments"_s] = QJsonObject{{u"path"_s, u"/test/file.cpp"_s}};
        assistantMsg.toolCalls.append(toolCall);
        
        data.messages = {assistantMsg};
        SessionStore::save(data);
        
        const auto loaded = SessionStore::load();
        QCOMPARE(loaded.messages.size(), 1);
        QCOMPARE(loaded.messages[0].toolCalls.size(), 1);
        QCOMPARE(loaded.messages[0].toolCalls[0].toObject()[u"name"_s].toString(), u"read_file"_s);
    }
};

} // namespace KateAi

QTEST_MAIN(KateAi::TestSession)
#include "test_session.moc"