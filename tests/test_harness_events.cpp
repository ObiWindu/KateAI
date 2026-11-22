/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/eventstore.h"
#include "harness/events.h"
#include "harness/replay.h"
#include "sessionstore.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

class TestHarnessEvents : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("KATEAI_SESSION_DB", m_dir.filePath(u"sessions.sqlite"_s).toUtf8());
    }

    void redactsCredentials()
    {
        QJsonObject nested;
        nested.insert(u"api_key"_s, u"sk-abcdefghijklmnop"_s);
        nested.insert(u"note"_s, u"Bearer abcdefghijklmnop"_s);

        QJsonObject payload;
        payload.insert(u"authorization"_s, u"Bearer secret-token-value"_s);
        payload.insert(u"nested"_s, nested);
        payload.insert(u"command"_s, u"OPENAI_API_KEY=sk-abcdefghijklmnop run"_s);
        payload.insert(u"tokens"_s, 42);

        const QJsonObject redacted = Events::redact(payload);
        QCOMPARE(redacted.value(u"authorization"_s).toString(), u"[redacted]"_s);
        QCOMPARE(redacted.value(u"nested"_s).toObject().value(u"api_key"_s).toString(),
                 u"[redacted]"_s);
        QVERIFY(!redacted.value(u"command"_s).toString().contains(u"sk-abcdefghijklmnop"_s));
        QVERIFY(redacted.value(u"command"_s).toString().contains(u"[redacted]"_s));
        // A token count is not a credential.
        QCOMPARE(redacted.value(u"tokens"_s).toInt(), 42);
    }

    void appendsReadsAndProjects()
    {
        EventStore store(m_dir.filePath(u"events.sqlite"_s));
        QVERIFY(store.open());

        const QString conversation = u"conv-events"_s;
        QJsonObject userPayload;
        userPayload.insert(u"userText"_s, u"Please fix the build"_s);
        QJsonObject callObject;
        callObject.insert(u"id"_s, u"call-1"_s);
        callObject.insert(u"name"_s, u"read_file"_s);
        callObject.insert(u"arguments"_s, u"{\"path\":\"a.cpp\"}"_s);
        QJsonArray calls;
        calls.append(callObject);
        QJsonObject assistantPayload;
        assistantPayload.insert(u"text"_s, u"Reading."_s);
        assistantPayload.insert(u"toolCalls"_s, calls);
        QJsonObject resultPayload;
        resultPayload.insert(u"toolCallId"_s, u"call-1"_s);
        resultPayload.insert(u"name"_s, u"read_file"_s);
        resultPayload.insert(u"output"_s, u"contents"_s);

        QList<HarnessEvent> events;
        events.append(Events::make(EventTypes::TurnStart, userPayload, u"turn-1"_s));
        events.append(Events::make(EventTypes::AssistantMessage, assistantPayload, u"turn-1"_s,
                                   u"step-1"_s));
        events.append(Events::make(EventTypes::ToolResult, resultPayload, u"turn-1"_s, u"step-1"_s));
        QVERIFY(store.append(conversation, events));
        QCOMPARE(store.headSeq(conversation), 3);

        const QList<HarnessEvent> read = store.read(conversation);
        QCOMPARE(read.size(), 3);
        QCOMPARE(read.at(0).type, EventTypes::TurnStart);
        QCOMPARE(read.at(1).seq, 2);
        QCOMPARE(read.at(1).turnId, u"turn-1"_s);
        QCOMPARE(read.at(1).stepId, u"step-1"_s);

        const QList<ChatMessage> messages = store.projectMessages(conversation);
        QCOMPARE(messages.size(), 3);
        QCOMPARE(messages.at(0).role, ChatMessage::Role::User);
        QCOMPARE(messages.at(0).content, u"Please fix the build"_s);
        QCOMPARE(messages.at(1).role, ChatMessage::Role::Assistant);
        QCOMPARE(messages.at(1).toolCalls.size(), 1);
        QCOMPARE(messages.at(1).toolCalls.first().toObject().value(u"function"_s).toObject()
                     .value(u"name"_s).toString(),
                 u"read_file"_s);
        QCOMPARE(messages.at(2).role, ChatMessage::Role::Tool);
        QCOMPARE(messages.at(2).toolCallId, u"call-1"_s);
        QCOMPARE(messages.at(2).content, u"contents"_s);

        QVERIFY(store.rebuildProjection(conversation));
        QCOMPARE(store.projectMessages(conversation).size(), 3);
        QVERIFY(store.removeConversation(conversation));
        QCOMPARE(store.headSeq(conversation), 0);
    }

    void rejectsOutOfOrderSequence()
    {
        EventStore store(m_dir.filePath(u"events-order.sqlite"_s));
        QVERIFY(store.open());
        QList<HarnessEvent> events;
        HarnessEvent event = Events::make(EventTypes::Notice, {});
        event.seq = 5;
        events.append(event);
        QVERIFY(store.append(u"conv-order"_s, events));
        QVERIFY(!store.append(u"conv-order"_s, events));
        QCOMPARE(store.headSeq(u"conv-order"_s), 5);
    }

    void backfillsLegacyConversation()
    {
        const QString id = SessionStore::createNewConversation();
        QVERIFY(!id.isEmpty());

        SessionStore::SessionData data;
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"Legacy question"_s;
        data.messages.append(user);
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"Legacy answer"_s;
        data.messages.append(assistant);
        QVERIFY(SessionStore::saveConversation(id, data, u"Legacy"_s));

        EventStore store;
        QVERIFY(store.open());
        QVERIFY(store.backfillConversation(id));
        const QList<HarnessEvent> events = store.read(id);
        QVERIFY(!events.isEmpty());
        QVERIFY(events.first().imported);
        const QList<ChatMessage> projected = store.projectMessages(id);
        QCOMPARE(projected.size(), 2);
        QCOMPARE(projected.at(0).content, u"Legacy question"_s);
        QCOMPARE(projected.at(1).content, u"Legacy answer"_s);

        // A second backfill is a no-op.
        const qint64 head = store.headSeq(id);
        QVERIFY(store.backfillConversation(id));
        QCOMPARE(store.headSeq(id), head);
    }

    void replayPairsCallsAndResults()
    {
        QList<HarnessEvent> events;
        events.append(Events::make(EventTypes::StepStart, {}, u"turn"_s, u"step"_s));
        QJsonObject callObject;
        callObject.insert(u"id"_s, u"call-1"_s);
        callObject.insert(u"name"_s, u"edit_file"_s);
        QJsonArray calls;
        calls.append(callObject);
        QJsonObject assistant;
        assistant.insert(u"text"_s, u"Editing."_s);
        assistant.insert(u"toolCalls"_s, calls);
        events.append(Events::make(EventTypes::AssistantMessage, assistant, u"turn"_s, u"step"_s));
        QJsonObject result;
        result.insert(u"toolCallId"_s, u"call-1"_s);
        result.insert(u"name"_s, u"edit_file"_s);
        result.insert(u"output"_s, u"ok"_s);
        events.append(Events::make(EventTypes::ToolResult, result, u"turn"_s, u"step"_s));

        ReplayReport report = Replay::analyze(events);
        QVERIFY2(report.ok, qPrintable(report.errors.join(u"; "_s)));
        QCOMPARE(report.steps.size(), 1);
        QCOMPARE(report.toolCalls, 1);
        QCOMPARE(report.toolResults, 1);
        QCOMPARE(report.finalAssistant, u"Editing."_s);

        events.removeLast();
        report = Replay::analyze(events);
        QVERIFY(!report.ok);
        QVERIFY(report.errors.join(u"; "_s).contains(u"never produced a result"_s));
    }
};

QTEST_GUILESS_MAIN(TestHarnessEvents)
#include "test_harness_events.moc"
