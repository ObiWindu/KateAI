/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/contextengine.h"
#include "harness/tokens.h"
#include "harness/toolresults.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

ChatMessage message(ChatMessage::Role role, const QString &text)
{
    ChatMessage msg;
    msg.role = role;
    msg.content = text;
    return msg;
}

ChatMessage toolMessage(const QString &name, const QString &text)
{
    ChatMessage msg;
    msg.role = ChatMessage::Role::Tool;
    msg.name = name;
    msg.content = text;
    return msg;
}

} // namespace

class TestHarnessContext : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        ToolResults::setDirectoryForTests(m_dir->path());
    }

    void cleanup()
    {
        ToolResults::setDirectoryForTests(QString());
        m_dir.reset();
    }

    void shortHistoryIsUntouched()
    {
        const QList<ChatMessage> history{
            message(ChatMessage::Role::System, u"system"_s),
            message(ChatMessage::Role::User, u"hello"_s),
        };
        ContextOptions options;
        options.contextWindow = 128000;
        const ContextResult result = ContextEngine::build(history, options);
        QCOMPARE(result.messages.size(), history.size());
        QVERIFY(!result.compacted);
        QVERIFY(result.shapers.isEmpty());
    }

    void oversizedToolResultBecomesAReference()
    {
        const QString big = QString(60000, u'y');
        const QList<ChatMessage> history{
            message(ChatMessage::Role::System, u"system"_s),
            message(ChatMessage::Role::User, u"inspect"_s),
            toolMessage(u"bash"_s, big),
            message(ChatMessage::Role::User, u"what did it say?"_s),
        };
        ContextOptions options;
        options.contextWindow = 8000;
        options.reserveForResponse = 2000;
        options.keepRecentTokens = 1000;
        options.autoCompact = true;
        const ContextResult result = ContextEngine::build(history, options);
        QVERIFY(result.compacted);
        QVERIFY(result.shapers.contains(QJsonValue(u"tool-result-budget"_s)));
        bool foundReference = false;
        for (const ChatMessage &msg : result.messages) {
            if (msg.content.contains(u"read_tool_result"_s)) {
                foundReference = true;
            }
            if (msg.role == ChatMessage::Role::Tool) {
                QVERIFY(msg.content.size() < 60000);
            }
        }
        QVERIFY(foundReference);
    }

    void incrementalSummaryKeepsAnEarlyDecision()
    {
        QList<ChatMessage> history;
        history.append(message(ChatMessage::Role::System, u"system"_s));
        history.append(message(ChatMessage::Role::User,
                               u"Early decision: use a QHash for the cache, never a QMap."_s));
        for (int i = 0; i < 120; ++i) {
            history.append(message(ChatMessage::Role::User, u"step %1 padding %2"_s.arg(i).arg(QString(240, u'z'))));
            history.append(message(ChatMessage::Role::Assistant, u"acknowledged step %1"_s.arg(i)));
        }
        history.append(message(ChatMessage::Role::User, u"What did we decide about the cache?"_s));

        ContextOptions options;
        options.contextWindow = 12000;
        options.reserveForResponse = 2000;
        const ContextResult first = ContextEngine::build(history, options);
        QVERIFY(first.compacted);
        QVERIFY(!first.summary.isEmpty());
        QVERIFY(first.summary.contains(u"QHash"_s));

        history.append(message(ChatMessage::Role::User, u"More work after the first compaction."_s));
        history.append(message(ChatMessage::Role::Assistant, QString(8000, u'q')));
        options.previousSummary = first.summary;
        options.compactionLevel = 1;
        const ContextResult second = ContextEngine::build(history, options);
        QVERIFY(second.summary.contains(u"QHash"_s));
        QVERIFY(second.summary.contains(u"Update after further compaction"_s));
    }

    void rehydrationTravelsAsATransientMessage()
    {
        QList<ChatMessage> history;
        history.append(message(ChatMessage::Role::System, u"system"_s));
        history.append(message(ChatMessage::Role::User, QString(30000, u'a')));
        history.append(message(ChatMessage::Role::Assistant, QString(30000, u'b')));
        history.append(message(ChatMessage::Role::User, u"continue"_s));
        ContextOptions options;
        options.contextWindow = 12000;
        options.reserveForResponse = 2000;
        options.plan = u"- [ ] fix the parser"_s;
        options.modifiedFiles = {u"src/a.cpp"_s, u"src/b.cpp"_s};
        options.runningTasks = {u"reviewer is checking the diff"_s};
        const ContextResult result = ContextEngine::build(history, options);
        bool found = false;
        for (const ChatMessage &msg : result.messages) {
            if (msg.name == u"context_rehydration"_s) {
                found = true;
                QVERIFY(msg.transient);
                QVERIFY(msg.content.contains(u"fix the parser"_s));
                QVERIFY(msg.content.contains(u"src/a.cpp"_s));
                QVERIFY(msg.content.contains(u"reviewer"_s));
            }
        }
        QVERIFY(found);
    }

    void calibratorMovesButIsClamped()
    {
        Tokens::Calibrator calibrator;
        QCOMPARE(calibrator.factor(), 1.0);
        calibrator.observe(1000, 1500);
        QVERIFY(calibrator.factor() > 1.0);
        QCOMPARE(calibrator.samples(), 1);
        for (int i = 0; i < 100; ++i) {
            calibrator.observe(1000, 100000);
        }
        QCOMPARE(calibrator.factor(), Tokens::kMaxFactor);
        for (int i = 0; i < 100; ++i) {
            calibrator.observe(1000, 1);
        }
        QCOMPARE(calibrator.factor(), Tokens::kMinFactor);
    }

    void contentStorePrunesOldestFirst()
    {
        QStringList ids;
        for (int i = 0; i < 5; ++i) {
            const StoredToolResult stored = ToolResults::store(QString(20000, QChar(u'a' + i)));
            QVERIFY(!stored.id.isEmpty());
            ids.append(stored.id);
        }
        const int removed = ToolResults::prune(45000);
        QVERIFY(removed > 0);
        qint64 total = 0;
        const QDir dir(m_dir->path());
        for (const QFileInfo &info : dir.entryInfoList({QStringLiteral("*.txt")}, QDir::Files)) {
            total += info.size();
        }
        QVERIFY(total <= 45000);
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
};

QTEST_GUILESS_MAIN(TestHarnessContext)
#include "test_harness_context.moc"
