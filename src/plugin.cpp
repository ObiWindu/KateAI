/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "plugin.h"
#include "boundedfile.h"
#include "acpinstall.h"
#include "configpage.h"
#include "kateaicommand.h"
#include "pluginview.h"
#include "settings.h"
#include "workspaceservices.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KPluginFactory>

using namespace Qt::Literals::StringLiterals;

K_PLUGIN_FACTORY_WITH_JSON(KateAiFactory, "kateai.json", registerPlugin<KateAi::KateAiPlugin>();)

namespace KateAi
{

static constexpr auto kConversationIdsKey = "ConversationIds";

namespace
{

// Updates only agents installed through Kate's ACP registry. Package agents
// are upgraded through their package manager; binary agents use the same
// verified archive installer as a manual installation.
class AcpAgentAutoUpdater final : public QObject
{
public:
    explicit AcpAgentAutoUpdater(KateAiPlugin *plugin)
        : QObject(plugin)
        , m_plugin(plugin)
    {
    }

    // The plugin owns one network manager for the whole process.
    void setNetwork(QNetworkAccessManager *network)
    {
        if (network) {
            m_network = network;
        }
    }

    void start()
    {
        // The check shells out to package managers and queries the ACP
        // registry; running it on every Kate launch is wasteful and can be a
        // network surprise, so run it at most once a week.
        {
            KConfigGroup g(KSharedConfig::openConfig(), u"KateAI"_s);
            const qint64 lastCheck = g.readEntry(u"AcpUpdateLastCheck"_s, 0LL);
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            if (lastCheck > 0 && now - lastCheck < 7 * 24 * 3600) {
                return;
            }
            g.writeEntry(u"AcpUpdateLastCheck"_s, now);
            g.sync();
        }
        if (!m_plugin) {
            return;
        }
        const QJsonArray installed = QJsonDocument::fromJson(m_plugin->settings().acpInstalledAgents.toUtf8()).array();
        bool registryHasMistral = false;
        for (const QJsonValue &value : installed) {
            if (value.toObject().value(u"id"_s).toString() == u"mistral-vibe"_s) {
                registryHasMistral = true;
                break;
            }
        }
        if (installed.isEmpty()) {
            if (!registryHasMistral && !QStandardPaths::findExecutable(u"vibe-acp"_s).isEmpty()) {
                checkStandaloneMistral();
            } else {
                updateNext();
            }
            return;
        }

        QNetworkRequest request(QUrl(u"https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json"_s));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = m_network->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, installed, registryHasMistral]() {
            // Same registry document as the settings page: bound it before the
            // copy and the JSON parse.
            constexpr qint64 kMaxRegistryBodyBytes = 4 * 1024 * 1024;
            if (!replyBodyWithinLimit(reply, kMaxRegistryBodyBytes)) {
                reply->deleteLater();
                qWarning() << "KateAI: ACP agent update check failed: the registry response is too large";
                if (!registryHasMistral && !QStandardPaths::findExecutable(u"vibe-acp"_s).isEmpty()) {
                    checkStandaloneMistral();
                } else {
                    updateNext();
                }
                return;
            }
            const QByteArray body = reply->readAll();
            const QString error = reply->errorString();
            const bool ok = reply->error() == QNetworkReply::NoError;
            reply->deleteLater();
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
            if (!ok || !document.isObject() || !document.object().value(u"agents"_s).isArray()) {
                qWarning() << "KateAI: ACP agent update check failed:" << (ok ? parseError.errorString() : error);
                if (!registryHasMistral && !QStandardPaths::findExecutable(u"vibe-acp"_s).isEmpty()) {
                    checkStandaloneMistral();
                } else {
                    updateNext();
                }
                return;
            }

            QHash<QString, QJsonObject> latestById;
            for (const QJsonValue &value : document.object().value(u"agents"_s).toArray()) {
                const QJsonObject agent = value.toObject();
                const QString id = agent.value(u"id"_s).toString();
                if (!id.isEmpty()) {
                    latestById.insert(id, agent);
                }
            }

            for (const QJsonValue &value : installed) {
                const QJsonObject current = value.toObject();
                const QString id = current.value(u"id"_s).toString();
                const QJsonObject latest = latestById.value(id);
                if (id.isEmpty() || latest.isEmpty()
                    || latest.value(u"version"_s).toString() == current.value(u"version"_s).toString()) {
                    continue;
                }

                QString distributionType = current.value(u"distributionType"_s).toString();
                if (distributionType.isEmpty() && !current.value(u"binaryArchiveUrl"_s).toString().isEmpty()) {
                    distributionType = u"binary"_s;
                }
                if (distributionType == u"binary"_s) {
                    if (!latest.value(u"distribution"_s).toObject().value(u"binary"_s).toObject().isEmpty()) {
                        m_pending.append({latest, distributionType, {}, current});
                    }
                } else if (distributionType == u"npx"_s || distributionType == u"uvx"_s) {
                    const QJsonObject distribution = latest.value(u"distribution"_s).toObject();
                    const QString key = distributionType;
                    const QJsonObject packageInfo = distribution.value(key).toObject();
                    const QString package = packageInfo.value(u"package"_s).toString();
                    if (!package.isEmpty()) {
                        m_pending.append({latest, distributionType, package, current});
                    }
                }
            }
            if (!registryHasMistral && !QStandardPaths::findExecutable(u"vibe-acp"_s).isEmpty()) {
                checkStandaloneMistral();
            } else {
                updateNext();
            }
        });
    }

private:
    struct Update
    {
        QJsonObject latest;
        QString type;
        QString package;
        QJsonObject previous;
    };

