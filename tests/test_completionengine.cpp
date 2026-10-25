/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "completionengine.h"
#include "llmclient.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

/**
 * The requestable half of the in-editor completions: prompt building,
 * staleness, the per-minute budget, and the response shapes the engine
 * accepts (plain JSON, SSE that ignored stream:false, empty completions).
 *
 * The HTTP cases run against an in-process mock server on the loopback
 * interface, so no provider is contacted and no python mock is needed.
 */
class TestCompletionEngine : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void buildContextBoundsAndSnapping();
    void buildContextCursorAtStart();
    void buildContextTinyDocument();
    void buildMessagesShape();
    void stopSequencesAreMeaningful();
    void stalenessByPosition();
    void fimPromptMarkers();
    void completedFromPlainJson();
    void completedFromSseStream();
    void emptyCompletionFails();
    void budgetWindowEnforced();
    void cancelDropsTheResponse();
    void agentBusyIsAHardSwitch();

private:
    Settings engineSettings() const;

    QTcpServer m_server;
    QByteArray m_responseBody;
    QByteArray m_requestBuffer;
    int m_delayMs = 0;
    int m_requestCount = 0;
    QList<QTcpSocket *> m_sockets;
};

void TestCompletionEngine::init()
{
    m_responseBody = QByteArrayLiteral("{\"choices\":[{\"message\":{\"content\":\"world()\"}}]}");
    m_delayMs = 0;
    m_requestCount = 0;
    m_requestBuffer.clear();
    if (!m_server.isListening()) {
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                m_sockets.append(socket);
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    m_requestBuffer.append(socket->readAll());
                    const int headerEnd = m_requestBuffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0) {
                        return;
                    }
                    // Wait for the whole POST body before answering.
                    const int lengthStart = m_requestBuffer.lastIndexOf("Content-Length:");
                    const int lengthEnd = m_requestBuffer.indexOf("\r\n", lengthStart);
                    const int contentLength = lengthStart >= 0 && lengthEnd > lengthStart
                        ? m_requestBuffer.mid(lengthStart + 15, lengthEnd - lengthStart - 15).trimmed().toInt()
                        : 0;
                    if (m_requestBuffer.size() < headerEnd + 4 + contentLength) {
                        return;
                    }
                    ++m_requestCount;
                    const QByteArray response =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json"
                        "\r\nContent-Length: " + QByteArray::number(m_responseBody.size())
                        + "\r\nConnection: keep-alive\r\n\r\n" + m_responseBody;
                    m_requestBuffer.clear();
                    const auto write = [socket, response]() {
                        socket->write(response);
                        socket->flush();
                    };
                    if (m_delayMs > 0) {
                        QTimer::singleShot(m_delayMs, socket, write);
                    } else {
                        write();
                    }
                });
            }
        });
    }
    qRegisterMetaType<KateAi::CompletionPromptContext>("KateAi::CompletionPromptContext");
}

void TestCompletionEngine::cleanup()
{
    m_sockets.clear();
}

Settings TestCompletionEngine::engineSettings() const
{
    Settings settings;
    settings.provider = Provider::OpenAICompatible;
    settings.openaiCompatibleApiKey = u"test-key"_s;
    settings.openaiCompatibleModel = u"test-model"_s;
    settings.openaiCompatibleUrl =
        u"http://127.0.0.1:%1/v1"_s.arg(m_server.serverPort());
    settings.completionRequestsPerMinute = 60;
    return settings;
}

void TestCompletionEngine::buildContextBoundsAndSnapping()
{
    // Ten lines of twenty characters each; the cursor sits mid-line 4.
    QString text;
    for (int i = 0; i < 10; ++i) {
        text += QString(19, QChar(u'a' + i)) + u'\n';
    }
    const auto context = CompletionEngine::buildContext(text, 4, 10, u"/tmp/f.cpp"_s, u"C++"_s, 40, 40);
    QCOMPARE(context.cursorLine, 4);
    QCOMPARE(context.cursorColumn, 10);
    QCOMPARE(context.path, u"/tmp/f.cpp"_s);
    QCOMPARE(context.language, u"C++"_s);
    // The prefix never exceeds the budget: the raw cut is snapped forward to
    // the next line start, so it can only shrink.
    QVERIFY(context.prefix.size() <= 40);
    QVERIFY(text.contains(context.prefix));
    QCOMPARE(context.prefix.left(1), QString(1, QChar(u'd')));
    // Same at the end: the suffix runs to the start of the line after the one
    // the budget landed in.
    QVERIFY(context.suffix.size() >= 20);
    QVERIFY(text.contains(context.suffix));
}

void TestCompletionEngine::buildContextCursorAtStart()
{
    const auto context = CompletionEngine::buildContext(u"int main() {}\n"_s, 0, 0, QString(), u"C++"_s, 100, 100);
    QCOMPARE(context.prefix, QString());
    QCOMPARE(context.suffix, u"int main() {}\n"_s);
}

void TestCompletionEngine::buildContextTinyDocument()
{
    const auto context = CompletionEngine::buildContext(u"x"_s, 0, 1, QString(), QString(), 100, 100);
    QCOMPARE(context.prefix, u"x"_s);
    QCOMPARE(context.suffix, QString());
}

