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
class SubtaskDock;
class SuggestionWidget;
class QuestionWidget;
class TodoWidget;
class InspectorPanel;
class EditTracker;
class TurnStatus;
class AcpBinaryInstaller;
class VoiceInput;

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

    // The review queue of agent edits, for callers (the plugin view) that
    // mirror its state into the editor: banners and hover hints.
    EditTracker *editTracker() const
    {
        return m_editTracker;
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
    bool forkConversation();
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
    // The conversation this chat is showing; the view reports it to the
    // plugin so the session config can restore it per window.
    QString conversationId() const
    {
        return m_currentConversationId;
    }

    // --- Editor review queue --------------------------------------------------
    // Re-points pending agent edits at the file's new path after a Save-As.
    void renamePendingEdits(const QString &oldPath, const QString &newPath);
    // Rejects every still-pending agent edit of the path (the in-editor
    // banner's action). Returns false when nothing was pending.
    bool rejectPendingEdits(const QString &path);

    PromptEdit *promptEdit() const { return m_prompt; }
    void setCompletionWords(const QStringList &words);

    // Voice prompting: microphone availability check, capture, noise removal,
    // transcription, and sending to chat.
    VoiceInput *voiceInput() const { return m_voiceInput; }
    bool isMicrophoneAvailable() const;
    void refreshMicrophoneAvailability();
    void toggleVoicePrompt();
    void cancelVoicePrompt();

    // Announces that the Kate AI workspace switched (another folder was
    // opened in Kate). The notice is pinned to the top of the composer.
    void showWorkspaceNotice(const QString &workspace);
    // Shows a transient notice (a retry, a failed restore) in the strip above
    // the composer. Errors stay until dismissed.
    void showInfoMessage(const QString &message, bool isError);
    // Plain-text reports opened by ":kateai context" and ":kateai usage".
    void showContextReport();
    void showUsageReport();
    void showPromptReport();
    void showMemoryReport();
    void showToolsReport();
    void showSkillsReport();
    void showInspector();
    void showVerificationReport();
    void showPolicyReport();
    void showHooksReport();
    void showTraceReport();

    // Modes / MCP / checkpoints, exposed so the view can drive them too.
    void showModeMenu();
    void showMcpMenu();
    void showTeamMenu();
    void updateTeamButton();
    // The running-state style of the agent-team button: a colour the flash
    // timer cycles through while sub-agents run; the header icon style when
    // idle. Split from updateTeamButton() so a flash tick repaints only.
    void styleTeamButton();
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
    void conversationWorkspaceRequested(const QString &workspace);
    // An agent edit of this path entered the review queue. The view turns it
    // into an in-editor banner on the open document.
    void agentEditedFile(const QString &path);
    // The review queue went from empty to non-empty or back.
    void pendingEditsChanged(bool hasPending);
    // A suggestion card was accepted and carries a file anchor. The view opens
    // the file at that line; Kate core only owns the card.
    void openFileAtLineRequested(const QString &path, int line);

protected:
    void resizeEvent(QResizeEvent *event) override;
    // Watches the reasoning browsers: their pinned height has to follow the
    // width the layout finally gives them, and that width arrives as a resize.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void resetForNewChat(bool saveCurrentConversation);
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
    void showTodoList(const QJsonArray &plan);
    // Re-fits a reasoning browser's pinned height to its document at the
    // browser's real width. A fit measured before the block was laid out
    // wraps the text at a stale width, which shows up as empty space below
    // a short block (or clipped lines below a long one).
    void fitReasoningBrowser(QTextBrowser *browser);
    // The bottom-most content widget of the transcript, above the slack: the
    // anchor a retiring docked card is inserted past.
    QWidget *lastTranscriptContentWidget() const;
    void scrollToBottom();
    void forceScrollToBottom();
    void pinWidgetInViewport(QWidget *widget);
    void applyPinnedViewport();
    void updateScrollButtonPosition();
    void updateIndicatorsPosition();
    void syncIndicatorsOverlay();
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
    bool ensureAcpBinaryInstalled(const QString &agentId);
    Settings settingsForAcpAgent(const QString &agentId) const;
    void updateModelSelectorLabel();
    void applyInfoBarStyle();
    void applyWorkspaceBannerStyle();
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
    // Moves a docked card back into the transcript. `anchor` is the transcript
    // card that was last when the docked operation began: the card is inserted
    // right past it so the transcript keeps its time order instead of dropping
    // a long-running operation below everything that happened after it.
    void retireIntentWidget(QWidget *widget, QWidget *anchor = nullptr);
    // Re-applies the quarter-of-the-panel height cap the docked sub-agent cards
    // live under. Called whenever the panel is resized and whenever a card is
    // docked, so a card never inherits a stale budget.
    void applySubtaskHeightBudget();
    // Approval cards are serialized: the first request is shown, later ones
    // wait in m_queuedPermissionRequests until the visible decision is made.
    void enqueuePermissionRequest(const PermissionRequest &request);
    void presentPermissionRequest(const PermissionRequest &request);
    void showNextQueuedPermission();
    void retirePermissionRequest(const QString &toolCallId);
    // Wires Apply/Discard for one sub-agent card. Used by both the live card
    // and a card rebuilt from saved history, so a restored worktree patch can
    // still be applied or discarded.
    void wireSubtaskWorktreeActions(SubtaskWidget *subtaskWidget);
    void restoreSubtaskWorktree(SubtaskWidget *subtaskWidget, const SubagentRecord &record);

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
    // Retires one docked question card, e.g. when a newer question replaced the
    // parked RPC or the turn stopped.
    void removeQuestionCard(const QString &toolCallId);
    // `keepId` is an agent-side ACP question that remains answerable after the
    // turn ends; every other card is retired.
    void clearQuestionCards(const QString &keepId = QString());

    void submit();
        // True while a tool card is showing its inline approval row.
        bool m_approvalPending() const;
    void applyProviderToCombos();
    void refreshProviders();
    void refreshModels();
    void updateSendButtonState();
    void updateVoicePromptUi();
    void updateTokenDisplay();
    void updateThinkingButtonStyle();
    void updateReasoningEffortButton();
    bool modelSupportsReasoningEffort() const;
    void showReasoningEffortMenu();
    void showAcpEffortMenu();
    void setThinkingIndicator(bool show);
    void setWorkingIndicator(bool show);
    void tickIndicators();
    void syncIndicatorAnimation();
    bool registerThinkingBlock(QWidget *block, QTextBrowser *browser, QPushButton *toggle);
    void applyTranscriptCollapse();
    // Drop the oldest finished tool cards once a long session keeps more than
    // the retention cap. Restored history can rebuild them from the session.
    void pruneOldTranscriptCards();
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
    void fitUserMessageBody(QTextBrowser *browser, int availableWidth = 0);
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
    QPushButton *m_modeButton = nullptr;
    QPushButton *m_permissionButton = nullptr;
    QPushButton *m_mcpButton = nullptr;
    QPushButton *m_checkpointButton = nullptr;
        QPushButton *m_teamButton = nullptr;
        // Colour cycle for the agent-team indicator while sub-agents run.
        QTimer *m_teamFlashTimer = nullptr;
        int m_teamFlashTick = 0;

    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_transcriptContainer = nullptr;
    QVBoxLayout *m_transcriptLayout = nullptr;
    QPointer<QWidget> m_activeAssistantWidget;
    QPointer<QTextBrowser> m_activeAssistantBrowser;

    PermissionBar *m_permissionBar = nullptr;
    // Intent dock: sits between the transcript and the permission bar.
    QWidget *m_intentDock = nullptr;
    QVBoxLayout *m_intentDockLayout = nullptr;
    // The dock's sub-agent slot: tabs over the one live sub-agent card, so
    // concurrent delegations share a single bounded strip instead of stacking.
    SubtaskDock *m_subtaskDock = nullptr;
    PromptEdit *m_prompt = nullptr;
    VoiceInput *m_voiceInput = nullptr;
    QTimer *m_micRefreshTimer = nullptr;
    TurnStatus *m_turnStatus = nullptr;
    // Tool calls completed in the current turn, shown live in the status strip.
    int m_completedToolCount = 0;
    QLabel *m_infoBar = nullptr;
    // Workspace switch notice, pinned above the info bar in the composer so
    // "Kate now works in this folder" is seen where the user is typing.
    QLabel *m_workspaceBanner = nullptr;
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
    QString m_acpModelCatalogAgentId;
    Provider m_preferredProvider = Provider::Grok;
    bool m_updatingCombos = false;
    QString m_modelFilter;
    QString m_acpAgentModelViewAgentId;
    QString m_acpAgentModelViewAgentName;
    QString m_acpModelsLoading;
    // Agent whose slash commands the composer is showing. Lets a settings
    // change keep a live list (the cache may predate it) while an agent switch
    // still clears the outgoing agent's commands.
    QString m_acpCommandsAgentId;
    // Effort/thought levels advertised by the live ACP agent, plus the level
    // it last reported as selected. Drives the composer effort button.
    QStringList m_acpEfforts;
    QString m_acpCurrentEffort;
    QPointer<AcpClient> m_acpModelProbe;
    QPointer<AcpBinaryInstaller> m_acpBinaryInstaller;
    QString m_acpInstallingAgentId;
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
    // Reasoning already rendered into a card frozen earlier in the same turn
    // (a tool freeze splits one turn into several cards). The loop re-reports
    // the full turn's thinking when the answer text starts; this prefix is
    // what must not be drawn a second time in the later card.
    QString m_thinkingShownEarlierInTurn;
    // True once the visible answer has started streaming in the current card.
    // Some providers keep sending reasoning after the first text delta; those
    // later deltas are buffered but not paced in, so the reasoning and the
    // answer are never written into the chat at the same time.
    bool m_answerStarted = false;
    int m_thinkingPacedLength = 0;
    QTimer *m_thinkingPacerTimer = nullptr;

    // The todo list of the turn, docked at the bottom of the chat above the
    // composer: the plan stays in view while the transcript scrolls. The
    // widget outlives turns; it hides itself when no plan is set.
    QPointer<TodoWidget> m_todoWidget;

    QHash<QString, ToolCallWidget *> m_toolCallWidgets;
    QList<QPointer<ToolCallWidget>> m_toolCallOrder;
    // One live card per running sub-agent, keyed by task id.
    QHash<QString, SubtaskWidget *> m_subtaskWidgets;
    QList<QPointer<SubtaskWidget>> m_subtaskOrder;
    // The transcript card that was last when each sub-agent started. The
    // sub-agent's card retires right past it, so a delegation that began
    // early in the turn is not glued to the bottom of the transcript when it
    // finishes or is stopped.
    QHash<QString, QPointer<QWidget>> m_subtaskAnchors;
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
    // The question each question tool asked, keyed by the tool call id. The
    // card shows it while the question is open; when the tool finishes the
    // answer takes the body, so the question is folded back in.
    QHash<QString, QString> m_questionTexts;
    // A child question's card id -> task id. Answer is routed to the child,
    // not to the parent's own parked question.
    QHash<QString, QString> m_subtaskQuestionTaskIds;
    // Only one approval strip is visible at a time. Concurrent children (or a
    // parent and a child) queue their requests instead of each new card
    // cancelling the previous one and leaving its owner parked forever.
    QList<PermissionRequest> m_queuedPermissionRequests;
    QString m_activePermissionCallId;
    QPointer<InspectorPanel> m_inspector;
    // Latest live output per running tool, flushed on a timer so a chatty
    // command does not re-render its card once per chunk.
    QHash<QString, QString> m_pendingProgress;
    QTimer *m_progressTimer = nullptr;
    // Activity notes are ephemeral status lines, not messages. Keep only the
    // recent ones so a long turn cannot accumulate one QLabel per event.
    QList<QPointer<QLabel>> m_activityPills;

    // Conversation history
    QPushButton *m_historyButton = nullptr;
    // The number of conversations the history button shows. Negative means
    // "not known yet" and makes the next update read it from the store.
    int m_historyCount = -1;
    QMenu *m_historyMenu = nullptr;
    QString m_currentConversationId;
    bool m_loadingConversation = false;
    // True while rebuildTranscript() is creating the history cards. The
    // readout update that can compact an over-full history must not run then:
    // it would replace the messages still being rendered.
    bool m_renderingTranscript = false;
    // The transcript card count at which pruneOldTranscriptCards() should next
    // run. Pruning on every append above the threshold rescanned the whole
    // transcript each time and made restoring a long conversation quadratic;
    // after a prune the next check waits for a batch of new cards.
    int m_nextTranscriptPrune = 512;
    QTimer *m_sessionSaveTimer = nullptr;
    bool m_sessionDirty = false;

    // Global override of the tool-card budget. Auto means the budget decides;
    // the other two are a deliberate "all of them" / "none of them".
    enum class ToolCardsOverride {
        Auto,
        ExpandAll,
        CollapseAll,
    };
    ToolCardsOverride m_toolCardsOverride = ToolCardsOverride::Auto;
    int m_focusedToolCardIndex = -1;
};

} // namespace KateAi
