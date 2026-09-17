#pragma once

#include "agentloop.h"
#include "types.h"

#include <QWidget>
#include <QHash>
#include <QLineEdit>
#include <QIcon>
#include <QColor>
#include <QCheckBox>

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
    void rebuildTranscript();

Q_SIGNALS:
    void settingsChanged(const Settings &settings);
    void configureRequested();

private:
    void addUserMessage(const QString &text);
    void addActivityMessage(const QString &text);
    void setStreaming(const QString &text);
    void freezeStreaming();
    void addThinkingBlock(const QString &text);
    void collapseThinkingBlock();
    void toggleThinking();
    void addPlanChecklist(const QJsonArray &plan);
    void markPlanStepCompleted(const QString &stepId);
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
    void updateThinkingButtonStyle();
    void updateReasoningEffortButton();
    bool modelSupportsReasoningEffort() const;
    void showReasoningEffortMenu();
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
    QPushButton *m_reasoningEffort = nullptr;

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

    QWidget *m_toolbar = nullptr;
    QWidget *m_composerContainer = nullptr;
    QWidget *m_composerCard = nullptr;

    // Collapsible hidden-reasoning block rendered at the top of the active
    // assistant turn. Collapsed once the visible answer starts streaming.
    QWidget *m_thinkingBlock = nullptr;
    QTextBrowser *m_thinkingBrowser = nullptr;
    QPushButton *m_thinkingToggle = nullptr;
    bool m_thinkingExpanded = false;

    // Structured plan checklist rendered below the thinking block.
    QWidget *m_planBlock = nullptr;
    QVBoxLayout *m_planLayout = nullptr;
    QHash<QCheckBox *, QString> m_planSteps;

    QHash<QString, ToolCallWidget *> m_toolCallWidgets;
};

} // namespace KateAi