    static QString packageName(QString package)
    {
        const int at = package.lastIndexOf(u'@');
        const int scopeSlash = package.startsWith(u'@') ? package.indexOf(u'/') : -1;
        if (at > scopeSlash) {
            package.truncate(at);
        }
        return package;
    }

    static QString packageAtLatest(const QString &package)
    {
        return packageName(package) + u"@latest"_s;
    }

    static QString pinPackage(const QString &package, const QString &version, bool npm)
    {
        if (version.trimmed().isEmpty()) {
            return package;
        }
        const bool alreadyPinned = npm ? (package.lastIndexOf(u'@') > 0)
                                       : (package.contains(u"=="_s) || package.lastIndexOf(u'@') > 0);
        if (alreadyPinned) {
            return package;
        }
        const QString suffix = npm ? u"@"_s : u"=="_s;
        return package + suffix + version.trimmed();
    }

    static QStringList stringList(const QJsonArray &array)
    {
        QStringList out;
        for (const QJsonValue &value : array) {
            out.append(value.toString());
        }
        return out;
    }

    static QJsonArray jsonArray(const QStringList &values)
    {
        QJsonArray out;
        for (const QString &value : values) {
            out.append(value);
        }
        return out;
    }

    void checkStandaloneMistral()
    {
        auto *process = new QProcess(this);
        m_process = process;
        auto finish = [this, process](bool success) {
            if (m_process != process) {
                return;
            }
            const QString output = QString::fromLocal8Bit(process->readAllStandardOutput());
            m_process = nullptr;
            if (success && QRegularExpression(u"^\\s*(?:[-*]\\s*)?mistral-vibe(?:\\s|$)"_s,
                                              QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption)
                                   .match(output).hasMatch()) {
                // `uv tool list --outdated` only includes tools that need an
                // upgrade, avoiding a reinstall of Mistral Vibe on every Kate
                // launch when it is already current.
                m_pending.prepend({{}, u"uvx"_s, u"mistral-vibe"_s, {}});
            }
            process->deleteLater();
            updateNext();
        };
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [finish](int code, QProcess::ExitStatus status) { finish(code == 0 && status == QProcess::NormalExit); });
        connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                finish(false);
            }
        });
        auto *timeout = new QTimer(process);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, process, &QProcess::kill);
        connect(process, &QProcess::started, timeout, [timeout]() { timeout->start(30000); });
        process->start(u"uv"_s, {u"tool"_s, u"list"_s, u"--outdated"_s});
    }

    void updateNext()
    {
        if (m_pending.isEmpty() || !m_plugin) {
            return;
        }
        const Update update = m_pending.takeFirst();
        if (update.type == u"binary"_s) {
            auto *installer = new AcpBinaryInstaller(update.latest, acpCurrentPlatformKey(), m_network, this);
            m_installer = installer;
            connect(installer, &AcpBinaryInstaller::finished, this, [this, installer](const QJsonObject &updated) {
                saveUpdatedAgent(updated);
                if (m_installer == installer) {
                    m_installer = nullptr;
                }
                installer->deleteLater();
                updateNext();
            });
            connect(installer, &AcpBinaryInstaller::failed, this, [this, installer, id = update.latest.value(u"id"_s).toString()](const QString &message) {
                qWarning() << "KateAI: could not update ACP agent" << id << ':' << message;
                if (m_installer == installer) {
                    m_installer = nullptr;
                }
                installer->deleteLater();
                updateNext();
            });
            installer->start();
            return;
        }

        const bool npm = update.type == u"npx"_s;
        if (!update.latest.isEmpty()) {
            // A registry package agent is launched directly through npx/uvx
            // with the version pinned in its args, so an update is just a new
            // pinned launch line. Running `npm install --global` both mutated
            // the user's global prefix and left the old pinned args in place,
            // so the old version kept running.
            const QJsonObject distribution = update.latest.value(u"distribution"_s).toObject();
            const QJsonObject packageInfo = distribution.value(update.type).toObject();
            QStringList launch;
            if (npm) {
                launch.append(u"--yes"_s);
            }
            launch.append(pinPackage(update.package, update.latest.value(u"version"_s).toString(), npm));
            launch += stringList(packageInfo.value(u"args"_s).toArray());
            QJsonObject updated = update.latest;
            updated.insert(u"command"_s, npm ? u"npx"_s : u"uvx"_s);
            updated.insert(u"args"_s, jsonArray(launch));
            updated.insert(u"package"_s, update.package);
            updated.insert(u"pinnedPackage"_s, pinPackage(update.package, update.latest.value(u"version"_s).toString(), npm));
            updated.insert(u"distributionType"_s, update.type);
            saveUpdatedAgent(updated);
            updateNext();
            return;
        }

        auto *process = new QProcess(this);
        m_process = process;
        const QString package = packageAtLatest(update.package);
        const QString program = npm ? u"npm"_s : u"uv"_s;
        const QStringList args = npm
            ? QStringList{u"install"_s, u"--global"_s, package}
            : QStringList{u"tool"_s, u"upgrade"_s, u"--refresh"_s, packageName(update.package)};
        auto finish = [this, process, update](bool success) {
            if (m_process != process) {
                return;
            }
            m_process = nullptr;
            if (success) {
                if (!update.latest.isEmpty()) {
                    QJsonObject updated = update.latest;
                    updated.insert(u"command"_s, update.previous.value(u"command"_s));
                    updated.insert(u"args"_s, update.previous.value(u"args"_s));
                    updated.insert(u"package"_s, update.package);
                    updated.insert(u"distributionType"_s, update.type);
                    saveUpdatedAgent(updated);
                }
            } else {
                const QString output = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
                qWarning() << "KateAI: ACP package update failed for" << update.latest.value(u"id"_s).toString() << output;
            }
            process->deleteLater();
            updateNext();
        };
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [finish](int code, QProcess::ExitStatus status) { finish(code == 0 && status == QProcess::NormalExit); });
        connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                finish(false);
            }
        });
        auto *timeout = new QTimer(process);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, this, [process]() { process->kill(); });
        connect(process, &QProcess::started, timeout, [timeout]() { timeout->start(120000); });
        process->start(program, args);
    }

    void saveUpdatedAgent(const QJsonObject &updated)
    {
        if (!m_plugin) {
            return;
        }
        Settings settings = m_plugin->settings();
        QJsonArray agents = QJsonDocument::fromJson(settings.acpInstalledAgents.toUtf8()).array();
        const QString id = updated.value(u"id"_s).toString();
        bool replaced = false;
        for (qsizetype i = 0; i < agents.size(); ++i) {
            if (agents.at(i).toObject().value(u"id"_s).toString() == id) {
                agents.replace(i, updated);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            return;
        }
        settings.acpInstalledAgents = QString::fromUtf8(QJsonDocument(agents).toJson(QJsonDocument::Compact));
        if (settings.acpAgentId == id) {
            const AcpAgentPreset launch = acpAgentLaunchLine(settings, id);
            if (!launch.command.isEmpty()) {
                settings.acpCommand = launch.command;
                settings.acpArgs = launch.args;
                settings.acpApiKeyEnv = launch.apiKeyEnv;
            }
        }
        m_plugin->setSettings(settings);
    }

    QPointer<KateAiPlugin> m_plugin;
    QNetworkAccessManager *m_network = nullptr;
    QList<Update> m_pending;
    QPointer<QProcess> m_process;
    QPointer<AcpBinaryInstaller> m_installer;
};

} // namespace

