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
#include <QShortcut>

class QAction;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QTimer;
class QVBoxLayout;
class QMenu;
class QListWidget;
class QTabWidget;

namespace KateAi
{

class PermissionBar;
class PromptEdit;
class ToolCallWidget;
class SubtaskWidget;
class SuggestionWidget;
class QuestionWidget;
class EditTracker;
class TurnStatus;

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
    // Re-applies the theme palette to the whole panel and repaints the
    // transcript. Call after ChatTheme::loadTheme() when the theme changes.
    void applyTheme();

    void focusPrompt();
    void ask(const QString &text);
    void newChat();
    void rebuildTranscript();
    void restoreCurrentTurn(const SessionStore::SessionData &sessionData);
    void updateHistoryButton();

    // --- Global transcript controls ------------------------------------------
    // The per-card budget (`maxExpandedToolCards`) decides what opens on its
    // own; these are the user overriding it once for a whole transcript, the
    // way a long log of tool cards needs. The override is dropped when a new
    // turn starts, so the automatic behaviour comes back.
    void setAllToolCardsExpanded(bool expanded);
    bool allToolCardsExpanded() const;
    void toggleAllToolCards();
    // Walks the tool cards: +1 down, -1 up. Expands the card it lands on and
    // scrolls it into view, so a transcript can be read card by card without
    // hunting for the next one.
    void focusToolCard(int delta);
    int focusedToolCardIndex() const;

    // Plan mode is a read-only turn restriction toggled from two menus and now
    // a shortcut; the toggle lives here so all three stay in step.
    void togglePlanMode();
    // Steps the model list of the current provider, wrapping at both ends.
    // Returns false when the provider offers fewer than two models.
    bool cycleModel(int delta);

    // Conversation history
    void showConversationHistory();
    void switchToConversation(const QString &conversationId);
    void deleteConversation(const QString &conversationId);
    void setCurrentConversationId(const QString &conversationId);

    PromptEdit *promptEdit() const { return m_prompt; }
    void setCompletionWords(const QStringList &words);

    // Modes / MCP / checkpoints, exposed so the view can drive them too.
    void showModeMenu();
    void showMcpMenu();
    void showTeamMenu();
    void updateTeamButton();
    void markRunningSubtasksAbandoned();
    void showCheckpointMenu();
    void showCheckpointMenuFor(const CheckpointInfo &info);
    void setMcpToolAutoApproved(const McpTool &tool, bool enabled);
    void applyMode(const QString &modeId);

Q_SIGNALS:
    void settingsChanged(const Settings &settings);
    void configureRequested();
    void aboutToSubmit();
    void conversationChanged(const QString &conversationId);
    // A suggestion card was accepted and carries a file anchor. The view opens
    // the file at that line; Kate core only owns the card.
    void openFileAtLineRequested(const QString &path, int line);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void scheduleSessionSave();
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
    void pinWidgetInViewport(QWidget *widget);
    void applyPinnedViewport();
    void updateScrollButtonPosition();
    void animateScrollButtonShow();
    void animateScrollButtonHide();
    QPushButton *createCopyButton(const QString &textToCopy, QWidget *parent);
    QWidget *createWelcomeWidget();
    QWidget *createIndicatorsRow();
    int transcriptInsertIndex() const;
    void appendTranscriptWidget(QWidget *widget);
    void showSettingsMenu();
    void showModelMenu();
    void positionModelMenu();
    void fillModelResultList();
    void refreshModeButton();
    void showPermissionMenu();
    void refreshPermissionButton();
    void populateModes();
    void updateMcpButton();
    void applyAutoApproveTool(const QString &toolName, bool enabled);
    void applyModelMenuFilter();
    void moveModelMenuSelection(int delta);
    void selectModel(Provider provider, const QString &model);
    void selectAcpAgentAndModel(const QString &agentId, const QString &model);
    void requestAcpAgentModels(const QString &agentId);
    Settings settingsForAcpAgent(const QString &agentId) const;
    void updateModelSelectorLabel();
    void showInfoMessage(const QString &message, bool isError);
    void applyInfoBarStyle();
    void saveCurrentConversation();
    // Re-applies every theme-derived stylesheet on the panel chrome and the
    // live transcript widgets, without rebuilding the transcript.
    void restylePanel();
    void restyleWelcomeWidget();
    void restyleLiveCards();

