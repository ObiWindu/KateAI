#pragma once

#include "agentloop.h"
#include "types.h"

#include <QWidget>
#include <QHash>
#include <QLineEdit>
#include <QIcon>

class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QVBoxLayout;

namespace KateAi
{

class PermissionBar;
class PromptEdit;
class ToolCallWidget;

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
    void addUserMessage(const QString &text);
    void addActivityMessage(const QString &text);
    void setStreaming(const QString &text);
    void freezeStreaming();
    void scrollToBottom();
    void showSettingsMenu();
    void showModelMenu();
    void updateModelSelectorLabel();
    
    void submit();
    void applyProviderToCombos();
    void refreshProviders();
    void refreshModels();
    void updateSendButtonState();
    void updateTokenDisplay();
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
    QPushButton *m_thinking = nullptr;

    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_transcriptContainer = nullptr;
    QVBoxLayout *m_transcriptLayout = nullptr;
    QWidget *m_activeAssistantWidget = nullptr;
    QTextBrowser *m_activeAssistantBrowser = nullptr;

    PermissionBar *m_permissionBar = nullptr;
    PromptEdit *m_prompt = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_threadTitle = nullptr;
    QLabel *m_tokenCount = nullptr;
    QPushButton *m_modelSelector = nullptr;
    QString m_streamText;
    QHash<Provider, QStringList> m_modelCatalog;
    Provider m_preferredProvider = Provider::Grok;
    bool m_updatingCombos = false;
    QString m_modelFilter;

    // Tool call tracking — maps toolCallId to its widget
    QHash<QString, ToolCallWidget *> m_toolCallWidgets;
};

} // namespace KateAi
