/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "agentloop.h"
#include "graph/projectgraph.h"
#include "harness/toolresults.h"
#include "sessionstore.h"
#include "workspaceservices.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{
qint64 rssBytes()
{
#ifdef Q_OS_LINUX
    QFile file(u"/proc/self/statm"_s);
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QList<QByteArray> parts = file.readAll().split(' ');
    if (parts.size() < 2) {
        return -1;
    }
    bool ok = false;
    const qint64 pages = parts.at(1).toLongLong(&ok);
    if (!ok || pages < 0) {
        return -1;
    }
    return pages * static_cast<qint64>(sysconf(_SC_PAGESIZE));
#else
    return -1;
#endif
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size();
}

class TestMemoryBudget : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void restoreLargeSessionStaysBounded()
    {
        if (rssBytes() < 0) {
            QSKIP("RSS measurement is only available on Linux");
        }

        SessionStore::SessionData data;
        data.messages.reserve(2000);
        for (int i = 0; i < 2000; ++i) {
            ChatMessage message;
            message.role = ChatMessage::Role::User;
            message.content = QString(4000, u'x');
            data.messages.append(message);
        }

        const qint64 before = rssBytes();
        AgentLoop agent;
        agent.restoreSession(data);
        const qint64 after = rssBytes();
        const qint64 delta = after - before;
        qInfo() << "restoreLargeSessionStaysBounded delta" << delta;
        // The model history is retained and the transcript is pruned. Allow
        // some allocator slack; this should be far below a full second copy.
        QVERIFY2(delta <= 40LL * 1024 * 1024,
                 qPrintable(QStringLiteral("restore delta %1 bytes exceeds 40 MB").arg(delta)));
    }

    void reasoningIsNotRetainedForEveryTurn()
    {
        // Every turn's reasoning used to stay in the model history for the rest
        // of the conversation, so a reasoning-heavy model doubled or tripled the
        // request, the session payload and every copy made from them. Only the
        // newest turns keep theirs.
        SessionStore::SessionData data;
        constexpr int kTurns = 6;
        data.messages.reserve(kTurns * 2);
        for (int i = 0; i < kTurns; ++i) {
            ChatMessage user;
            user.role = ChatMessage::Role::User;
            user.content = u"turn %1"_s.arg(i);
            data.messages.append(user);

            ChatMessage assistant;
            assistant.role = ChatMessage::Role::Assistant;
            assistant.content = u"answer %1"_s.arg(i);
            assistant.thinking = QString(4000, u't');
            data.messages.append(assistant);
        }

        AgentLoop agent;
        agent.restoreSession(data);
        const QJsonObject counters = agent.memoryCounters();
        qInfo() << "reasoningIsNotRetainedForEveryTurn" << counters;

        // Restore keeps the whole session as the display transcript, but
        // memoryCounters reports what the model history still carries: only the
        // newest kThinkingRetainedTurns turns keep their reasoning.
        QVERIFY2(counters.value(u"thinkingChars"_s).toDouble() <= 2.0 * 4000.0,
                 qPrintable(u"retained %1 reasoning chars"_s.arg(
                     counters.value(u"thinkingChars"_s).toDouble())));
        QVERIFY2(agent.messages().last().thinking.size() == 4000,
                 "the newest turn lost its reasoning");
    }

    void restoredHistoryStaysBounded()
    {
        SessionStore::SessionData data;
        data.messages.reserve(2000);
        for (int i = 0; i < 2000; ++i) {
            ChatMessage message;
            message.role = ChatMessage::Role::User;
            message.content = QString(16000, u'x');
            data.messages.append(message);
        }

        AgentLoop agent;
        agent.restoreSession(data);
        const QJsonObject counters = agent.memoryCounters();
        qInfo() << "restoredHistoryStaysBounded" << counters;
        QVERIFY2(counters.value(u"messageChars"_s).toDouble() <= 16.0 * 1024 * 1024,
                 "the model history exceeded its safety ceiling");
        QVERIFY2(counters.value(u"messages"_s).toInt() < data.messages.size(),
                 "a restored oversized history was not trimmed");
        QVERIFY2(agent.messages().first().role == ChatMessage::Role::User,
                 "the trim broke the first retained turn boundary");
    }

    void writeHeavyTranscriptStaysBounded()
    {
        SessionStore::SessionData data;
        const QJsonArray toolCalls = [] {
            QString arguments = u"{\"content\":\""_s;
            arguments += QString(100 * 1024, u'x');
            arguments += u"\"}"_s;
            QJsonObject function;
            function.insert(u"name"_s, u"write_file"_s);
            function.insert(u"arguments"_s, arguments);
            QJsonObject call;
            call.insert(u"id"_s, u"call"_s);
            call.insert(u"type"_s, u"function"_s);
            call.insert(u"function"_s, function);
            return QJsonArray{call};
        }();
        data.messages.reserve(200);
        for (int i = 0; i < 200; ++i) {
            ChatMessage message;
            message.role = ChatMessage::Role::Assistant;
            message.content = u"writing"_s;
            message.toolCalls = toolCalls;
            data.messages.append(message);
        }

        AgentLoop agent;
        agent.restoreSession(data);
        const QJsonObject counters = agent.memoryCounters();
        qInfo() << "writeHeavyTranscriptStaysBounded" << counters;
        QVERIFY2(counters.value(u"transcriptMessages"_s).toInt() < data.messages.size(),
                 "the transcript budget ignored tool arguments");
        QVERIFY(counters.value(u"transcriptChars"_s).toDouble() <= 8.0 * 1024 * 1024
                || counters.value(u"transcriptMessages"_s).toInt() <= 8);
        QVERIFY2(counters.value(u"actionRepeatEntries"_s).toInt() == 0,
                 "restored action repeat state was not reset");
    }

    void workspaceServicesAreShared()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.filePath(u"main.cpp"_s),
                          QByteArrayLiteral("int main() { return 0; }\n")));

        WorkspaceServices services(dir.path());
        QVERIFY(services.graph());
        QVERIFY(services.mcpManager());
        QVERIFY(services.eventStore());
        QVERIFY(!services.mcpManager()->loadsWorkspaceConfig());
        QVERIFY(services.graph()->getNodeCount() > 0);

        AgentLoop first;
        AgentLoop second;
        // Both loops adopt the same objects; the second install must not
        // create a second graph or event connection.
        first.setHarnessServices(services.graph(), services.mcpManager(), services.eventStore());
        second.setHarnessServices(services.graph(), services.mcpManager(), services.eventStore());
        QCOMPARE(first.getProjectGraph(), services.graph().get());
        QCOMPARE(second.getProjectGraph(), services.graph().get());
        QCOMPARE(first.mcpManager(), services.mcpManager());
        QCOMPARE(second.mcpManager(), services.mcpManager());
    }

    void toolResultPageStaysBounded()
    {
        if (rssBytes() < 0) {
            QSKIP("RSS measurement is only available on Linux");
        }

        const QString text(8 * 1024 * 1024, u'x');
        const StoredToolResult stored = ToolResults::store(text, 4000, 2000);
        QVERIFY(!stored.id.isEmpty());

        const qint64 before = rssBytes();
        QString page;
        qint64 total = 0;
        QString error;
        QVERIFY2(ToolResults::read(stored.id, 100000, 20000, &page, &total, &error),
                 qPrintable(error));
        const qint64 after = rssBytes();
        const qint64 delta = after - before;
        qInfo() << "toolResultPageStaysBounded delta" << delta << "total" << total;
        QCOMPARE(page.size(), 20000);
        QVERIFY2(delta <= 16LL * 1024 * 1024,
                 qPrintable(QStringLiteral("page read delta %1 bytes exceeds 16 MB").arg(delta)));
    }

    void projectGraphScanStaysBounded()
    {
        if (rssBytes() < 0) {
            QSKIP("RSS measurement is only available on Linux");
        }

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray content(512, 'a');
        for (int i = 0; i < 3000; ++i) {
            QVERIFY(writeFile(dir.filePath(QStringLiteral("file%1.txt").arg(i)), content));
        }

        const qint64 before = rssBytes();
        ProjectGraph graph;
        graph.generateGraph(dir.path());
        const qint64 after = rssBytes();
        const qint64 delta = after - before;
        qInfo() << "projectGraphScanStaysBounded delta" << delta;
        QVERIFY(graph.getNodeCount() <= 1000);
        QVERIFY2(delta <= 25LL * 1024 * 1024,
                 qPrintable(QStringLiteral("graph scan delta %1 bytes exceeds 25 MB").arg(delta)));
    }
};

} // namespace

QTEST_GUILESS_MAIN(TestMemoryBudget)
#include "test_memory_budget.moc"