    // --- Intent dock ---------------------------------------------------------
    // A strip pinned directly above the composer holding everything that needs
    // the user to act: sub-agent cards (cancel) and approval rows
    // (allow / deny). Anything in here stays reachable without scrolling the
    // transcript. Newest item goes on top so the live card is nearest the input.
    void addIntentWidget(QWidget *widget);
    // Moves an existing widget into the dock without rebuilding it, so the
    // approval strip stays the single instance the tool card owns.
    void moveToIntentDock(QWidget *widget, int index = -1);
    void updateIntentDockVisibility();
    // Drops every docked widget; used when the transcript is cleared.
    void clearIntentDock();
    // Moves a docked widget back into the transcript once it stops needing
    // attention, so the dock only ever holds live work and the finished card
    // rejoins the conversation.
    void retireIntentWidget(QWidget *widget);

    // --- Suggestion cards ------------------------------------------------------
    // One card per suggestion the agent posted, keyed by the tool call id that
    // produced it (or a generated id on the ACP path). Pending entries exist
    // between toolStarted and toolFinished, because that is the only window in
    // which the arguments are visible before the card is built.
    // Posts a suggestion card. `running` marks it as still in flight: the tool
    // carrying the advice has not reported back yet.
    void addSuggestionCard(const QString &id, const Suggestion &suggestion, bool running = false);
    void addSuggestionErrorCard(const QString &id, const QString &message);
    void removeSuggestionCard(const QString &id);
    void clearSuggestionCards();

    // --- Question cards --------------------------------------------------------
    // The `question` tool stops the turn until the user answers, so its card
    // lives in the intent dock while it waits and is retired into the
    // transcript once it does. Keyed by tool call id, like the tool cards.
    void showQuestionCard(const PermissionRequest &request);
    void clearQuestionCards();

    void submit();
        // True while a tool card is showing its inline approval row.
        bool m_approvalPending() const;
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
    void tickIndicators();
    void syncIndicatorAnimation();
    bool registerThinkingBlock(QWidget *block, QTextBrowser *browser, QPushButton *toggle);
    void applyTranscriptCollapse();
    void clearTranscriptContents();
    void reflowTranscriptMedia();
    void scheduleStreamHeightUpdate();
    // Streaming renders the whole accumulated answer on every chunk, which is
    // quadratic and saturates the UI thread once the answer (or the surrounding
    // context) gets large. These coalesce the rebuild onto a timer so the number
    // of full re-renders is bounded by wall-clock rate rather than token count.
    void scheduleStreamRender();
    void flushStreamRender();
    void flushToolProgress();
    void fitUserMessageBody(QTextBrowser *browser);
    // One finished card for a tool call already stored in the session. The
    // caller decides where it goes: ACP records the result before the assistant
    // message, native turns record it after.
    void restoreHistoricalTool(const QString &toolCallId, const QString &toolName,
                               const QString &argumentsJson, const QString &output, bool hasResult);
    static bool isInternalUserMessage(const QString &text);
    void startThinkingPacer();
    void stopThinkingPacer();
    void flushThinkingPacer();
    void updateThinkingDisplay();
    void resetThinkingState();
    void clearStreamingPointers();
    static QString escape(const QString &text);
    static QString markdownToHtml(const QString &text);
    static QString closedMarkdown(const QString &text);
    void updateExpandAllButton();

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
    QPushButton *m_modeButton = nullptr;
    QPushButton *m_permissionButton = nullptr;
    QPushButton *m_mcpButton = nullptr;
    QPushButton *m_checkpointButton = nullptr;
        QPushButton *m_teamButton = nullptr;

    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_transcriptContainer = nullptr;
    QVBoxLayout *m_transcriptLayout = nullptr;
    QPointer<QWidget> m_activeAssistantWidget;
    QPointer<QTextBrowser> m_activeAssistantBrowser;

