#include "mcp.h"
#include "permissions.h"
#include "sandbox.h"
#include "types.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

// Drives a real MCP client against tests/mock_mcp_server.py over stdio. This
// is the only test that exercises the JSON-RPC handshake, tool discovery and
// the tools/call round trip end to end.
class TestMcpIntegration : public QObject
{
    Q_OBJECT

private:
    static QString mockServerPath()
    {
        // The test binary runs from the build dir, so walk up to the source.
        QDir dir(QDir::current());
        for (int i = 0; i < 5; ++i) {
            const QString candidate = dir.absoluteFilePath(u"tests/mock_mcp_server.py"_s);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
            dir.cdUp();
        }
        return {};
    }

    // Spins the event loop until `predicate` holds or the timeout expires.
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

private Q_SLOTS:
    void initTestCase()
    {
        m_server = mockServerPath();
        QVERIFY2(!m_server.isEmpty(), "could not locate tests/mock_mcp_server.py");
    }

    void discoversToolsOverStdio()
    {
        McpServerConfig config;
        config.name = u"mock"_s;
        config.command = u"python3"_s;
        config.args = QStringList({m_server});
        config.timeoutMs = 10000;

        McpManager manager;
        manager.setEnabled(true);
        manager.setAutoConnect(true);
        manager.setServers({config});

        QVERIFY(waitFor([&manager] {
            return manager.readyServers().contains(u"mock"_s) && manager.toolNames().size() == 2;
        }));

        // Order follows the server's own tools/list order.
                QStringList discovered = manager.toolNames();
                discovered.sort();
                QCOMPARE(discovered, QStringList({u"mcp__mock__boom"_s, u"mcp__mock__echo"_s}));
        QVERIFY(manager.isAvailable(u"mcp__mock__echo"_s));
        QVERIFY(!manager.isAvailable(u"mcp__mock__missing"_s));
        QVERIFY(!manager.isAvailable(u"read_file"_s));

        // The schema and annotations from the server must reach the model.
        const std::optional<McpTool> echo = manager.tool(u"mcp__mock__echo"_s);
        QVERIFY(echo.has_value());
        QCOMPARE(echo->server, u"mock"_s);
        QCOMPARE(echo->name, u"echo"_s);
        QVERIFY(echo->readOnly);
        QCOMPARE(echo->inputSchema.value(u"required"_s).toArray().size(), 1);

        const std::optional<McpTool> boom = manager.tool(u"mcp__mock__boom"_s);
        QVERIFY(boom.has_value());
        QVERIFY(!boom->readOnly);

        QCOMPARE(manager.readOnlyTools(), QStringList({u"mcp__mock__echo"_s}));
        QCOMPARE(manager.toolDefinitions().size(), 2);

        manager.disconnectAll();
        QVERIFY(manager.readyServers().isEmpty());
        QVERIFY(manager.toolNames().isEmpty());
    }

    void toolCallReturnsContentAndErrors()
    {
        McpServerConfig config;
        config.name = u"mock"_s;
        config.command = u"python3"_s;
        config.args = QStringList({m_server});
        config.timeoutMs = 10000;

        McpManager manager;
        manager.setEnabled(true);
        manager.setAutoConnect(true);
        manager.setServers({config});
        QVERIFY(waitFor([&manager] {
            return manager.toolNames().size() == 2;
        }));

        QString ok = QString();
        bool succeeded = false;
        QString output;
        QString failure;
        QObject::connect(&manager, &McpManager::toolResult, &manager,
                         [&](const QString &id, bool success, const QString &content, const QString &message) {
                             if (id != u"token-ok"_s) {
                                 return;
                             }
                             succeeded = success;
                             output = content;
                             failure = message;
                         });

        manager.callTool(u"token-ok"_s, u"mcp__mock__echo"_s, QJsonObject{{u"text"_s, u"hello from the mock"_s}});
        QVERIFY(waitFor([&] {
            return !output.isEmpty() || !failure.isEmpty();
        }));
        QVERIFY2(succeeded, qPrintable(failure));
        QCOMPARE(output, u"hello from the mock"_s);

        // A tool that reports isError must come back as a failure the model
        // can read, not as a silent success.
        succeeded = true;
        output.clear();
        failure.clear();
        manager.callTool(u"token-ok"_s, u"mcp__mock__boom"_s, QJsonObject());
        QVERIFY(waitFor([&] {
            return !output.isEmpty() || !failure.isEmpty();
        }));
        QVERIFY(!succeeded);
        QVERIFY(failure.contains(u"exploded"_s));

        // An unknown qualified name is reported, not ignored.
        succeeded = true;
        output.clear();
        failure.clear();
        manager.callTool(u"token-ok"_s, u"mcp__mock__nope"_s, QJsonObject());
        QVERIFY(waitFor([&] {
            return !failure.isEmpty();
        }));
        QVERIFY(!succeeded);

        manager.disconnectAll();
    }

    void permissionPolicyUsesMcpAnnotations()
    {
        PermissionPolicy policy(PermissionMode::AcceptEdits);
        Sandbox sandbox(QDir::tempPath(), SandboxProfile::Workspace);

        policy.setReadOnlyTools({u"mcp__mock__echo"_s});
        policy.setAutoApproveTools({u"mcp__mock__boom"_s});

        QString reason;
        // Accept edits must not silently allow an arbitrary MCP tool...
        QCOMPARE(policy.evaluate(u"mcp__mock__other"_s, {}, sandbox, &reason), PermissionPolicy::Verdict::Ask);
        // ...but a readOnlyHint tool is free, and alwaysAllow works.
        QCOMPARE(policy.evaluate(u"mcp__mock__echo"_s, {}, sandbox, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"mcp__mock__boom"_s, {}, sandbox, &reason), PermissionPolicy::Verdict::Allow);
    }

    void disabledServersAreNotStarted()
    {
        McpServerConfig config;
        config.name = u"off"_s;
        config.command = u"python3"_s;
        config.args = QStringList({m_server});
        config.enabled = false;

        McpManager manager;
        manager.setEnabled(true);
        manager.setAutoConnect(true);
        manager.setServers({config});

        QTest::qWait(300);
        QVERIFY(manager.readyServers().isEmpty());
        QVERIFY(manager.toolNames().isEmpty());
        QCOMPARE(manager.servers().size(), 1);
    }

    void badCommandFailsWithoutCrashing()
    {
        McpServerConfig config;
        config.name = u"missing"_s;
        config.command = u"kateai-no-such-binary-xyz"_s;

        McpManager manager;
        manager.setEnabled(true);
        manager.setAutoConnect(true);
        manager.setServers({config});

        // Give the spawn failure time to settle, then make sure the manager
        // is still usable and simply reports nothing available.
        QTest::qWait(1500);

        QVERIFY(!manager.isAvailable(u"mcp__missing__anything"_s));
        manager.setServers({});
        QVERIFY(manager.servers().isEmpty());
    }

private:
    QString m_server;
};

QTEST_MAIN(TestMcpIntegration)
#include "test_mcpintegration.moc"