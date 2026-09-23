/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "plugin.h"
#include "configpage.h"
#include "pluginview.h"
#include "settings.h"

#include <KLocalizedString>
#include <KPluginFactory>

using namespace Qt::Literals::StringLiterals;

K_PLUGIN_FACTORY_WITH_JSON(KateAiFactory, "kateai.json", registerPlugin<KateAi::KateAiPlugin>();)

namespace KateAi
{

KateAiPlugin::KateAiPlugin(QObject *parent, const QVariantList &)
    : KTextEditor::Plugin(parent)
{
    // Keep plugin construction cheap. Persistent settings are loaded when Kate
    // actually requests a plugin view or configuration page.
}

void KateAiPlugin::ensureSettingsLoaded()
{
    if (!m_settingsLoaded) {
        m_settings = SettingsStore::load();
        m_settingsLoaded = true;
    }
}

QObject *KateAiPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    ensureSettingsLoaded();
    return new KateAiView(this, mainWindow);
}

int KateAiPlugin::configPages() const
{
    // Return the number of configuration pages this plugin provides (1 page)
    return 1;
}

KTextEditor::ConfigPage *KateAiPlugin::configPage(int number, QWidget *parent)
{
    if (number != 0) {
        return nullptr;
    }
    ensureSettingsLoaded();
    return new KateAiConfigPage(parent, this);
}

void KateAiPlugin::setSettings(const Settings &settings)
{
    // Update plugin settings, save them persistently, and notify listeners
    m_settings = settings;
    m_settingsLoaded = true;
    SettingsStore::save(m_settings);
    Q_EMIT settingsChanged(m_settings);
}

} // namespace KateAi

#include "plugin.moc"