    PermissionBar *m_permissionBar = nullptr;
    // Intent dock: sits between the transcript and the permission bar.
    QWidget *m_intentDock = nullptr;
    QVBoxLayout *m_intentDockLayout = nullptr;
    PromptEdit *m_prompt = nullptr;
    TurnStatus *m_turnStatus = nullptr;
    // Tool calls completed in the current turn, shown live in the status strip.
    int m_completedToolCount = 0;
    QLabel *m_infoBar = nullptr;
    // What the info bar is currently showing, so a theme change can re-apply
    // its state-dependent style instead of resetting it to the default box.
    QString m_infoBarMessage;
    bool m_infoBarIsError = false;
    // The empty-state hero in the transcript; kept so a theme change can
    // re-style it (it is deleted once the first message arrives).
    QPointer<QWidget> m_welcomeWidget;
    // Set when the theme changed mid-turn: the transcript rebuild that would
    // repaint finished messages is deferred until the turn ends.
    bool m_themeRefreshPending = false;
    QLabel *m_threadTitle = nullptr;
    QLabel *m_tokenCount = nullptr;
    QPushButton *m_modelSelector = nullptr;
    QString m_streamText;
    QHash<Provider, QStringList> m_modelCatalog;
    Provider m_preferredProvider = Provider::Grok;
    bool m_updatingCombos = false;
    QString m_modelFilter;
    QString m_acpAgentModelViewAgentId;
    QString m_acpAgentModelViewAgentName;
    QString m_acpModelsLoading;
    QPointer<AcpClient> m_acpModelProbe;
    // Custom popup rather than QMenu: a QLineEdit inside QMenu::exec() plus
    // setVisible() on hundreds of model QActions is what made typing hang, and
    // exec() below the chip opened the list off the bottom of the panel.
    QPointer<QWidget> m_modelPopup;
    QPointer<QListWidget> m_modelResultList;
    QPointer<QListWidget> m_acpAgentResultList;
    QPointer<QTabWidget> m_modelPickerTabs;
    QPointer<QLineEdit> m_modelFilterEdit;
    int m_modelMenuSelection = -1;

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
    QList<QPointer<ToolCallWidget>> m_toolCallOrder;
    // One live card per running sub-agent, keyed by task id.
    QHash<QString, SubtaskWidget *> m_subtaskWidgets;
    QList<QPointer<SubtaskWidget>> m_subtaskOrder;
    QPointer<QPushButton> m_scrollToBottomBtn;
    QPointer<QPushButton> m_activeAssistantCopyBtn;
    QPointer<QLabel> m_activeAssistantPulse;
    bool m_userScrolledUp = true;
    // While a disclosure (tool card, thinking block) is opening, keep this
    // widget's top edge glued to the viewport so the body grows downward
    // instead of the follow-tail scroller yanking the header up.
    QPointer<QWidget> m_pinnedViewportWidget;
    int m_pinnedViewportOffset = 0;
    QTimer *m_streamHeightTimer = nullptr;
    QTimer *m_streamRenderTimer = nullptr;

    struct ThinkingBlockRef {
        QPointer<QWidget> block;
        QPointer<QTextBrowser> browser;
        QPointer<QPushButton> toggle;
    };
    QList<ThinkingBlockRef> m_thinkingBlocks;

    // Dynamic status indicators at bottom of chat
    QPointer<QWidget> m_indicatorsRow;
    QPointer<QLabel> m_thinkingIndicator;
    QPointer<QLabel> m_workingIndicator;
    QTimer *m_indicatorTimer = nullptr;
    int m_indicatorTick = 0;
    QString m_workingLabelBase;
    bool m_isThinking = false;
    bool m_isWorking = false;
    bool m_isStreaming = false;

    // Edit tracker for AcceptEdits permission mode
    EditTracker *m_editTracker = nullptr;

    // Snapshot of one in-flight write/edit tool call, taken at toolStarted before
    // the tool ran. Kept as a struct rather than a PermissionRequest because the
    // tracker needs facts a request does not carry: whether the file existed at
    // all, and the tool's own name rather than one borrowed from `details`.
    struct PendingEdit {
        QString path;
        QString toolName;
        QString diff;
        QString oldContent;
        bool existed = false;
    };
    QHash<QString, PendingEdit> m_pendingToolCalls;

    // Suggestions the suggest tool has started but not finished, plus the live
    // cards and their order in the transcript.
    QHash<QString, Suggestion> m_pendingSuggestions;
    QHash<QString, SuggestionWidget *> m_suggestionWidgets;
    QList<SuggestionWidget *> m_suggestionOrder;

    // Live question cards, keyed by the tool call id that asked them.
    QHash<QString, QuestionWidget *> m_questionWidgets;
    QList<QPointer<QuestionWidget>> m_questionOrder;
    // Latest live output per running tool, flushed on a timer so a chatty
    // command does not re-render its card once per chunk.
    QHash<QString, QString> m_pendingProgress;
    QTimer *m_progressTimer = nullptr;

    // Conversation history
    QPushButton *m_historyButton = nullptr;
    // The number of conversations the history button shows. Negative means
    // "not known yet" and makes the next update read it from the store.
    int m_historyCount = -1;
    QMenu *m_historyMenu = nullptr;
    QString m_currentConversationId;
    bool m_loadingConversation = false;
    QTimer *m_sessionSaveTimer = nullptr;

    // Global override of the tool-card budget. Auto means the budget decides;
    // the other two are a deliberate "all of them" / "none of them".
    enum class ToolCardsOverride {
        Auto,
        ExpandAll,
        CollapseAll,
    };
    ToolCardsOverride m_toolCardsOverride = ToolCardsOverride::Auto;
    int m_focusedToolCardIndex = -1;
    QPushButton *m_expandAllBtn = nullptr;
};

} // namespace KateAi
