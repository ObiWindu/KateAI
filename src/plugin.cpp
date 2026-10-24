/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "plugin.h"
#include "configpage.h"
#include "kateaicommand.h"
#include "pluginview.h"
#include "settings.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>

using namespace Qt::Literals::StringLiterals;

K_PLUGIN_FACTORY_WITH_JSON(KateAiFactory, "kateai.json", registerPlugin<KateAi::KateAiPlugin>();)

namespace KateAi
{

static constexpr auto kConversationIdsKey = "ConversationIds";

KateAiPlugin::KateAiPlugin(QObject *parent, const QVariantList &args)
    : KTextEditor::Plugin(parent)
    , m_settings(SettingsStore::load())
{
    Q_UNUSED(args);
    // Initialize the plugin with default settings loaded from persistent storage
    // The ":kateai" command is owned by the plugin, so it lives as long as the
    // editor process and dispatches to the view of the window it was used in.
    m_command = new KateAiCommand(this, this);
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

void KateAiPlugin::readSessionConfig(const KConfigGroup &config)
{
    // One id per window, in the order the windows were created when the
    // session was saved. Views claim them in the same order as Kate
    // recreates the windows.
    m_sessionConversations = config.readEntry(kConversationIdsKey, QStringList());
}

void KateAiPlugin::writeSessionConfig(KConfigGroup &config)
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
}

} // namespace KateAi

#include "plugin.moc"
