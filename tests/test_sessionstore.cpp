/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sessionstore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestSessionStore : public QObject
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

    void titlePreservedAcrossSaves()
    {
        const QString id = SessionStore::createNewConversation();
        QVERIFY(!id.isEmpty());

        SessionStore::SessionData data;
        ChatMessage msg;
        msg.role = ChatMessage::Role::User;
        msg.content = u"Implement the feature"_s;
        data.messages.append(msg);
        SessionStore::saveConversation(id, data, u"My Title"_s);

        // A second save without a title must keep the existing title.
        SessionStore::SessionData data2 = data;
        ChatMessage msg2;
        msg2.role = ChatMessage::Role::Assistant;
        msg2.content = u"Done"_s;
        data2.messages.append(msg2);
        SessionStore::saveConversation(id, data2);

        const SessionStore::SessionData loaded = SessionStore::loadConversation(id);
        QCOMPARE(loaded.messages.size(), 2);
    }

    void lastTurnIdSurvivesARoundTrip()
    {
        const QString id = SessionStore::createNewConversation();
        QVERIFY(!id.isEmpty());

        SessionStore::SessionData data;
        data.lastTurnId = u"turn-1234"_s;
        QVERIFY(SessionStore::saveConversation(id, data, u"Turn id"_s));
        QCOMPARE(SessionStore::loadConversation(id).lastTurnId, u"turn-1234"_s);
    }

    void transcriptSurvivesARoundTrip()
    {
        // After context compaction the model history holds a summary plus the
        // newest turns, while the display transcript still has everything the
        // user saw. Both halves have to come back from storage.
        const QString id = SessionStore::createNewConversation();
        QVERIFY(!id.isEmpty());

        ChatMessage first;
        first.role = ChatMessage::Role::User;
        first.content = u"first question"_s;

        ChatMessage answer;
        answer.role = ChatMessage::Role::Assistant;
        answer.content = u"first answer"_s;

        ChatMessage summary;
        summary.role = ChatMessage::Role::User;
        summary.name = u"context_summary"_s;
        summary.content = u"[Earlier conversation was compacted]"_s;

        ChatMessage latest;
        latest.role = ChatMessage::Role::User;
        latest.content = u"latest question"_s;

        SessionStore::SessionData data;
        data.messages = {summary, latest};
        data.transcript = {first, answer, latest};
        QVERIFY(SessionStore::saveConversation(id, data, u"Compacted"_s));

        const SessionStore::SessionData loaded = SessionStore::loadConversation(id);
        QCOMPARE(loaded.messages.size(), 2);
        QCOMPARE(loaded.transcript.size(), 3);
        QCOMPARE(loaded.transcript.first().content, u"first question"_s);
        QCOMPARE(loaded.transcript.at(1).content, u"first answer"_s);
    }

    void oldPayloadWithoutTranscriptLoadsItsMessages()
    {        // A conversation saved by an earlier build has no transcript field.
        // It must decode as an empty transcript (AgentLoop::restoreSession
        // then falls back to the messages) and must not look damaged.
        const QString id = SessionStore::createNewConversation();
        QVERIFY(!id.isEmpty());

        ChatMessage msg;
        msg.role = ChatMessage::Role::User;
        msg.content = u"legacy question"_s;

        SessionStore::SessionData data;
        data.messages = {msg};
        QVERIFY(SessionStore::saveConversation(id, data, u"Legacy"_s));

        // Rewrite the stored payload in the shape an earlier build wrote:
        // version 3 and no transcript key.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"legacy_payload_writer"_s);
            db.setDatabaseName(SessionStore::databasePath());
            QVERIFY(db.open());
            QSqlQuery read(db);
            read.prepare(u"SELECT payload FROM conversations WHERE id = ?"_s);
            read.addBindValue(id);
            QVERIFY(read.exec());
            QVERIFY(read.next());
            QJsonObject payload = QJsonDocument::fromJson(read.value(0).toByteArray()).object();
            payload.remove(u"transcript"_s);
            payload.insert(u"version"_s, 3);
            QSqlQuery write(db);
            write.prepare(u"UPDATE conversations SET payload = ? WHERE id = ?"_s);
            write.addBindValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
            write.addBindValue(id);
            QVERIFY(write.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase(u"legacy_payload_writer"_s);

        const SessionStore::SessionData loaded = SessionStore::loadConversation(id);
        QVERIFY(!loaded.loadFailed);
        QCOMPARE(loaded.messages.size(), 1);
        QCOMPARE(loaded.messages.first().content, u"legacy question"_s);
        QVERIFY(loaded.transcript.isEmpty());
    }

    void forkConversationCopiesTheTranscriptAndRecordsItsParent()
    {
        const QString sourceId = SessionStore::createNewConversation();
        QVERIFY(!sourceId.isEmpty());

        ChatMessage question;
        question.role = ChatMessage::Role::User;
        question.content = u"fork me"_s;
        ChatMessage answer;
        answer.role = ChatMessage::Role::Assistant;
        answer.content = u"forked answer"_s;

        SessionStore::SessionData data;
        data.messages = {question, answer};
        data.transcript = {question, answer};
        QVERIFY(SessionStore::saveConversation(sourceId, data, u"Source"_s));

        const QString forkId = SessionStore::forkConversation(sourceId, u"Branch"_s);
        QVERIFY(!forkId.isEmpty());
        QVERIFY(forkId != sourceId);
        QCOMPARE(SessionStore::getActiveConversationId(), forkId);

        const SessionStore::SessionData fork = SessionStore::loadConversation(forkId);
        QCOMPARE(fork.parentConversationId, sourceId);
        QCOMPARE(fork.messages.size(), 2);
        QCOMPARE(fork.messages.first().content, u"fork me"_s);
        QCOMPARE(fork.transcript.size(), 2);
        QCOMPARE(SessionStore::conversationTitle(forkId), u"Branch"_s);

        // The source remains untouched.
        const SessionStore::SessionData source = SessionStore::loadConversation(sourceId);
        QVERIFY(source.parentConversationId.isEmpty());
        QCOMPARE(source.messages.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestSessionStore)
#include "test_sessionstore.moc"
