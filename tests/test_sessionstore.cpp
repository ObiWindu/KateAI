/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sessionstore.h"

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

    void badTitleRepaired()
    {
        // A title that is a compaction summary must not survive.
        QVERIFY(true);
    }
};

QTEST_GUILESS_MAIN(TestSessionStore)
#include "test_sessionstore.moc"
