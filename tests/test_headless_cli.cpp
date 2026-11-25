/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Spawns the real kateai-headless binary against an in-process mock provider:
 * the run must write a file, emit JSONL events, produce a summary and exit 0;
 * a non-interactive permission request must exit 4; replay must read the same
 * event log back without a model.
 */

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxyFactory>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Qt::Literals::StringLiterals;

namespace
{

class MockProvider : public QObject
{
    Q_OBJECT
public:
    explicit MockProvider(QObject *parent = nullptr)
        : QObject(parent)
        , m_server(new QTcpServer(this))
    {
        connect(m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = m_server->nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            auto buffer = std::make_shared<QByteArray>();
            auto replied = std::make_shared<bool>(false);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, replied] {
                if (*replied) {
                    return;
                }
                buffer->append(socket->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                int contentLength = 0;
                for (const QByteArray &line : buffer->left(headerEnd).split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        contentLength = line.mid(15).trimmed().toInt();
                        break;
                    }
                }
                if (buffer->size() < headerEnd + 4 + contentLength) {
                    return;
                }
                *replied = true;
                handle(socket);
            });
        });
    }

    bool listen()
    {
        return m_server->listen(QHostAddress::LocalHost);
    }
    int port() const
    {
        return m_server->serverPort();
    }

private:
    void handle(QTcpSocket *socket)
    {
        ++m_requests;
        QByteArray body;
        if (m_requests == 1) {
            QJsonObject function;
            function.insert(u"name"_s, u"write_file"_s);
            function.insert(u"arguments"_s,
                            QString::fromUtf8(QJsonDocument(
                                                  QJsonObject{{u"path"_s, u"note.txt"_s},
                                                              {u"content"_s,
                                                               u"hello headless"_s}})
                                                  .toJson(QJsonDocument::Compact)));
            QJsonObject call;
            call.insert(u"index"_s, 0);
            call.insert(u"id"_s, u"call-1"_s);
            call.insert(u"type"_s, u"function"_s);
            call.insert(u"function"_s, function);
            QJsonObject chunk;
            chunk.insert(u"choices"_s,
                         QJsonArray{QJsonObject{{u"index"_s, 0},
                                                {u"delta"_s,
                                                 QJsonObject{{u"tool_calls"_s,
                                                              QJsonArray{call}}}},
                                                {u"finish_reason"_s, QJsonValue::Null}}});
            body += "data: " + QJsonDocument(chunk).toJson(QJsonDocument::Compact) + "\n\n";
            body += "data: "
                + QJsonDocument(QJsonObject{{u"choices"_s,
                                             QJsonArray{QJsonObject{
                                                 {u"index"_s, 0},
                                                 {u"delta"_s, QJsonObject()},
                                                 {u"finish_reason"_s, u"tool_calls"_s}}}}})
                      .toJson(QJsonDocument::Compact)
                + "\n\n";
        } else {
            body += "data: "
                + QJsonDocument(QJsonObject{{u"choices"_s,
                                             QJsonArray{QJsonObject{
                                                 {u"index"_s, 0},
                                                 {u"delta"_s,
                                                  QJsonObject{{u"content"_s, u"Done."_s}}},
                                                 {u"finish_reason"_s, u"stop"_s}}}}})
                      .toJson(QJsonDocument::Compact)
                + "\n\n";
        }
        body += "data: [DONE]\n\n";
        send(socket, body);
    }

    void send(QTcpSocket *socket, const QByteArray &body)
    {
        const QByteArray head = "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/event-stream\r\n"
                                "Connection: close\r\n"
                                "Content-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n";
        socket->write(head);
        socket->write(body);
        socket->flush();
        socket->disconnectFromHost();
    }

    QTcpServer *m_server = nullptr;
    int m_requests = 0;
};

struct RunResult {
    int exitCode = -1;
    QString output;
    QString error;
};

} // namespace

