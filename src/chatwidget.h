/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "agentloop.h"
#include "types.h"

#include <QWidget>
#include <QHash>
#include <QList>
#include <QLineEdit>
#include <QIcon>
#include <QColor>
#include <QCheckBox>
#include <QPointer>

class QAction;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QTimer;
class QVBoxLayout;
class QMenu;

namespace KateAi
{

class PermissionBar;
class PromptEdit;
class ToolCallWidget;
class EditTracker;

class ChatWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ChatWidget(QWidget *parent = nullptr);
    ~ChatWidget() override;

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
    void restoreCurrentTurn(const SessionStore::SessionData &sessionData);
    void updateHistoryButton();

    // Conversation history
    void showConversationHistory();
    void switchToConversation(const QString &conversationId);
    void deleteConversation(const QString &conversationId);
    void setCurrentConversationId(const QString &conversationId);

    PromptEdit *promptEdit() const { return m_prompt; }
    void setCompletionWords(const QStringList &words);

Q_SIGNALS:
    void settingsChanged(const Settings &settings);
    void configureRequested();
    void aboutToSubmit();
    void conversationChanged(const QString &conversationId);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void addUserMessage(const QString &text);
    void addActivityMessage(const QString &text);
    void setStreaming(const QString &text);
    void freezeStreaming();
    void addThinkingBlock(const QString &text);
    void appendThinkingDelta(const QString &delta);
    void renderThinkingHtml();
    void collapseThinkingBlock();
    void toggleThinking();
    QWidget *createThinkingBlock(QWidget *parent, QTextBrowser *&browser, QPushButton *&toggle, bool initiallyExpanded);
    void applyThinkingState(QWidget *block, QTextBrowser *browser, QPushButton *toggle, bool expanded);
    void addPlanChecklist(const QJsonArray &plan);
    void markPlanStepCompleted(const QString &stepId);
    void scrollToBottom();
    void forceScrollToBottom();
    void updateScrollButtonPosition();
    void animateScrollButtonShow();
    void animateScrollButtonHide();
    QPushButton *createCopyButton(const QString &textToCopy, QWidget *parent);
    QWidget *createWelcomeWidget();
    int transcriptInsertIndex() const;
    void appendTranscriptWidget(QWidget *widget);
    void showSettingsMenu();
    void showModelMenu();
    void rebuildModelMenuProviderSubmenus();
    void applyModelMenuFilter();
    void selectModel(Provider provider, const QString &model);
    void updateModelSelectorLabel();
    void showInfoMessage(const QString &message, bool isError);

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
    void setThinkingIndicator(bool show);
    void setWorkingIndicator(bool show);
    void scheduleStreamHeightUpdate();
    void startThinkingPacer();
    void stopThinkingPacer();
    void flushThinkingPacer();
    void updateThinkingDisplay();
    void resetThinkingState();
    void clearStreamingPointers();
    static QString escape(const QString &text);
    static QString markdownToHtml(const QString &text);
    static QString closedMarkdown(const QString &text);

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
    QPointer<QWidget> m_activeAssistantWidget;
    QPointer<QTextBrowser> m_activeAssistantBrowser;

    PermissionBar *m_permissionBar = nullptr;
    PromptEdit *m_prompt = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_infoBar = nullptr;
    QLabel *m_threadTitle = nullptr;
    QLabel *m_tokenCount = nullptr;
    QPushButton *m_modelSelector = nullptr;
    QString m_streamText;
    QHash<Provider, QStringList> m_modelCatalog;
    Provider m_preferredProvider = Provider::Grok;
    bool m_updatingCombos = false;
    QString m_modelFilter;
    QMenu *m_modelMenu = nullptr;
    QList<QMenu *> m_modelMenuProviderMenus;
    QList<QAction *> m_modelMenuFlatActions;
    QAction *m_modelMenuNoMatchAction = nullptr;

    QWidget *m_toolbar = nullptr;
    QWidget *m_composerContainer = nullptr;
    QWidget *m_composerCard = nullptr;

    // Collapsible hidden-reasoning block rendered at the top of the active
    // assistant turn. Collapsed once the visible answer starts streaming.
    QPointer<QWidget> m_thinkingBlock;
    QPointer<QTextBrowser> m_thinkingBrowser;
    QPointer<QPushButton> m_thinkingToggle;
    bool m_thinkingExpanded = true;

    // Raw thinking text buffer for the current turn.
    QString m_thinkingBuffer;
    int m_thinkingPacedLength = 0;
    QTimer *m_thinkingPacerTimer = nullptr;

    // Structured plan checklist rendered below the thinking block.
    QPointer<QWidget> m_planBlock;
    QPointer<QVBoxLayout> m_planLayout;
    QHash<QCheckBox *, QString> m_planSteps;

    QHash<QString, ToolCallWidget *> m_toolCallWidgets;
    QPointer<QPushButton> m_scrollToBottomBtn;
    QPointer<QPushButton> m_activeAssistantCopyBtn;
    bool m_userScrolledUp = true;
    QTimer *m_streamHeightTimer = nullptr;

    // Dynamic status indicators at bottom of chat
    QLabel *m_thinkingIndicator = nullptr;
    QLabel *m_workingIndicator = nullptr;
    bool m_isThinking = false;
    bool m_isWorking = false;

    // Edit tracker for AcceptEdits permission mode
    EditTracker *m_editTracker = nullptr;

    // Track pending write/edit tool calls for edit tracking
    QHash<QString, PermissionRequest> m_pendingToolCalls;

    // Conversation history
    QPushButton *m_historyButton = nullptr;
    QMenu *m_historyMenu = nullptr;
    QString m_currentConversationId;
    bool m_loadingConversation = false;
};

} // namespace KateAi