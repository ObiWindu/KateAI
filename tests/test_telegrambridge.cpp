#include "telegrambridge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestTelegramBridge : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void allowlistParses()
    {
        const QSet<qlonglong> ids = TelegramBridge::parseAllowedChatIds(u"123, 456 789;1000"_s);
        QVERIFY(ids.contains(123));
        QVERIFY(ids.contains(456));
        QVERIFY(ids.contains(789));
        QVERIFY(ids.contains(1000));
        QVERIFY(!TelegramBridge::isChatAllowed(999, ids));
        QVERIFY(TelegramBridge::isChatAllowed(123, ids));
        QVERIFY(!TelegramBridge::isChatAllowed(0, ids));
        QVERIFY(TelegramBridge::parseAllowedChatIds({}).isEmpty());
    }

    void tokenValidation()
    {
        QVERIFY(TelegramBridge::isTokenWellFormed(u"123456789:AAH-abcdefghijklmnopqrstuvwxyz012345"_s));
        QVERIFY(!TelegramBridge::isTokenWellFormed({}));
        QVERIFY(!TelegramBridge::isTokenWellFormed(u"not-a-token"_s));
        QVERIFY(!TelegramBridge::isTokenWellFormed(u"123:short"_s));
        QVERIFY(!TelegramBridge::isTokenWellFormed(u"123456:abc/def"_s));
    }

    void startRefusesUnsafeConfig()
    {
        TelegramBridge bridge;
        AgentLoop loop;
        bridge.setAgentLoop(&loop);
        QString error;
        TelegramBridge::Config bad;
        bad.botToken = u"bad"_s;
        bad.allowedChatIds = {123};
        QVERIFY(!bridge.start(bad, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!bridge.isRunning());

        TelegramBridge::Config emptyAllow;
        emptyAllow.botToken = u"123456789:AAH-abcdefghijklmnopqrstuvwxyz012345"_s;
        QVERIFY(!bridge.start(emptyAllow, &error));
        QVERIFY(!bridge.isRunning());
    }

    void redactAndTruncate()
    {
        const QString dirty = u"key sk-abcdefgh12345678 and Bearer abcdefgh12345678 and api_key: secret123"_s;
        const QString clean = TelegramBridge::redactSecrets(dirty);
        QVERIFY(!clean.contains(u"sk-abcdefgh12345678"_s));
        QVERIFY(!clean.contains(u"Bearer abcdefgh12345678"_s));
        QVERIFY(!clean.contains(u"secret123"_s));
        QVERIFY(clean.contains(u"[redacted"_s));

        const QString priv = u"-----BEGIN PRIVATE KEY-----\nABC\n-----END PRIVATE KEY-----"_s;
        QVERIFY(TelegramBridge::redactSecrets(priv).contains(u"[redacted-private-key]"_s));

        QCOMPARE(TelegramBridge::truncateForTelegram(u"hello"_s, 10), u"hello"_s);
        QVERIFY(TelegramBridge::truncateForTelegram(QString(5000, u'x'), 3500).size() <= 3500);
        QVERIFY(TelegramBridge::truncateForTelegram(QString(5000, u'x'), 3500).contains(u"truncated"_s));
    }

    void payloadAndKeyboard()
    {
        const QJsonObject payload = TelegramBridge::buildSendMessagePayload(123, u"hi"_s);
        QCOMPARE(payload.value(u"chat_id"_s).toVariant().toLongLong(), 123LL);
        QCOMPARE(payload.value(u"text"_s).toString(), u"hi"_s);

        const QJsonObject kb = TelegramBridge::buildApprovalKeyboard(u"tool-1"_s);
        const QJsonArray row = kb.value(u"inline_keyboard"_s).toArray().at(0).toArray();
        QCOMPARE(row.size(), 2);
        // Callback data stays tiny and carries no internal ids.
        for (const QJsonValue &v : row) {
            QVERIFY(v.toObject().value(u"callback_data"_s).toString().size() <= 64);
        }
        QVERIFY(row.at(0).toObject().value(u"callback_data"_s).toString().startsWith(u"tg:"_s));
    }

    void parseUpdateShapes()
    {
        qlonglong chat = 0;
        QString text, data, cbId;
        qlonglong updateId = 0;
        QJsonObject msg;
        msg.insert(u"chat"_s, QJsonObject{{u"id"_s, 321}});
        QJsonObject update;
        update.insert(u"update_id"_s, 10);
        update.insert(u"message"_s, QJsonObject{{u"chat"_s, QJsonObject{{u"id"_s, 321}}},
                                                {u"text"_s, u"/ask fix bug"_s}});
        QVERIFY(TelegramBridge::parseUpdate(update, &chat, &text, &data, &cbId, &updateId));
        QCOMPARE(chat, 321LL);
        QCOMPARE(text, u"/ask fix bug"_s);
        QCOMPARE(updateId, 10LL);

        QJsonObject cq;
        cq.insert(u"id"_s, u"cb1"_s);
        cq.insert(u"data"_s, u"tg:allow"_s);
        cq.insert(u"message"_s, QJsonObject{{u"chat"_s, QJsonObject{{u"id"_s, 321}}}});
        QJsonObject update2;
        update2.insert(u"update_id"_s, 11);
        update2.insert(u"callback_query"_s, cq);
        chat = 0;
        QVERIFY(TelegramBridge::parseUpdate(update2, &chat, &text, &data, &cbId, &updateId));
        QCOMPARE(chat, 321LL);
        QCOMPARE(data, u"tg:allow"_s);
        QCOMPARE(cbId, u"cb1"_s);
    }

    void commandsAllowlisted()
    {
        QVERIFY(TelegramBridge::isAllowedCommand(u"hello, fix the bug"_s));
        QVERIFY(TelegramBridge::isAllowedCommand(u"/ask fix it"_s));
        QVERIFY(TelegramBridge::isAllowedCommand(u"/stop"_s));
        QVERIFY(TelegramBridge::isAllowedCommand(u"/approve"_s));
        QVERIFY(!TelegramBridge::isAllowedCommand(u"/exec rm -rf /"_s));
        QVERIFY(!TelegramBridge::isAllowedCommand(u"/shell ls"_s));
    }

    void hardenedSettingsForceSafeDefaults()
    {
        Settings s;
        s.permissionMode = PermissionMode::AlwaysApprove;
        s.sandbox = SandboxProfile::Off;
        s.allowReadsOutsideWorkspace = true;
        s.autoApproveTools = QStringList{u"bash"_s};
        s.telegramEnabled = true;
        s.telegramAllowWrites = false;

        const Settings hardened = TelegramBridge::hardenedSettingsForRemote(s);
        QCOMPARE(hardened.permissionMode, PermissionMode::Ask);
        QVERIFY(!hardened.allowReadsOutsideWorkspace);
        QVERIFY(hardened.autoApproveTools.isEmpty());
        QVERIFY(hardened.planMode);
        QCOMPARE(hardened.sandbox, SandboxProfile::Strict);

        Settings writable = s;
        writable.telegramAllowWrites = true;
        const Settings hardenedWritable = TelegramBridge::hardenedSettingsForRemote(writable);
        QCOMPARE(hardenedWritable.permissionMode, PermissionMode::Ask);
        QCOMPARE(hardenedWritable.sandbox, SandboxProfile::Workspace);
        QVERIFY(!hardenedWritable.planMode);
    }
};

QTEST_GUILESS_MAIN(TestTelegramBridge)
#include "test_telegrambridge.moc"
