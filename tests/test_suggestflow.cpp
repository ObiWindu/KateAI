/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Drives a real AgentLoop against the mock LLM endpoint so the suggest tool's
 * arguments are followed the whole way from the provider's stream to the
 * PermissionRequest the UI builds its card from.
 */

#include "agentloop.h"
#include "test_python.h"
#include "types.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxyFactory>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestSuggestFlow : public QObject
{
    Q_OBJECT

private:
    static QString repoFile(const QString &relativePath)
    {
        QDir dir(QDir::current());
        for (int i = 0; i < 5; ++i) {
            const QString candidate = dir.absoluteFilePath(relativePath);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
            dir.cdUp();
        }
        return {};
    }

    template<typename Predicate>
    static bool waitFor(Predicate predicate, int timeoutMs = 20000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate()) {
            if (timer.elapsed() > timeoutMs) {
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QTest::qWait(10);
        }
        return true;
    }

    std::unique_ptr<QTemporaryDir> m_workspace;
    QProcess *m_server = nullptr;
    QString m_portFile;
    int m_port = 0;

private Q_SLOTS:
    void initTestCase()
    {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
    }

    void init()
    {
        m_workspace = std::make_unique<QTemporaryDir>();
        QVERIFY(m_workspace->isValid());
        QFile marker(m_workspace->path() + u"/README.md"_s);
        QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::Text));
        marker.write("workspace\n");

        m_portFile = m_workspace->path() + u"/port"_s;
        m_server = new QProcess(this);
        const QString script = repoFile(u"tests/mock_llm_server.py"_s);
        QVERIFY2(!script.isEmpty(), "could not locate tests/mock_llm_server.py");
        m_server->start(python3Command(), QStringList{script, m_portFile});
        QVERIFY(m_server->waitForStarted(10000));

        QVERIFY(waitFor([this] {
            return QFileInfo::exists(m_portFile);
        }, 15000));
        QFile portFile(m_portFile);
        QVERIFY(portFile.open(QIODevice::ReadOnly | QIODevice::Text));
        m_port = QString::fromUtf8(portFile.readAll()).trimmed().toInt();
        QVERIFY(m_port > 0);
    }

    void cleanup()
    {
        if (m_server && m_server->state() != QProcess::NotRunning) {
            m_server->kill();
            m_server->waitForFinished(2000);
        }
        m_server = nullptr;
        m_workspace.reset();
    }

    // Runs one turn against the given provider and returns the request the
    // suggest tool call produced, so each shape a provider can stream it in
    // can be checked the same way. The provider picker keeps the settings
    // block readable; nullptr means "no call arrived".
    PermissionRequest suggestRequestFor(Provider provider, const QString &prompt, bool *arrived)
    {
        *arrived = false;
        AgentLoop agent;
        agent.setWorkspace(m_workspace->path());
        Settings settings;
        settings.provider = provider;
        settings.agentMode = u"code"_s;
        if (provider == Provider::ClaudeCompatible) {
            settings.claudeCompatibleUrl = u"http://127.0.0.1:%1"_s.arg(m_port);
            settings.claudeCompatibleApiKey = u"test"_s;
            settings.claudeCompatibleModel = u"mock"_s;
        } else {
            settings.openaiCompatibleUrl = u"http://127.0.0.1:%1/v1"_s.arg(m_port);
            settings.openaiCompatibleApiKey = u"test"_s;
            settings.openaiCompatibleModel = u"mock"_s;
        }
        agent.setSettings(settings);

        QList<PermissionRequest> started;
        connect(&agent, &AgentLoop::toolStarted, this, [&started](const PermissionRequest &request) {
            started.append(request);
        });

        agent.start(prompt);
        waitFor([&started] {
            return !started.isEmpty();
        });
        if (started.size() != 1) {
            return {};
        }
        const PermissionRequest request = started.first();
        if (request.toolName != u"suggest"_s) {
            return {};
        }
        *arrived = true;
        return request;
    }

    void argumentStringShapeKeepsTheAdvice()
    {
        bool arrived = false;
        const PermissionRequest request = suggestRequestFor(Provider::OpenAICompatible, u"Suggest an improvement."_s, &arrived);
        QVERIFY2(arrived, "no suggest call arrived");
        QVERIFY2(request.details.contains(u"std::string_view"_s), qPrintable(request.details));
        QVERIFY2(request.summary.contains(u"std::string_view"_s), qPrintable(request.summary));
    }

    void argumentObjectShapeKeepsTheAdvice()
    {
        // Ollama and other local servers send "arguments" as a JSON object
        // rather than a string; reading it as a string drops it entirely and
        // the call reaches the tool with an empty object.
        bool arrived = false;
        const PermissionRequest request = suggestRequestFor(Provider::OpenAICompatible, u"Suggest with object arguments."_s, &arrived);
        QVERIFY2(arrived, "no suggest call arrived");
        QVERIFY2(request.details.contains(u"std::string_view"_s), qPrintable(request.details));
        QVERIFY2(request.summary.contains(u"std::string_view"_s), qPrintable(request.summary));
    }

    void anthropicFragmentShapeKeepsTheAdvice()
    {
        bool arrived = false;
        const PermissionRequest request = suggestRequestFor(Provider::ClaudeCompatible, u"Suggest an improvement."_s, &arrived);
        QVERIFY2(arrived, "no suggest call arrived");
        QVERIFY2(request.details.contains(u"std::string_view"_s), qPrintable(request.details));
    }
};

QTEST_MAIN(TestSuggestFlow)
#include "test_suggestflow.moc"