class TestHeadlessCli : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    RunResult run(const QStringList &arguments, const QHash<QString, QString> &env = {})
    {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"HOME"_s, m_dir.filePath(u"home"_s));
        environment.insert(u"XDG_CONFIG_HOME"_s, m_dir.filePath(u"config"_s));
        environment.insert(u"XDG_DATA_HOME"_s, m_dir.filePath(u"data"_s));
        environment.insert(u"KATEAI_SESSION_DB"_s, m_dir.filePath(u"sessions.sqlite"_s));
        environment.insert(u"KATEAI_HARNESS_FLAGS"_s, u"+eventLog"_s);
        for (auto it = env.constBegin(); it != env.constEnd(); ++it) {
            environment.insert(it.key(), it.value());
        }
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(QStringLiteral(KATEAI_HEADLESS_BIN), arguments);
        if (!process.waitForStarted(10000)) {
            return {.exitCode = -2, .output = QString(), .error = process.errorString()};
        }
        QElapsedTimer timer;
        timer.start();
        while (process.state() != QProcess::NotRunning) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (timer.elapsed() > 60000) {
                process.kill();
                process.waitForFinished(2000);
                return {.exitCode = -3,
                        .output = QString::fromUtf8(process.readAllStandardOutput()),
                        .error = QString::fromUtf8(process.readAllStandardError())
                            + u" (timeout)"_s};
            }
        }
        return {.exitCode = process.exitCode(),
                .output = QString::fromUtf8(process.readAllStandardOutput()),
                .error = QString::fromUtf8(process.readAllStandardError())};
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(m_dir.filePath(u"home"_s)));
        QVERIFY(QDir().mkpath(m_dir.filePath(u"workspace"_s)));
        QNetworkProxyFactory::setUseSystemConfiguration(false);
    }

    void helpAndVersion()
    {
        RunResult help = run({u"--help"_s});
        QCOMPARE(help.exitCode, 0);
        QVERIFY(help.output.contains(u"Usage: kateai-headless"_s));

        RunResult version = run({u"--version"_s});
        QCOMPARE(version.exitCode, 0);
        QVERIFY(version.output.contains(u"kateai-headless"_s));
    }

    void writesFileAndArtifacts()
    {
        MockProvider provider;
        QVERIFY(provider.listen());
        const QString workspace = m_dir.filePath(u"workspace"_s);

        RunResult result = run({
            u"run"_s,
            u"--workspace"_s, workspace,
            u"--output-dir"_s, m_dir.filePath(u"out"_s),
            u"--prompt"_s, u"Write the note file."_s,
            u"--provider"_s, u"openai-compatible"_s,
            u"--base-url"_s, u"http://127.0.0.1:%1/v1"_s.arg(provider.port()),
            u"--model"_s, u"mock"_s,
            u"--api-key"_s, u"test"_s,
            u"--permission"_s, u"auto"_s,
            u"--sandbox"_s, u"off"_s,
            u"--no-verify"_s,
        });
        QVERIFY2(result.exitCode == 0,
                 qPrintable(result.error + u"\n" + result.output));
        QFile note(QDir(workspace).filePath(u"note.txt"_s));
        QVERIFY(note.open(QIODevice::ReadOnly));
        QCOMPARE(note.readAll(), QByteArray("hello headless"));

        QVERIFY(QFile::exists(m_dir.filePath(u"out/events.jsonl"_s)));
        QVERIFY(QFile::exists(m_dir.filePath(u"out/summary.json"_s)));
        QVERIFY(QFile::exists(m_dir.filePath(u"out/trace.jsonl"_s)));
        QFile trace(m_dir.filePath(u"out/trace.jsonl"_s));
        QVERIFY(trace.open(QIODevice::ReadOnly));
        QVERIFY(!trace.readAll().trimmed().isEmpty());
        QFile events(m_dir.filePath(u"out/events.jsonl"_s));
        QVERIFY(events.open(QIODevice::ReadOnly));
        const QByteArray contents = events.readAll();
        QVERIFY(contents.contains("\"type\":\"tool/call\""));
        QVERIFY(contents.contains("\"type\":\"turn/end\""));

        QFile summary(m_dir.filePath(u"out/summary.json"_s));
        QVERIFY(summary.open(QIODevice::ReadOnly));
        const QJsonObject object = QJsonDocument::fromJson(summary.readAll()).object();
        QVERIFY(object.value(u"ok"_s).toBool());
        QCOMPARE(object.value(u"exitCode"_s).toInt(), 0);
        QVERIFY(!object.value(u"conversationId"_s).toString().isEmpty());

        // Replay the same event log without a model.
        RunResult replay = run({u"replay"_s,
                                u"--conversation"_s,
                                object.value(u"conversationId"_s).toString()});
        QVERIFY2(replay.exitCode == 0 && replay.output.contains(u"\"ok\": true"_s),
                 qPrintable(replay.error + u"\n" + replay.output));
    }

    void permissionAskExitsFour()
    {
        MockProvider provider;
        QVERIFY(provider.listen());
        RunResult result = run({
            u"run"_s,
            u"--workspace"_s, m_dir.filePath(u"workspace"_s),
            u"--prompt"_s, u"Write the note file."_s,
            u"--provider"_s, u"openai-compatible"_s,
            u"--base-url"_s, u"http://127.0.0.1:%1/v1"_s.arg(provider.port()),
            u"--model"_s, u"mock"_s,
            u"--api-key"_s, u"test"_s,
            u"--permission"_s, u"ask"_s,
            u"--sandbox"_s, u"off"_s,
            u"--no-verify"_s,
            u"--no-persist"_s,
        });
        QCOMPARE(result.exitCode, 4);
    }
};

QTEST_GUILESS_MAIN(TestHeadlessCli)

#include "test_headless_cli.moc"
