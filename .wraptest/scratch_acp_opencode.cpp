#include "acpclient.h"
#include "types.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <cstdio>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

static void qWait(int ms)
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, ms);
}

static bool waitFor(std::function<bool()> predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        qWait(10);
    }
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString scenario = argc > 1 ? QString::fromUtf8(argv[1]) : u"plain"_s;

    Settings settings;
    settings.provider = Provider::Acp;
    settings.apiFormat = ApiFormat::AcpNative;
    settings.acpAgentId = u"opencode"_s;
    settings.acpCommand = u"opencode"_s;
    settings.acpArgs = u"acp"_s;
    settings.permissionMode = PermissionMode::Ask;

    AcpClient client;
    QStringList models;
    QString error;
    int modelsSignals = 0;
    QObject::connect(&client, &AcpClient::modelsReceived, [&](const QStringList &m) {
        ++modelsSignals;
        if (models.isEmpty()) {
            models = m;
        }
        std::printf("[modelsReceived] count=%d\n", int(m.size()));
    });
    QObject::connect(&client, &AcpClient::failed, [&](const QString &message) {
        error = message;
        std::printf("[failed] %s\n", qPrintable(message));
    });
    QObject::connect(&client, &AcpClient::logMessage, [&](const QString &message) {
        std::printf("[log] %s\n", qPrintable(message.left(300)));
    });

    client.setSettings(settings);
    client.setWorkspace(u"/home/macewindu/ai-dev/KateAI"_s);
    client.fetchModels();

    waitFor([&] { return modelsSignals > 0 || !error.isEmpty(); }, 60000);
    std::printf("RESULT models=%d signals=%d error=%s\n",
                int(models.size()), modelsSignals, qPrintable(error));
    std::printf("current model: %s\n", qPrintable(client.currentModelId()));
    if (!models.isEmpty()) {
        std::printf("first models: %s\n", qPrintable(models.mid(0, 5).join(u", "_s)));
    }
    if (scenario == u"set"_s && !models.isEmpty()) {
        // Pick a model that differs from the current one: setModel() no-ops
        // on the id it already runs, which would prove nothing.
        const QString current = client.currentModelId();
        QString target;
        for (const QString &model : models) {
            if (model != current) {
                target = model;
                break;
            }
        }
        if (target.isEmpty()) {
            std::printf("SET SKIPPED: no model other than the current one\n");
        } else {
            std::printf("SET target=%s\n", qPrintable(target));
            const bool accepted = client.setModel(target);
            const bool applied = waitFor([&] { return client.currentModelId() == target; }, 30000);
            std::printf("SET %s: accepted=%d current=%s\n",
                        applied ? "OK" : "FAILED", int(accepted),
                        qPrintable(client.currentModelId()));
            if (!applied) {
                return 1;
            }
        }
    }
    client.stop();
    return 0;
}
