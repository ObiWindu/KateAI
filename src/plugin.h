#pragma once

#include "types.h"

#include <KTextEditor/Plugin>

namespace KateAi
{

class KateAiPlugin : public KTextEditor::Plugin
{
    Q_OBJECT

public:
    explicit KateAiPlugin(QObject *parent, const QVariantList &args = {});

    QObject *createView(KTextEditor::MainWindow *mainWindow) override;
    int configPages() const override;
    KTextEditor::ConfigPage *configPage(int number = 0, QWidget *parent = nullptr) override;

    Settings settings() const
    {
        return m_settings;
    }
    void setSettings(const Settings &settings);

Q_SIGNALS:
    void settingsChanged(const Settings &settings);

private:
    Settings m_settings;
};

} // namespace KateAi
