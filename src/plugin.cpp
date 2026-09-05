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
}

QObject *KateAiPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    return new KateAiView(this, mainWindow);
}

int KateAiPlugin::configPages() const
{
    return 1;
}

KTextEditor::ConfigPage *KateAiPlugin::configPage(int number, QWidget *parent)
{
    if (number != 0) {
        return nullptr;
    }
    return new KateAiConfigPage(parent, this);
}

void KateAiPlugin::setSettings(const Settings &settings)
{
    m_settings = settings;
    SettingsStore::save(m_settings);
    Q_EMIT settingsChanged(m_settings);
}

} // namespace KateAi

#include "plugin.moc"
