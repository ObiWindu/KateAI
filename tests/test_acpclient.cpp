#include "acpclient.h"
#include "documentbridge.h"
#include "tools.h"
#include "types.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QStandardPaths>
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
        QCOMPARE(AcpClient::agentArguments(settings),
                 (QStringList{u"--model"_s, u"gemini-2.5-pro"_s, u"--acp"_s}));
        QVERIFY(!acpAgentIsGrok(settings));

        settings.acpAgentId = u"custom"_s;
        settings.acpCommand = u"python3"_s;
        settings.acpArgs = u"/tmp/mock_acp_agent.py echo"_s;
        QCOMPARE(AcpClient::agentArguments(settings),
                 (QStringList{u"/tmp/mock_acp_agent.py"_s, u"echo"_s}));

        settings.acpCommand.clear();
        settings.acpArgs.clear();
        settings.acpModel.clear();
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
        // The plan carries the ACP spec form (content-block arrays) and the
        // status spellings seen in the wild; every variant has to reach the
        // todo list as a readable step. The textless entry is skipped.
        QCOMPARE(plan.size(), 3);
        const QJsonObject first = plan.at(0).toObject();
        QCOMPARE(first.value(u"description"_s).toString(), u"Look around"_s);
        QVERIFY(first.value(u"completed"_s).toBool());
        QVERIFY(!first.value(u"inProgress"_s).toBool());
        const QJsonObject second = plan.at(1).toObject();
        QCOMPARE(second.value(u"description"_s).toString(), u"Fix the bug"_s);
        QVERIFY(second.value(u"inProgress"_s).toBool());
        QVERIFY(!second.value(u"completed"_s).toBool());
        const QJsonObject third = plan.at(2).toObject();
        QCOMPARE(third.value(u"description"_s).toString(), u"Tell the user"_s);
        QVERIFY(third.value(u"inProgress"_s).toBool());
        QVERIFY(!third.value(u"completed"_s).toBool());
        QCOMPARE(client.sessionId(), u"sess_kateai_test"_s);
        client.stop();
    }

    void toolStreamsPartialOutputBeforeItFinishes()
    {
        // An ACP agent reports a running tool through repeated in-progress
        // tool_call_update notifications. Dropping those meant the card stayed
        // blank for the whole command and only filled in when it ended, which is
        // exactly what the live stream is meant to avoid.
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"toolstream"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QStringList streamed;
        int toolsFinished = 0;
        QString toolOutput;
        QString error;
        QString stopReason;
        connect(&client, &AcpClient::toolProgress, this, [&](const QString &toolCallId, const QString &partial) {
            QCOMPARE(toolCallId, u"call_stream"_s);
            streamed << partial;
        });
        connect(&client, &AcpClient::toolFinished, this, [&](const ToolResult &result) {
            ++toolsFinished;
            toolOutput = result.output;
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"build"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(stopReason, u"end_turn"_s);

        // Both in-progress updates arrived, and each one carried more than the
        // last, so a card can grow rather than flicker.
        QCOMPARE(streamed.size(), 2);
        QVERIFY2(streamed.at(0).contains(u"scanning dependencies"_s), qPrintable(streamed.value(0)));
        QVERIFY2(streamed.at(1).contains(u"scanning dependencies"_s), qPrintable(streamed.value(1)));
        QVERIFY2(streamed.at(1).contains(u"building target"_s), qPrintable(streamed.value(1)));

        // The stream is not the result: the finished call still reports once.
        QCOMPARE(toolsFinished, 1);
        QVERIFY2(toolOutput.contains(u"build finished"_s), qPrintable(toolOutput));
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

    void refusingWithoutARejectOptionCancelsRatherThanApproving()
    {
        // The agent advertises only allow options, so there is no matching kind
        // for a Deny. The client must fall back to "cancelled" instead of
        // echoing the first option id, which would execute the denied tool.
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"permission-allowonly"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString text;
        QString error;
        QString stopReason;
        bool asked = false;

        connect(&client, &AcpClient::permissionNeeded, this, [&](const PermissionRequest &) {
            asked = true;
            client.resolvePermission(PermissionDecision::Deny);
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
        QCOMPARE(text, u"refusal:cancelled"_s);
        client.stop();
    }

    void pipelinedPermissionRequestsBothGetAnswered()
    {
        // An agent that asks twice before the first answer arrives must not have
        // the first question dropped: an unanswered session/request_permission
        // blocks that agent for the rest of the session. The client shows one bar
        // at a time, so it has to close the earlier question itself.
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"permission-double"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString text;
        QString error;
        QString stopReason;
        int asks = 0;

        connect(&client, &AcpClient::permissionNeeded, this, [&](const PermissionRequest &) {
            ++asks;
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
        // Let both questions arrive while nothing is answered, which is the
        // overlapping case. Then settle whichever one is still on screen.
        QVERIFY2(waitFor([&] { return asks >= 2; }, 10000),
                 "the second permission request never reached the client");
        client.resolvePermission(PermissionDecision::Deny);

        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(asks, 2);
        // The earlier question is closed by the client as cancelled; the later
        // one carries the user's decision.
        QCOMPARE(text, u"permissions:a=cancelled;b=reject-once"_s);
        client.stop();
    }

    void stringIdsInRepliesStillFinishTheTurn()
    {
        // session/prompt carries no timeout, so a reply that quotes the request
        // id as a string used to be dropped and the turn never ended at all.
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"stringid"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString text;
        QString error;
        QString stopReason;
        connect(&client, &AcpClient::textDelta, this, [&](const QString &delta) {
            text += delta;
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"hello"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), "the turn never finished; the string id was dropped");
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(stopReason, u"end_turn"_s);
        QCOMPARE(text, u"string-id-ok"_s);
        client.stop();
    }

    void approvingWithoutAnAllowOnceOptionStillAllows()
    {
        // An agent that advertises only allow_always/reject_always leaves no
        // allow_once for a plain Allow. Falling back to "cancelled" would drop a
        // tool the user approved; the safest option that still points the same
        // way is the allow option the agent did advertise.
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"permission-alwaysonly"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString text;
        QString error;
        QString stopReason;
        bool asked = false;

        connect(&client, &AcpClient::permissionNeeded, this, [&](const PermissionRequest &) {
            asked = true;
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
        QCOMPARE(text, u"approval:allow-always"_s);
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

    void writePlanFileClientMethodFilesThePlanOutsideTheWorkspace()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"plan"_s));
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

        client.prompt(u"plan this"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(stopReason, u"end_turn"_s);

        // The agent is told where the plan landed, and the file itself lives
        // in the temp directory rather than in the user's project.
        const QString planPath = planFilePath(workspace.path());
        QCOMPARE(text, u"plan:"_s + planPath);
        QString written;
        QVERIFY(bridge.readDocument(planPath, &written));
        QCOMPARE(written, u"# Plan\n\n1. Inspect the code\n2. Change it\n"_s);
        QCOMPARE(QDir(workspace.path()).entryList(QDir::Files), QStringList());
        client.stop();
    }

    void suggestClientMethodPostsAdviceTouchingNothing()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"suggest"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QList<Suggestion> posted;
        QString stopReason;
        QString error;
        connect(&client, &AcpClient::suggestionPosted, this, [&](const Suggestion &suggestion) {
            posted.append(suggestion);
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"suggest something"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(stopReason, u"end_turn"_s);

        QCOMPARE(posted.size(), 1);
        const Suggestion suggestion = posted.first();
        QVERIFY(suggestion.text.contains(u"std::string_view"_s));
        QCOMPARE(suggestion.path, u"src/main.cpp"_s);
        QCOMPARE(suggestion.line, 7);

        // Advice only: the workspace is untouched, exactly as with the native
        // tool, so the card is safe to show without a checkpoint.
        QCOMPARE(QDir(workspace.path()).entryList(QDir::Files), QStringList());
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

    void sessionConfigOptionsLoadModelsLikeZed()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        AcpClient client;
        Settings settings = mockSettings(u"models"_s);
        settings.acpModel = u"grok-4"_s;
        client.setSettings(settings);
        client.setWorkspace(workspace.path());

        QStringList models;
        QString error;
        connect(&client, &AcpClient::modelsReceived, this, [&](const QStringList &next) {
            models = next;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.fetchModels();
        QVERIFY2(waitFor([&] {
            return error.isEmpty() == false || (models.contains(u"grok-4.7"_s) && client.currentModelId() == u"grok-4"_s);
        }), qPrintable(error.isEmpty() ? u"timed out waiting for config option models"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(models, (QStringList{u"grok-4"_s, u"grok-4.5"_s, u"grok-4.7"_s}));
        QVERIFY(!models.contains(u"code"_s));
        QVERIFY(!models.contains(u"stale-model"_s));
        QCOMPARE(client.currentModelId(), u"grok-4"_s);
        client.stop();
    }

    // The config page's Fetch Models flow owns no workspace (the standard Kate
    // config page is created without one), yet agents reject a non-absolute
    // cwd -- opencode answers "cwd must be an absolute path". The client must
    // fall back to the process working directory so the models still load.
    void modelsLoadWithoutAWorkspace()
    {
        AcpClient client;
        client.setSettings(mockSettings(u"models"_s));

        QStringList models;
        QString error;
        connect(&client, &AcpClient::modelsReceived, this, [&](const QStringList &next) {
            models = next;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.fetchModels();
        QVERIFY2(waitFor([&] {
            return !models.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(models.contains(u"grok-4.5"_s));
        client.stop();
    }

    // opencode's installer puts the binary in ~/.opencode/bin, which is only
    // added to the *shell* PATH; a Kate launched from the desktop cannot find
    // a bare "opencode" on PATH, so the fallback has to look there.
    void resolvedCommandFallsBackToTheOpencodeInstallDir()
    {
        Settings settings;
        settings.acpAgentId = u"opencode"_s;
        settings.acpCommand = u"opencode"_s;
        settings.acpArgs = u"acp"_s;

        QString expected = QStandardPaths::findExecutable(u"opencode"_s);
        if (expected.isEmpty()) {
            const QFileInfo ownInstall(QDir::homePath() + u"/.opencode/bin/opencode"_s);
            if (ownInstall.exists() && ownInstall.isExecutable()) {
                expected = ownInstall.absoluteFilePath();
            }
        }
        if (expected.isEmpty()) {
            const QFileInfo localBin(QDir::homePath() + u"/.local/bin/opencode"_s);
            if (localBin.exists() && localBin.isExecutable()) {
                expected = localBin.absoluteFilePath();
            }
        }
        if (expected.isEmpty()) {
            expected = u"opencode"_s;
        }
        QCOMPARE(AcpClient::resolvedCommand(settings), expected);
    }

    void legacySessionModelsUseSetModel()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        AcpClient client;
        client.setSettings(mockSettings(u"legacy-models"_s));
        client.setWorkspace(workspace.path());

        QStringList models;
        QString error;
        connect(&client, &AcpClient::modelsReceived, this, [&](const QStringList &next) {
            models = next;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.fetchModels();
        QVERIFY2(waitFor([&] {
            return !models.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(models, (QStringList{u"gemini-2.5-pro"_s, u"gemini-2.5-flash"_s}));
        QCOMPARE(client.currentModelId(), u"gemini-2.5-pro"_s);

        client.setModel(u"gemini-2.5-flash"_s);
        QVERIFY2(waitFor([&] {
            return client.currentModelId() == u"gemini-2.5-flash"_s || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out setting legacy model"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(client.currentModelId(), u"gemini-2.5-flash"_s);
        client.stop();
    }

private:
    QString m_agent;
};

QTEST_MAIN(TestAcpClient)
#include "test_acpclient.moc"
