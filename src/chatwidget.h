#pragma once

#include "agentloop.h"
#include "types.h"

#include <QWidget>
#include <QHash>
#include <QLineEdit>

class QComboBox;
class QLabel;
class QPushButton;
class QTextBrowser;

namespace KateAi
{

class PermissionBar;
class PromptEdit;

class ChatWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ChatWidget(QWidget *parent = nullptr);

    AgentLoop *agent()
    {
        return &m_agent;
    }

    void setSettings(const Settings &settings);
    Settings settings() const
    {
        return m_settings;
    }

    void focusPrompt();
    void ask(const QString &text);
    void newChat();

Q_SIGNALS:
    void settingsChanged(const Settings &settings);
    void configureRequested();

private:
    void appendHtml(const QString &html);
    void setStreaming(const QString &text);
    void freezeStreaming();
    void submit();
    void applyProviderToCombos();
    void refreshProviders();
    void refreshModels();
    void updateSendButtonState();
    static QString escape(const QString &text);
    static QString markdownToHtml(const QString &text);

    Settings m_settings;
    AgentLoop m_agent;
    QComboBox *m_provider = nullptr;
    QComboBox *m_model = nullptr;
    QComboBox *m_permission = nullptr;
    QComboBox *m_sandbox = nullptr;
    QComboBox *m_mode = nullptr;
    QPushButton *m_newChat = nullptr;
    QPushButton *m_configure = nullptr;
    QPushButton *m_send = nullptr;
    QPushButton *m_stop = nullptr;
    QTextBrowser *m_transcript = nullptr;
    PermissionBar *m_permissionBar = nullptr;
    PromptEdit *m_prompt = nullptr;
    QLabel *m_status = nullptr;
    QString m_historyHtml;
    QString m_streamText;
    QHash<Provider, QStringList> m_modelCatalog;
    Provider m_preferredProvider = Provider::Grok;
    bool m_updatingCombos = false;
    QString m_modelFilter;
};

} // namespace KateAi