void TestCompletionEngine::buildMessagesShape()
{
    CompletionPromptContext context;
    context.language = u"Python"_s;
    context.path = u"/tmp/a.py"_s;
    context.prefix = u"def add(a, b):\n    return "_s;
    context.suffix = u"\n\n# more\n"_s;
    const QList<ChatMessage> messages = CompletionEngine::buildMessages(context);
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.first().role, ChatMessage::Role::System);
    QVERIFY(messages.first().content.contains(u"only the completion"_s, Qt::CaseInsensitive));
    QCOMPARE(messages.last().role, ChatMessage::Role::User);
    const QString &prompt = messages.last().content;
    QVERIFY(prompt.contains(u"Language: Python"_s));
    QVERIFY(prompt.contains(u"File: /tmp/a.py"_s));
    QVERIFY(prompt.contains(context.prefix));
    QVERIFY(prompt.contains(context.suffix));
    QVERIFY(prompt.contains(u"<CURSOR>"_s));
}

void TestCompletionEngine::stopSequencesAreMeaningful()
{
    const QStringList stops = CompletionEngine::stopSequences();
    QVERIFY(!stops.isEmpty());
    QVERIFY(!stops.contains(QString()));
}

void TestCompletionEngine::stalenessByPosition()
{
    CompletionPromptContext request;
    request.cursorLine = 3;
    request.cursorColumn = 7;
    QVERIFY(!CompletionEngine::isStale(request, 3, 7));
    QVERIFY(CompletionEngine::isStale(request, 4, 7));
    QVERIFY(CompletionEngine::isStale(request, 3, 8));
}

void TestCompletionEngine::fimPromptMarkers()
{
    CompletionPromptContext context;
    context.prefix = u"pre"_s;
    context.suffix = u"suf"_s;
    const QString deepseek = CompletionEngine::fimPrompt(context, Provider::DeepSeek);
    QVERIFY(deepseek.contains(u"<｜fim▁begin｜>"_s));
    QVERIFY(deepseek.contains(u"<｜fim▁end｜>"_s));
    QVERIFY(deepseek.contains(context.prefix));
    QVERIFY(deepseek.contains(context.suffix));
    const QString generic = CompletionEngine::fimPrompt(context, Provider::OpenAICompatible);
    QVERIFY(generic.contains(u"<|fim_prefix|>"_s));
    QVERIFY(generic.contains(u"<|fim_middle|>"_s));
}

void TestCompletionEngine::completedFromPlainJson()
{
    CompletionEngine engine;
    engine.setSettings(engineSettings());
    CompletionPromptContext context;
    context.prefix = u"hello "_s;
    QSignalSpy completed(&engine, &CompletionEngine::completed);
    engine.request(context);
    QTRY_COMPARE(completed.count(), 1);
    const auto arguments = completed.takeFirst();
    QCOMPARE(arguments.at(0).toString(), u"world()"_s);
    // The context travels back so the caller can drop stale responses.
    const auto returned = arguments.at(1).value<CompletionPromptContext>();
    QCOMPARE(returned.prefix, context.prefix);
}

void TestCompletionEngine::completedFromSseStream()
{
    m_responseBody = QByteArrayLiteral("data: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n"
                                       "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
                                       "data: [DONE]\n\n");
    CompletionEngine engine;
    engine.setSettings(engineSettings());
    QSignalSpy completed(&engine, &CompletionEngine::completed);
    engine.request(CompletionPromptContext{});
    QTRY_COMPARE(completed.count(), 1);
    QCOMPARE(completed.takeFirst().at(0).toString(), u"hello"_s);
}

void TestCompletionEngine::emptyCompletionFails()
{
    m_responseBody = QByteArrayLiteral("{\"choices\":[{\"message\":{\"content\":\"\"}}]}");
    CompletionEngine engine;
    engine.setSettings(engineSettings());
    QSignalSpy completed(&engine, &CompletionEngine::completed);
    QSignalSpy failed(&engine, &CompletionEngine::failed);
    engine.request(CompletionPromptContext{});
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(completed.count(), 0);
}

void TestCompletionEngine::budgetWindowEnforced()
{
    Settings settings = engineSettings();
    settings.completionRequestsPerMinute = 2;
    CompletionEngine engine;
    engine.setSettings(settings);
    QVERIFY(engine.canRequestNow());
    QSignalSpy completed(&engine, &CompletionEngine::completed);
    engine.request(CompletionPromptContext{});
    QTRY_COMPARE(completed.count(), 1);
    engine.request(CompletionPromptContext{});
    QTRY_COMPARE(completed.count(), 2);
    // The window is full: the engine refuses the third request until a minute
    // has passed, regardless of what the UI asks for.
    QVERIFY(!engine.canRequestNow());
    const int before = completed.count();
    engine.request(CompletionPromptContext{});
    QTest::qWait(100);
    QCOMPARE(completed.count(), before);
}

void TestCompletionEngine::cancelDropsTheResponse()
{
    m_delayMs = 200;
    CompletionEngine engine;
    engine.setSettings(engineSettings());
    QSignalSpy completed(&engine, &CompletionEngine::completed);
    QSignalSpy failed(&engine, &CompletionEngine::failed);
    engine.request(CompletionPromptContext{});
    QVERIFY(engine.isBusy());
    engine.cancel();
    QVERIFY(!engine.isBusy());
    QTest::qWait(400);
    QCOMPARE(completed.count(), 0);
    QCOMPARE(failed.count(), 0);
}

void TestCompletionEngine::agentBusyIsAHardSwitch()
{
    CompletionEngine engine;
    engine.setSettings(engineSettings());
    QVERIFY(engine.canRequestNow());
    engine.setAgentBusy(true);
    QVERIFY(!engine.canRequestNow());
    engine.setAgentBusy(false);
    QVERIFY(engine.canRequestNow());
}

QTEST_GUILESS_MAIN(TestCompletionEngine)
#include "test_completionengine.moc"
