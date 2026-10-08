#include "acpclient.h"
#include "documentbridge.h"
#include "types.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestAcpClient : public QObject
{
    Q_OBJECT

private:
    static QString mockAgentPath()
    {
        QDir dir(QDir::current());
        for (int i = 0; i < 5; ++i) {
            const QString candidate = dir.absoluteFilePath(u"tests/mock_acp_agent.py"_s);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
            dir.cdUp();
        }
        return {};
    }

    template<typename Predicate>
    static bool waitFor(Predicate predicate, int timeoutMs = 15000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate()) {
            if (timer.elapsed() > timeoutMs) {
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QTest::qWait(10);
        }
        return true;
    }

    Settings mockSettings(const QString &scenario) const
    {
        Settings settings;
        settings.provider = Provider::Acp;
        settings.apiFormat = ApiFormat::AcpNative;
        settings.acpAgentId = u"custom"_s;
        settings.acpCommand = u"python3"_s;
        settings.acpArgs = u'"' + m_agent + u"\" "_s + scenario;
        settings.permissionMode = PermissionMode::Ask;
        settings.sandbox = SandboxProfile::Off;
        return settings;
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_agent = mockAgentPath();
        QVERIFY2(!m_agent.isEmpty(), "could not locate tests/mock_acp_agent.py");
    }

    void resolvedCommandFindsGrokFallbacksAndFlags()
    {
        Settings settings;
        settings.acpAgentId = u"grok-build"_s;
        settings.acpCommand = u"grok"_s;
        settings.acpArgs = u"agent stdio"_s;
        settings.acpModel = u"grok-4.6"_s;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        const QStringList args = AcpClient::agentArguments(settings);
        QVERIFY(args.contains(u"--model"_s));
        QVERIFY(args.contains(u"grok-4.6"_s));
        QVERIFY(args.contains(u"--always-approve"_s));
        QCOMPARE(args.last(), u"stdio"_s);
        QVERIFY(acpAgentIsGrok(settings));
        QVERIFY(usesAcpNative(settings) == false);
        settings.provider = Provider::Acp;
        QVERIFY(usesAcpNative(settings));
        QVERIFY(!providerRequiresApiKey(settings, Provider::Acp));
        QVERIFY(providerIsSelectable(settings, Provider::Acp));
    }

    void genericAgentsKeepTheirOwnArguments()
    {
        Settings settings;
        settings.provider = Provider::Acp;
        settings.apiFormat = ApiFormat::AcpNative;
        settings.acpAgentId = u"gemini"_s;
        settings.acpCommand = u"gemini"_s;
        settings.acpArgs = u"--acp"_s;
        settings.acpModel = u"gemini-2.5-pro"_s;
        settings.permissionMode = PermissionMode::AlwaysApprove;
        QCOMPARE(AcpClient::agentArguments(settings), QStringList{u"--acp"_s});
        QVERIFY(!acpAgentIsGrok(settings));

        settings.acpAgentId = u"custom"_s;
        settings.acpCommand = u"python3"_s;
        settings.acpArgs = u"/tmp/mock_acp_agent.py echo"_s;
        QCOMPARE(AcpClient::agentArguments(settings),
                 (QStringList{u"/tmp/mock_acp_agent.py"_s, u"echo"_s}));

        settings.acpCommand.clear();
        settings.acpArgs.clear();
        settings.acpAgentId = u"custom"_s;
        QVERIFY(AcpClient::resolvedCommand(settings).isEmpty());
        QVERIFY(AcpClient::agentArguments(settings).isEmpty());

        settings.acpAgentId = u"claude-acp"_s;
        QCOMPARE(acpEffectiveCommand(settings), u"npx"_s);
        QCOMPARE(AcpClient::agentArguments(settings),
                 (QStringList{u"-y"_s, u"@agentclientprotocol/claude-agent-acp"_s}));
        QVERIFY(!acpAgentIsGrok(settings));
    }

    void echoStreamsThoughtToolsAndText()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"echo"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString text;
        QString thinking;
        QJsonArray plan;
        int toolsStarted = 0;
        int toolsFinished = 0;
        QString toolOutput;
        QString stopReason;
        QString finishedText;
        QString error;

        connect(&client, &AcpClient::textDelta, this, [&](const QString &delta) {
            text += delta;
        });
        connect(&client, &AcpClient::thinkingDelta, this, [&](const QString &delta) {
            thinking += delta;
        });
        connect(&client, &AcpClient::planUpdated, this, [&](const QJsonArray &next) {
            plan = next;
        });
        connect(&client, &AcpClient::toolStarted, this, [&](const PermissionRequest &) {
            ++toolsStarted;
        });
        connect(&client, &AcpClient::toolFinished, this, [&](const ToolResult &result) {
            ++toolsFinished;
            QVERIFY(result.ok);
            toolOutput = result.output;
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &full, const QJsonArray &) {
            stopReason = reason;
            finishedText = full;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"world"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(stopReason, u"end_turn"_s);
        QCOMPARE(thinking, u"thinking..."_s);
        QCOMPARE(text, u"hello world"_s);
        QCOMPARE(finishedText, u"hello world"_s);
        QCOMPARE(toolsStarted, 1);
        QCOMPARE(toolsFinished, 1);
        QCOMPARE(toolOutput, u"ok"_s);
        QCOMPARE(plan.size(), 1);
        QCOMPARE(client.sessionId(), u"sess_kateai_test"_s);
        client.stop();
    }

    void permissionRoundTripSelectsAllowOnce()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"permission"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString text;
        QString error;
        QString stopReason;
        bool asked = false;

        connect(&client, &AcpClient::permissionNeeded, this, [&](const PermissionRequest &request) {
            asked = true;
            QCOMPARE(request.toolCallId, u"call_perm"_s);
            client.resolvePermission(PermissionDecision::AllowOnce);
        });
        connect(&client, &AcpClient::textDelta, this, [&](const QString &delta) {
            text += delta;
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"please write"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(asked);
        QCOMPARE(text, u"permission:allow-once"_s);
        client.stop();
    }

    void fsReadAndWriteGoThroughDocumentBridge()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString readPath = workspace.filePath(u"in.txt"_s);
        const QString writePath = workspace.filePath(u"out.txt"_s);
        {
            QFile file(readPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream(&file) << u"payload"_s;
        }

        DiskDocumentBridge bridge;
        AcpClient client;
        Settings settings = mockSettings(u"fs"_s);
        settings.sandbox = SandboxProfile::Workspace;
        client.setSettings(settings);
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString stopReason;
        QString error;
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"READPATH:%1 WRITEPATH:%2"_s.arg(readPath, writePath));
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));

        QString written;
        QVERIFY(bridge.readDocument(writePath, &written));
        QCOMPARE(written, u"from-agent:payload"_s);
        client.stop();
    }

    void terminalCreateWaitAndOutput()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"terminal"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString text;
        QString stopReason;
        QString error;
        connect(&client, &AcpClient::textDelta, this, [&](const QString &delta) {
            text += delta;
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"run"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }, 20000), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(text.contains(u"hello-term"_s));
        client.stop();
    }

    void terminalShellLineKeepsInnerQuotes()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"quoted"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString text;
        QString stopReason;
        QString error;
        connect(&client, &AcpClient::textDelta, this, [&](const QString &delta) {
            text += delta;
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"run"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }, 20000), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(!text.contains(u"unexpected EOF"_s), qPrintable(text));
        QVERIFY2(text.contains(u"quoted-ok"_s), qPrintable(text));
        client.stop();
    }

    void cancelReturnsCancelledStopReason()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"cancel"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString stopReason;
        QString error;
        connect(&client, &AcpClient::textDelta, this, [&](const QString &) {
            QTimer::singleShot(20, &client, &AcpClient::cancel);
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"hang"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(stopReason, u"cancelled"_s);
        client.stop();
    }

private:
    QString m_agent;
};

QTEST_MAIN(TestAcpClient)
#include "test_acpclient.moc"
