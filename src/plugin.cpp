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
    , m_settings(SettingsStore::load())
{
    // Initialize the plugin with default settings loaded from persistent storage
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

} // namespace KateAi

#include "plugin.moc"
