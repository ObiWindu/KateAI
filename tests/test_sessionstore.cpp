/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/worktree.h"
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

    void cleanup()
    {
        SubagentWorktree::setCacheRootForTests(QString());
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

    void largeConversationRoundTripsThroughJournal()
    {
        const QString id = SessionStore::createNewConversation();
        QVERIFY(!id.isEmpty());

        SessionStore::SessionData data;
        for (int i = 0; i < 250; ++i) {
            ChatMessage msg;
            msg.role = (i % 2) ? ChatMessage::Role::Assistant : ChatMessage::Role::User;
            msg.content = QStringLiteral("message-%1-").arg(i) + QString(1000, u'x');
            data.messages.append(msg);
        }
        // saveConversation() returns whether the conversation was new, so an
        // update intentionally returns false; the loaded state is the check.
        QVERIFY(SessionStore::saveConversation(id, data, u"Large"_s));

        for (int i = 250; i < 260; ++i) {
            ChatMessage msg;
            msg.role = ChatMessage::Role::User;
            msg.content = QStringLiteral("message-%1-").arg(i) + QString(1000, u'x');
            data.messages.append(msg);
        }
        SessionStore::saveConversation(id, data);

        const SessionStore::SessionData loaded = SessionStore::loadConversation(id);
        QCOMPARE(loaded.messages.size(), 260);
        QCOMPARE(loaded.messages.last().content, data.messages.last().content);

        // Compaction replaces the shape of the list. The journal must fall
        // back to a full revision instead of appending to the old prefix.
        data.messages = data.messages.mid(200);
        SessionStore::saveConversation(id, data);
        const SessionStore::SessionData compacted = SessionStore::loadConversation(id);
        QCOMPARE(compacted.messages.size(), 60);
        QCOMPARE(compacted.messages.first().content, data.messages.first().content);
        QCOMPARE(compacted.messages.last().content, data.messages.last().content);

        SessionStore::deleteConversation(id);
        const SessionStore::SessionData removed = SessionStore::loadConversation(id);
        QVERIFY(removed.messages.isEmpty());
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

    void subagentRecordsSurviveARoundTrip()
    {
        const QString id = SessionStore::createNewConversation();
        QVERIFY(!id.isEmpty());

        QJsonObject record;
        record.insert(u"taskId"_s, u"call-1"_s);
        record.insert(u"status"_s, u"ok"_s);
        record.insert(u"worktreePath"_s, u"/tmp/worktree"_s);
        record.insert(u"patchPath"_s, u"/tmp/worktree.patch"_s);
        record.insert(u"filesChanged"_s, QJsonArray{u"src/a.cpp"_s});

        SessionStore::SessionData data;
        data.subagents.append(record);
        QVERIFY(SessionStore::saveConversation(id, data, u"Sub-agents"_s));

        const SessionStore::SessionData loaded = SessionStore::loadConversation(id);
        QCOMPARE(loaded.subagents.size(), 1);
        const QJsonObject restored = loaded.subagents.first().toObject();
        QCOMPARE(restored.value(u"taskId"_s).toString(), u"call-1"_s);
        QCOMPARE(restored.value(u"patchPath"_s).toString(), u"/tmp/worktree.patch"_s);
        QCOMPARE(restored.value(u"filesChanged"_s).toArray().size(), 1);
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
        data.acpSessionId = u"acp-source-session"_s;
        QJsonObject worktreeRecord;
        worktreeRecord.insert(u"taskId"_s, u"call-source-worktree"_s);
        worktreeRecord.insert(u"worktreePath"_s, u"/tmp/source-worktree"_s);
        data.subagents.append(worktreeRecord);
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
        // The fork must not attach to the source thread's ACP session or
        // inherit its pending worktree paths.
        QVERIFY(fork.acpSessionId.isEmpty());
        QVERIFY(fork.subagents.isEmpty());

        // The source remains untouched, including its agent-side session and
        // the worktree it still owns.
        const SessionStore::SessionData source = SessionStore::loadConversation(sourceId);
        QVERIFY(source.parentConversationId.isEmpty());
        QCOMPARE(source.messages.size(), 2);
        QCOMPARE(source.acpSessionId, u"acp-source-session"_s);
        QCOMPARE(source.subagents.size(), 1);
    }

    void deletingAConversationRemovesItsSavedWorktrees()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        SubagentWorktree::setCacheRootForTests(cache.path());
        const QString worktree = cache.filePath(u"kateai-test-worktree"_s);
        QVERIFY(QDir().mkpath(worktree));
        QVERIFY(QFileInfo(worktree).isDir());

        const QString id = SessionStore::createNewConversation();
        QVERIFY(!id.isEmpty());
        SessionStore::SessionData data;
        ChatMessage msg;
        msg.role = ChatMessage::Role::User;
        msg.content = u"isolated work"_s;
        data.messages = {msg};
        QJsonObject record;
        record.insert(u"taskId"_s, u"call-worktree"_s);
        record.insert(u"worktreePath"_s, worktree);
        data.subagents.append(record);
        QVERIFY(SessionStore::saveConversation(id, data, u"Worktree"_s));

        SessionStore::deleteConversation(id);
        QVERIFY2(!QFileInfo::exists(worktree),
                 "deleting a conversation left its isolated worktree on disk");
    }
};

QTEST_GUILESS_MAIN(TestSessionStore)
#include "test_sessionstore.moc"