KateAiPlugin::KateAiPlugin(QObject *parent, const QVariantList &args)
    : KTextEditor::Plugin(parent)
    , m_settings(SettingsStore::load())
    , m_network(new QNetworkAccessManager(this))
{
    Q_UNUSED(args);
    // Initialize the plugin with default settings loaded from persistent storage
    // The ":kateai" command is owned by the plugin, so it lives as long as the
    // editor process and dispatches to the view of the window it was used in.
    m_command = new KateAiCommand(this, this);
    auto *acpUpdater = new AcpAgentAutoUpdater(this);
    acpUpdater->setNetwork(m_network);
    QTimer::singleShot(0, acpUpdater, [acpUpdater]() { acpUpdater->start(); });
}

QObject *KateAiPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    // Create and return a new Kate AI view for the given main window
    return new KateAiView(this, mainWindow);
}

int KateAiPlugin::configPages() const
{
    // Return the number of configuration pages this plugin provides (1 page)
    return 1;
}

KTextEditor::ConfigPage *KateAiPlugin::configPage(int number, QWidget *parent)
{
    // Return the configuration page for the given page number, or nullptr if invalid
    if (number != 0) {
        return nullptr;
    }
    return new KateAiConfigPage(parent, this);
}

void KateAiPlugin::setSettings(const Settings &settings)
{
    // Update plugin settings, save them persistently, and notify listeners
    m_settings = settings;
    SettingsStore::save(m_settings);
    Q_EMIT settingsChanged(m_settings);
}

