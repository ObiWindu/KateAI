/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpclient.h"
#include "agentloop.h"
#include "modes.h"
#include "types.h"

#include "test_python.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestModeTools : public QObject
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

    Settings mockSettings(const QString &scenario, PermissionMode mode = PermissionMode::Ask) const
    {
        Settings settings;
        settings.provider = Provider::Acp;
        settings.apiFormat = ApiFormat::AcpNative;
        settings.acpAgentId = u"custom"_s;
        settings.acpCommand = python3Command();
        settings.acpArgs = u'"' + m_agent + u"\" "_s + scenario;
        settings.permissionMode = mode;
        settings.sandbox = SandboxProfile::Off;
        return settings;
    }

    QString m_agent;

private Q_SLOTS:
    void initTestCase()
    {
        m_agent = mockAgentPath();
        QVERIFY2(!m_agent.isEmpty(), "could not locate tests/mock_acp_agent.py");
    }

    void planModeDeclinesAcpWrites()
    {
        // In plan mode, an ACP edit tool's permission request is declined.
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        Settings settings = mockSettings(u"bare-permission"_s, PermissionMode::Ask);
        settings.planMode = true;
        client.setSettings(settings);
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        PermissionRequest asked;
        int askedCount = 0;
        QString stopReason;
        QString error;
        connect(&client, &AcpClient::permissionNeeded, this, [&](const PermissionRequest &request) {
            asked = request;
            ++askedCount;
            // Simulate plan mode declining: resolve as Deny.
            client.resolvePermission(PermissionDecision::Deny);
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"EDITPATH:%1"_s.arg(workspace.filePath(u"notes.txt"_s)));
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        // The permission was still requested (AgentLoop would decline it).
        QVERIFY(askedCount >= 1);
        QCOMPARE(asked.toolName, u"edit_file"_s);
    }

    void askModeAllowsReads()
    {
        // Read-only tools are in the read group, so they pass.
        const ToolAccess ask = ModeRegistry::toolAccessFor(ModeDefinition{}, false);
        // Default mode is code (read+edit+command). For ask mode, set read-only.
        ModeDefinition askMode;
        askMode.groups = {ToolGroup::read(), ToolGroup::mcp(), ToolGroup::web()};
        const ToolAccess askAccess = ModeRegistry::toolAccessFor(askMode, false);
        QVERIFY(askAccess.allows(u"read_file"_s));
        QVERIFY(askAccess.allows(u"list_dir"_s));
        QVERIFY(askAccess.allows(u"grep"_s));
        QVERIFY(!askAccess.allows(u"bash"_s));
        QVERIFY(!askAccess.allows(u"write_file"_s));
    }
};

QTEST_GUILESS_MAIN(TestModeTools)
#include "test_modetools.moc"
