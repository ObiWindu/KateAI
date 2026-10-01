/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Conversation history regression tests.
 *
 * "New Thread" used to lose the conversation that had just finished: nothing
 * claimed a conversation id when the first question was sent, so turnFinished()
 * skipped its save and the conversation lived in memory only, and newChat()
 * then deleted the active record from disk through
 * AgentLoop::clearSession() -> SessionStore::clear().
 */

#include "chatwidget.h"
#include "promptedit.h"
#include "sessionstore.h"
#include "settings.h"

#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QTest>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

class TestConversations : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        SessionStore::clearAllConversations();
    }

    void cleanupTestCase()
    {
        SessionStore::clearAllConversations();
        QStandardPaths::setTestModeEnabled(false);
    }

    /** The reported flow: ask something, click New Thread, look at the history. */
    void conversationSurvivesNewThread()
    {
        ChatWidget chat;
        chat.resize(600, 700);
        chat.show();
        QVERIFY(QTest::qWaitForWindowExposed(&chat));
        chat.setSettings(SettingsStore::load());
        chat.agent()->setWorkspace(QDir::currentPath());

        // Type a question and press send, exactly like a user.
        QVERIFY(chat.promptEdit());
        chat.promptEdit()->setPlainText(u"请记住这句话"_s);
        QPushButton *send = nullptr;
        const QList<QPushButton *> buttons = chat.findChildren<QPushButton *>();
        for (QPushButton *button : buttons) {
            if (button->toolTip().contains(u"Send"_s)) {
                send = button;
                break;
            }
        }
        QVERIFY2(send, "no send button in the chat widget");
        send->click();
        QTest::qWait(50);

        // The turn cannot reach a model here, so end it the way the agent
        // signals completion, then let the widget act on it.
        QVERIFY(QMetaObject::invokeMethod(chat.agent(), "turnFinished"));
        QTest::qWait(50);

        auto listed = SessionStore::listConversations(0);
        QCOMPARE(listed.size(), 1);
        const QString previousId = listed.first().id;
        QVERIFY(!previousId.isEmpty());

        const auto stored = SessionStore::loadConversation(previousId);
        QVERIFY(!stored.messages.isEmpty());
        bool sawUserText = false;
        for (const auto &message : stored.messages) {
            sawUserText = sawUserText || message.content.contains(u"请记住这句话"_s);
        }
        QVERIFY2(sawUserText, "the question was not persisted");

        // Click "New Thread" and look back: the conversation must still be there.
        chat.newChat();
        QTest::qWait(50);

        listed = SessionStore::listConversations(0);
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed.first().id, previousId);
        QCOMPARE(SessionStore::loadConversation(previousId).messages.size(),
                 stored.messages.size());
        QVERIFY2(SessionStore::getActiveConversationId() != previousId,
                 "New Thread reused the previous conversation id");
    }

    /** A finished turn must always be written, even with no id claimed yet. */
    void finishedTurnIsPersisted()
    {
        ChatWidget chat;
        chat.setSettings(SettingsStore::load());
        chat.agent()->setWorkspace(QDir::currentPath());
        chat.agent()->start(u"persist me"_s);
        chat.agent()->abort();
        QTest::qWait(30);

        QVERIFY(QMetaObject::invokeMethod(chat.agent(), "turnFinished"));
        QTest::qWait(30);

        const auto listed = SessionStore::listConversations(0);
        QCOMPARE(listed.size(), 1);
        const QString id = listed.first().id;
        QVERIFY(listed.first().messageCount >= 2);

        const auto reloaded = SessionStore::loadConversation(id);
        QCOMPARE(reloaded.messages.size(), listed.first().messageCount);
        QCOMPARE(reloaded.messages.last().content, u"persist me"_s);
    }

    /** A follow-up turn must append to the restored conversation. */
    void followUpTurnKeepsEarlierMessages()
    {
        SessionStore::SessionData stored;
        ChatMessage system;
        system.role = ChatMessage::Role::System;
        system.content = u"System prompt"_s;
        ChatMessage user;
        user.role = ChatMessage::Role::User;
        user.content = u"first question"_s;
        ChatMessage assistant;
        assistant.role = ChatMessage::Role::Assistant;
        assistant.content = u"first answer"_s;
        stored.messages = {system, user, assistant};
        SessionStore::save(stored);
        const QString id = SessionStore::getActiveConversationId();
        QVERIFY(!id.isEmpty());

        ChatWidget chat;
        chat.agent()->restoreSession(SessionStore::load());
        chat.setCurrentConversationId(id);
        QCOMPARE(chat.agent()->messages().size(), 3);

        chat.agent()->setWorkspace(QDir::currentPath());
        chat.agent()->start(u"second question"_s);

        const QList<ChatMessage> messages = chat.agent()->messages();
        QCOMPARE(messages.size(), 4);
        QCOMPARE(messages.at(1).content, u"first question"_s);
        QCOMPARE(messages.at(2).content, u"first answer"_s);
        QCOMPARE(messages.at(3).role, ChatMessage::Role::User);
        QCOMPARE(messages.at(3).content, u"second question"_s);

        chat.agent()->abort();
    }
};

QTEST_MAIN(TestConversations)
#include "test_conversations.moc"
