/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sessionstore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

/**
 * The session store must survive a damaged database file: the file is moved
 * aside (not deleted, so it can still be inspected) and a fresh store is
 * created, rather than the application reporting "no history" forever.
 *
 * This lives in its own test binary because the store caches one connection per
 * thread; the damage has to be in place before the first SessionStore call.
 */
class TestSessionRecovery : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("KATEAI_SESSION_DB", m_dir.filePath(u"sessions.sqlite"_s).toUtf8());

        // A truncated text file is exactly the kind of corruption that opens
        // lazily and then fails the integrity check.
        QFile corrupt(m_dir.filePath(u"sessions.sqlite"_s));
        QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        corrupt.write("this is not a sqlite database\n");
        corrupt.close();
    }

    void corruptDatabaseIsQuarantinedAndReplaced()
    {
        const QString id = SessionStore::createNewConversation();
        QVERIFY2(!id.isEmpty(), "the store did not recover from the damaged database");

        const QStringList entries = QDir(m_dir.path()).entryList(QDir::Files);
        bool foundQuarantine = false;
        for (const QString &entry : entries) {
            if (entry.contains(u"sessions.sqlite.corrupt-"_s)) {
                foundQuarantine = true;
                break;
            }
        }
        QVERIFY2(foundQuarantine, qPrintable(entries.join(u", "_s)));

        // The original text is still available for a manual salvage.
        const QStringList names = QDir(m_dir.path()).entryList({u"sessions.sqlite.corrupt-*"_s}, QDir::Files);
        QCOMPARE(names.size(), 1);
        QFile quarantined(m_dir.filePath(names.first()));
        QVERIFY(quarantined.open(QIODevice::ReadOnly));
        QVERIFY(quarantined.readAll().contains("not a sqlite database"));
    }

    void recoveredStoreWorksNormally()
    {
        SessionStore::SessionData data;
        ChatMessage message;
        message.role = ChatMessage::Role::User;
        message.content = u"survived the corruption"_s;
        data.messages.append(message);

        const QString id = SessionStore::createNewConversation();
        QVERIFY(!id.isEmpty());
        QVERIFY(SessionStore::saveConversation(id, data, u"Recovered"_s));

        const SessionStore::SessionData loaded = SessionStore::loadConversation(id);
        QCOMPARE(loaded.messages.size(), 1);
        QCOMPARE(loaded.messages.first().content, u"survived the corruption"_s);
        QCOMPARE(SessionStore::conversationTitle(id), u"Recovered"_s);
    }
};

QTEST_GUILESS_MAIN(TestSessionRecovery)
#include "test_sessionrecovery.moc"