std::shared_ptr<WorkspaceServices> KateAiPlugin::workspaceServices(const QString &workspace)
{
    if (workspace.trimmed().isEmpty() || !QFileInfo(workspace).isDir()) {
        return {};
    }
    QString key = QFileInfo(workspace).canonicalFilePath();
    if (key.isEmpty()) {
        key = QDir::cleanPath(workspace);
    }
    // Drop entries whose services were released with the last view. Without
    // this, every folder ever opened leaves a dead key for the session.
    for (auto it = m_workspaceServices.begin(); it != m_workspaceServices.end();) {
        if (it->expired()) {
            it = m_workspaceServices.erase(it);
        } else {
            ++it;
        }
    }
    const auto existing = m_workspaceServices.constFind(key);
    if (existing != m_workspaceServices.constEnd()) {
        if (const auto live = existing->lock()) {
            return live;
        }
    }

    auto services = std::make_shared<WorkspaceServices>(key);
    m_workspaceServices.insert(key, services);
    return services;
}

void KateAiPlugin::readSessionConfig(const KConfigGroup &config)
{
    // One id per window, in the order the windows were created when the
    // session was saved. Views claim them in the same order as Kate
    // recreates the windows.
    m_sessionConversations = config.readEntry(kConversationIdsKey, QStringList());
    m_lastConversationIds = m_sessionConversations;
}

QStringList KateAiPlugin::liveConversationIds() const
{
    QStringList ids;
    for (KateAiView *view : m_views) {
        // A view that never settled on a conversation contributes nothing;
        // trailing empty entries must not shift the ids of later windows.
        const QString id = m_viewConversations.value(view);
        if (!id.isEmpty()) {
            ids.append(id);
        }
    }
    return ids;
}

void KateAiPlugin::writeSessionConfig(KConfigGroup &config)
{
    // Normally the live views are still here and are the authoritative list.
    // Kate may write the session config after it has destroyed the windows
    // (the plugin and its views are separate objects), so when every view has
    // already unregistered, fall back to the order captured on the last change
    // or unregister. Writing an empty list there made every restart open a
    // blank chat even though the transcripts were safely stored.
    QStringList ids = liveConversationIds();
    if (ids.isEmpty()) {
        ids = m_lastConversationIds;
    } else {
        m_lastConversationIds = ids;
    }
    config.writeEntry(kConversationIdsKey, ids);
}

void KateAiPlugin::registerView(KateAiView *view)
{
    if (view && !m_views.contains(view)) {
        m_views.append(view);
    }
}

void KateAiPlugin::unregisterView(KateAiView *view)
{
    // Snapshot before dropping the view: this may be the last place the session
    // id is available if Kate tears the window down before writing the session.
    const QStringList ids = liveConversationIds();
    if (!ids.isEmpty()) {
        m_lastConversationIds = ids;
    }
    m_views.removeAll(view);
    m_viewConversations.remove(view);
}

QString KateAiPlugin::claimSessionConversation()
{
    if (m_sessionConversations.isEmpty()) {
        return QString();
    }
    return m_sessionConversations.takeFirst();
}

void KateAiPlugin::noteConversationChanged(KateAiView *view, const QString &conversationId)
{
    if (!view || conversationId.isEmpty()) {
        return;
    }
    m_viewConversations.insert(view, conversationId);
    const QStringList ids = liveConversationIds();
    if (!ids.isEmpty()) {
        m_lastConversationIds = ids;
    }
}

} // namespace KateAi

#include "plugin.moc"
