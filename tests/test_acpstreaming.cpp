/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpclient.h"
#include "documentbridge.h"
#include "test_python.h"
#include "types.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestAcpStreaming : public QObject
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
        settings.acpCommand = python3Command();
        settings.acpArgs = u'"' + m_agent + u"\" "_s + scenario;
        settings.permissionMode = PermissionMode::AlwaysApprove;
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

    void toolStreamsPartialOutput()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"toolstream"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString progress;
        QString finalOutput;
        QString stopReason;
        QString error;
        connect(&client, &AcpClient::toolProgress, this, [&](const QString &toolCallId, const QString &partial) {
            if (toolCallId == u"call_stream"_s) {
                progress = partial;
            }
        });
        connect(&client, &AcpClient::toolFinished, this, [&](const ToolResult &result) {
            if (result.toolCallId == u"call_stream"_s) {
                finalOutput = result.output;
            }
        });
        connect(&client, &AcpClient::promptFinished, this, [&](const QString &reason, const QString &, const QJsonArray &) {
            stopReason = reason;
        });
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"stream it"_s);
        QVERIFY2(waitFor([&] {
            return !stopReason.isEmpty() || !error.isEmpty();
        }), qPrintable(error.isEmpty() ? u"timed out"_s : error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(progress.contains(u"scanning dependencies"_s));
        QVERIFY(finalOutput.contains(u"build finished"_s));
    }

    void errorSurfaced()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        DiskDocumentBridge bridge;
        AcpClient client;
        client.setSettings(mockSettings(u"error-details"_s));
        client.setWorkspace(workspace.path());
        client.setDocumentBridge(&bridge);

        QString error;
        connect(&client, &AcpClient::failed, this, [&](const QString &message) {
            error = message;
        });

        client.prompt(u"do it"_s);
        QVERIFY2(waitFor([&] {
            return !error.isEmpty();
        }), "timed out");
        QVERIFY(error.contains(u"OAuth credential file permissions"_s));
    }
};

QTEST_GUILESS_MAIN(TestAcpStreaming)
#include "test_acpstreaming.moc"
