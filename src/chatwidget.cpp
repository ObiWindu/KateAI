/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "chatwidget.h"

#include "agenttext.h"
#include "chattheme.h"
#include "contextmanager.h"
#include "permissionbar.h"
#include "promptedit.h"
#include "turnstatus.h"
#include "sessionstore.h"
#include "settings.h"
#include "toolcallwidget.h"
#include "subtaskwidget.h"
#include "edittracker.h"
#include "tools.h"
#include "transcriptlayout.h"
#include "checkpoint.h"
#include "mcp.h"
#include "modes.h"

#include <KLocalizedString>

#include <QAction>
#include <QActionGroup>
#include <QWidgetAction>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QCursor>
#include <QFontMetrics>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScrollArea>
#include <QTextBrowser>
#include <QTextDocument>
#include <QJsonDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace {

QString progressiveDots(int tick)
{
    const int n = (tick % 3) + 1;
    return QString(n, u'.') + QString(3 - n, QChar(0x2007));
}

void attachPulseEffect(QLabel *label)
{
    if (!label || label->graphicsEffect()) {
        return;
    }
    auto *effect = new QGraphicsOpacityEffect(label);
    effect->setOpacity(1.0);
    label->setGraphicsEffect(effect);
}

// Tools whose effects land in a file, and therefore the ones the edit tracker
// needs to know about. The mutating-tool list was open-coded in four separate
// places (working label, tool tracking, transcript rebuild, risk colouring) and
// had already drifted: an unlisted mutator meant a silent gap in the review
// queue.
bool isFileMutatingTool(const QString &toolName)
{
    return toolName == u"write_file"_s || toolName == u"edit_file"_s || toolName == u"multi_edit_file"_s
        || toolName == u"multi_replace_file_content"_s;
}

QString workingLabelForTool(const QString &toolName)
{
    // Plain verbs, no emoji: this line re-renders on every tool call and sits
    // at the very bottom of the transcript, where pictograms read as noise.
    if (isFileMutatingTool(toolName)) {
        return i18n("Editing");
    }
    if (toolName == u"read_file"_s) {
        return i18n("Reading");
    }
    if (toolName == u"grep"_s || toolName == u"glob"_s) {
        return i18n("Searching");
    }
    if (toolName == u"list_dir"_s) {
        return i18n("Listing");
    }
    if (toolName == u"bash"_s) {
        return i18n("Running");
    }
    return i18n("Working");
}

} // namespace

namespace KateAi
{

ChatWidget::ChatWidget(QWidget *parent)
    : QWidget(parent)
    // Start pinned to the bottom. The scrollbar only reports valueChanged once
    // it actually moves, so a `true` here survived until the user's first scroll
    // and made the opening tool cards and thinking blocks of the very first turn
    // show a "jump to latest" button instead of following along.
    , m_userScrolledUp(false)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 1. Header. Deliberately thin: a title, then the actions that apply to the
    // whole thread. The controls that shape the next turn (mode, model,
    // reasoning effort) live with the composer instead, next to the input.
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(u"headerBar"_s);
    auto *toolbarLayout = new QHBoxLayout(m_toolbar);
    toolbarLayout->setContentsMargins(12, 8, 12, 8);
    toolbarLayout->setSpacing(6);

    m_threadTitle = new QLabel(i18n("New Thread"), this);
    m_threadTitle->setStyleSheet(ChatTheme::sectionLabel());
    toolbarLayout->addWidget(m_threadTitle);

    toolbarLayout->addStretch();

    // Unified Model Selector button. Created here so its menu is wired in one
    // place, but added to the composer further down.
    m_modelSelector = new QPushButton(this);
    m_modelSelector->setCursor(Qt::PointingHandCursor);
    m_modelSelector->setStyleSheet(ChatTheme::chipButton());
    m_modelSelector->setFixedHeight(26);
    m_modelSelector->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    // Long provider or model names must not push the send button off the row.
    m_modelSelector->setMaximumWidth(190);

        // Mode selector — Code / Ask / Architect / Debug / Orchestrator + custom.
        m_modeButton = new QPushButton(this);
            m_modeButton->setCursor(Qt::PointingHandCursor);
            m_modeButton->setToolTip(i18n("Agent mode"));
            m_modeButton->setStyleSheet(ChatTheme::chipButton());
                // A fixed height is what makes the pill radius land exactly on the
                // cap height; left to the layout the chips ended up subtly squared.
                m_modeButton->setFixedHeight(26);
                connect(m_modeButton, &QPushButton::clicked, this, &ChatWidget::showModeMenu);

        // Permission mode. The edit tracker that follows a file change only exists
        // in "Accept edits", so this has to be reachable from the composer: buried
        // two levels down in the settings menu it read as a feature that had
        // stopped working.
        m_permissionButton = new QPushButton(this);
        m_permissionButton->setCursor(Qt::PointingHandCursor);
        m_permissionButton->setStyleSheet(ChatTheme::chipButton());
        m_permissionButton->setFixedHeight(26);
        m_permissionButton->setToolTip(i18n("Permission mode"));
        connect(m_permissionButton, &QPushButton::clicked, this, &ChatWidget::showPermissionMenu);
        refreshPermissionButton();

            // MCP servers status button.
        m_mcpButton = new QPushButton(this);
        m_mcpButton->setCursor(Qt::PointingHandCursor);
        m_mcpButton->setFixedSize(28, 28);
        m_mcpButton->setToolTip(i18n("MCP servers"));
        m_mcpButton->setStyleSheet(ChatTheme::iconButton());
        connect(m_mcpButton, &QPushButton::clicked, this, &ChatWidget::showMcpMenu);
        toolbarLayout->addWidget(m_mcpButton);

        // Checkpoints button.
        m_checkpointButton = new QPushButton(QIcon::fromTheme(u"document-save"_s), QString(), this);
        m_checkpointButton->setCursor(Qt::PointingHandCursor);
        m_checkpointButton->setFixedSize(28, 28);
        m_checkpointButton->setToolTip(i18n("Checkpoints"));
        m_checkpointButton->setStyleSheet(ChatTheme::iconButton());
        connect(m_checkpointButton, &QPushButton::clicked, this, &ChatWidget::showCheckpointMenu);
        toolbarLayout->addWidget(m_checkpointButton);

        // Agent team: shows how many sub-agents are running right now.
        m_teamButton = new QPushButton(this);
        m_teamButton->setCursor(Qt::PointingHandCursor);
        m_teamButton->setToolTip(i18n("Agent team"));
        m_teamButton->setStyleSheet(ChatTheme::iconButton());
        connect(m_teamButton, &QPushButton::clicked, this, &ChatWidget::showTeamMenu);
        toolbarLayout->addWidget(m_teamButton);

    // Reasoning effort chooser button — sits right next to the model label
    // in the chat input area so the user can pick an effort level at a glance.
    m_reasoningEffort = new QPushButton(this);
    // Height only: a fixed width clipped the label ("Auto" rendered as "Au").
    m_reasoningEffort->setFixedHeight(26);
    m_reasoningEffort->setCursor(Qt::PointingHandCursor);
    m_reasoningEffort->setToolTip(i18n("Reasoning effort"));
    m_reasoningEffort->setVisible(true);
    connect(m_reasoningEffort, &QPushButton::clicked, this, &ChatWidget::showReasoningEffortMenu);

    // Conversation History button
    m_historyButton = new QPushButton(QIcon::fromTheme(u"view-history"_s), QString(), this);
    m_historyButton->setToolTip(i18n("Conversation History"));
    m_historyButton->setFixedSize(28, 28);
    m_historyButton->setCursor(Qt::PointingHandCursor);
    m_historyButton->setStyleSheet(ChatTheme::iconButton());
    connect(m_historyButton, &QPushButton::clicked, this, &ChatWidget::showConversationHistory);
    toolbarLayout->addWidget(m_historyButton);

    // New Chat button
    m_newChat = new QPushButton(QIcon::fromTheme(u"list-add"_s), QString(), this);
    m_newChat->setToolTip(i18n("New Thread"));
    m_newChat->setFixedSize(28, 28);
    m_newChat->setCursor(Qt::PointingHandCursor);
    m_newChat->setStyleSheet(ChatTheme::iconButton());
    toolbarLayout->addWidget(m_newChat);

    // Settings / Configure button
    m_configure = new QPushButton(QIcon::fromTheme(u"settings-configure"_s), QString(), this);
    m_configure->setToolTip(i18n("Settings"));
    m_configure->setFixedSize(28, 28);
    m_configure->setCursor(Qt::PointingHandCursor);
    m_configure->setStyleSheet(ChatTheme::iconButton());
    toolbarLayout->addWidget(m_configure);

    root->addWidget(m_toolbar);

    // Hidden controls retained for internal logic & backward compatibility
    m_provider = new QComboBox(this);
    m_provider->setVisible(false);
    m_model = new QComboBox(this);
    m_model->setEditable(true);
    m_model->setInsertPolicy(QComboBox::NoInsert);
    m_model->setVisible(false);

    m_permission = new QComboBox(this);
    m_permission->setVisible(false);
    m_permission->addItem(permissionModeLabel(PermissionMode::Ask), permissionModeId(PermissionMode::Ask));
    m_permission->addItem(permissionModeLabel(PermissionMode::AcceptEdits), permissionModeId(PermissionMode::AcceptEdits));
    m_permission->addItem(permissionModeLabel(PermissionMode::AlwaysApprove), permissionModeId(PermissionMode::AlwaysApprove));

    m_sandbox = new QComboBox(this);
    m_sandbox->setVisible(false);
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Workspace), sandboxProfileId(SandboxProfile::Workspace));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::ReadOnly), sandboxProfileId(SandboxProfile::ReadOnly));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Strict), sandboxProfileId(SandboxProfile::Strict));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Off), sandboxProfileId(SandboxProfile::Off));

    m_mode = new QComboBox(this);
    m_mode->setVisible(false);
    populateModes();

    m_thinking = new QPushButton(this);
    m_thinking->setCheckable(true);

    m_stop = new QPushButton(this);
    m_stop->setVisible(false);
    m_stop->setEnabled(false);

    // 2. Zed-style Transcript Area (Scroll Area with Cards & Tool Widgets)
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet(ChatTheme::scrollArea());

    m_transcriptContainer = new QWidget(m_scrollArea);
    m_transcriptContainer->setObjectName(u"transcript"_s);
    m_transcriptContainer->setStyleSheet(ChatTheme::transcript());
    m_transcriptLayout = new QVBoxLayout(m_transcriptContainer);
    m_transcriptLayout->setContentsMargins(14, 14, 14, 14);
    m_transcriptLayout->setSpacing(10);
    m_transcriptLayout->setAlignment(Qt::AlignTop);
    m_scrollArea->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    // Leading stretch: the transcript is bottom-anchored, so when the
    // conversation is shorter than the viewport the slack is absorbed above the
    // content. This keeps the newest message pinned to the composer instead of
    // stranding it at the top of the viewport behind an empty gap.
    m_transcriptLayout->addStretch();

    // Initial empty state welcome widget
    m_transcriptLayout->addWidget(createWelcomeWidget());

    // Dynamic status indicators (thinking/working) - always at bottom of transcript
    m_indicatorsRow = createIndicatorsRow();
    m_transcriptLayout->addWidget(m_indicatorsRow);
    m_scrollArea->setWidget(m_transcriptContainer);
    root->addWidget(m_scrollArea, 1);

    // Bottom-anchored: start pinned to the newest content, not the oldest.
    QTimer::singleShot(0, this, [thisWeak = QPointer<ChatWidget>(this)]() {
        if (thisWeak) {
            thisWeak->forceScrollToBottom();
        }
    });

    m_scrollToBottomBtn = new QPushButton(QIcon::fromTheme(u"go-down"_s), i18n("Jump to latest"), m_scrollArea);
    m_scrollToBottomBtn->setCursor(Qt::PointingHandCursor);
    m_scrollToBottomBtn->setStyleSheet(ChatTheme::jumpToLatest());
    m_scrollToBottomBtn->hide();
    connect(m_scrollToBottomBtn, &QPushButton::clicked, this, &ChatWidget::forceScrollToBottom);

    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        auto *sb = m_scrollArea->verticalScrollBar();
        const bool following = transcriptShouldFollowTail(value, sb->maximum());
        if (following) {
            m_userScrolledUp = false;
            if (m_scrollToBottomBtn && m_scrollToBottomBtn->isVisible()) {
                animateScrollButtonHide();
            }
        } else {
            m_userScrolledUp = true;
        }
    });

    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int min, int max) {
        Q_UNUSED(min);
        auto *sb = m_scrollArea->verticalScrollBar();
        if (!sb) {
            return;
        }
        if (!m_userScrolledUp) {
            sb->setValue(max);
        } else if (!transcriptShouldFollowTail(sb->value(), max)) {
            if (m_scrollToBottomBtn) {
                animateScrollButtonShow();
                m_scrollToBottomBtn->raise();
            }
        } else if (m_scrollToBottomBtn && m_scrollToBottomBtn->isVisible()) {
            // The tail grew back into view under the user (a collapsed card, a
            // narrower panel). Nothing is hidden any more, so retire the button.
            m_userScrolledUp = false;
            animateScrollButtonHide();
        }
    });

    // 3. Intent dock. Everything that requires the user to press something --
    // sub-agent cards with their Cancel, and the Allow / Deny strip -- is
    // pinned here, directly above the input. In the transcript these scrolled
    // away from the controls and left the agent apparently stuck while the
    // decision sat off-screen.
    m_intentDock = new QWidget(this);
    m_intentDock->setObjectName(u"intentDock"_s);
    m_intentDock->setStyleSheet(ChatTheme::intentDock());
    m_intentDockLayout = new QVBoxLayout(m_intentDock);
    m_intentDockLayout->setContentsMargins(10, 6, 10, 0);
    m_intentDockLayout->setSpacing(6);
    m_intentDock->hide();
    root->addWidget(m_intentDock);

    // 4. Permission Bar (Zed-style Inline Consent)
    m_permissionBar = new PermissionBar(this);
    root->addWidget(m_permissionBar);

    // 4b. Edit Tracker (for AcceptEdits permission mode) - compact bar at bottom of chat
    m_editTracker = new EditTracker(this);
    root->addWidget(m_editTracker);

    // 5. Composer
    auto *composerContainer = new QWidget(this);
    composerContainer->setObjectName(u"composerContainer"_s);
    composerContainer->setStyleSheet(
        QStringLiteral("QWidget#composerContainer { background-color: %1; border-top: 1px solid %2; }")
            .arg(ChatTheme::panelBg(), ChatTheme::border()));
    auto *composerLayout = new QVBoxLayout(composerContainer);
    composerLayout->setContentsMargins(12, 10, 12, 10);
    composerLayout->setSpacing(6);

    // Info bar for API messages (retries, errors) - shown above composer
    m_infoBar = new QLabel(composerContainer);
    m_infoBar->setWordWrap(true);
    m_infoBar->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_infoBar->setStyleSheet(ChatTheme::infoBar());
    m_infoBar->hide();
    composerLayout->addWidget(m_infoBar);

    auto *composerCard = new QWidget(composerContainer);
    composerCard->setObjectName(u"composerCard"_s);
    composerCard->setStyleSheet(ChatTheme::composerCard());
    auto *composerCardLayout = new QVBoxLayout(composerCard);
    composerCardLayout->setContentsMargins(10, 8, 8, 8);
    composerCardLayout->setSpacing(2);

    m_prompt = new PromptEdit(composerCard);
    m_prompt->setStyleSheet(ChatTheme::promptInput());
    composerCardLayout->addWidget(m_prompt);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(4, 2, 2, 0);
    bottomRow->setSpacing(6);

    // Mode and model live with the input rather than in the thread header.
    // They decide what happens to the text about to be typed, so they belong
    // next to it; up in the header they read as thread-wide settings.
    bottomRow->addWidget(m_modeButton);
    bottomRow->addWidget(m_permissionButton);
    bottomRow->addWidget(m_modelSelector);
    bottomRow->addWidget(m_reasoningEffort);
    // Thinking mode is a mode, not an action, so it groups with the other
    // settings on the left. Parked beside send it read as a second, competing
    // call to action.
    bottomRow->addWidget(m_thinking);

    bottomRow->addStretch();

    m_tokenCount = new QLabel(composerCard);
    m_tokenCount->setStyleSheet(ChatTheme::tokenLabel());
    m_tokenCount->hide();
    bottomRow->addWidget(m_tokenCount);

    // Thinking mode toggle button
    m_thinking->setParent(composerCard);
    m_thinking->setVisible(true);
    m_thinking->setCheckable(true);
    m_thinking->setFixedSize(26, 26);
    m_thinking->setCursor(Qt::PointingHandCursor);
    m_thinking->setToolTip(i18n("Toggle thinking mode"));
    updateThinkingButtonStyle();

    m_send = new QPushButton(composerCard);
    m_send->setFixedSize(30, 30);
    m_send->setCursor(Qt::PointingHandCursor);
    updateSendButtonState();
    bottomRow->addWidget(m_send);

    composerCardLayout->addLayout(bottomRow);
    composerLayout->addWidget(composerCard);

    // The status strip is now a live widget: keyboard hint while idle, elapsed
    // time and current activity while a turn runs.
    m_turnStatus = new TurnStatus(composerContainer);
    m_turnStatus->setIdleText(i18n("Enter to send · Shift+Enter for a new line"));
    m_turnStatus->setMinimumHeight(18);
    composerLayout->addWidget(m_turnStatus);
    connect(m_turnStatus, &TurnStatus::scrollToLatestRequested, this, &ChatWidget::forceScrollToBottom);

    root->addWidget(composerContainer);

    // Signal connections
    connect(m_prompt, &PromptEdit::submitRequested, this, &ChatWidget::submit);
    connect(m_prompt, &PromptEdit::escapePressed, this, [this]() {
        if (m_agent.isBusy()) {
            m_permissionBar->hideBar();
            m_agent.abort();
            updateSendButtonState();
        }
    });
    connect(m_prompt, &QPlainTextEdit::textChanged, this, [this]() {
        if (!m_agent.isBusy()) {
            updateSendButtonState();
        }
    });

    connect(m_send, &QPushButton::clicked, this, [this]() {
        if (m_agent.isBusy()) {
            m_permissionBar->hideBar();
            m_agent.abort();
            updateSendButtonState();
        } else {
            submit();
        }
    });

    connect(m_newChat, &QPushButton::clicked, this, &ChatWidget::newChat);
    connect(m_configure, &QPushButton::clicked, this, &ChatWidget::showSettingsMenu);
    connect(m_modelSelector, &QPushButton::clicked, this, &ChatWidget::showModelMenu);
    connect(m_permissionBar, &PermissionBar::decided, &m_agent, &AgentLoop::resolvePermission);

    connect(m_provider, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingCombos || m_provider->currentData().isNull()) {
            return;
        }
        m_settings.provider = providerFromId(m_provider->currentData().toString());
        m_preferredProvider = m_settings.provider;
        refreshModels();
        updateModelSelectorLabel();
        updateTokenDisplay();
        updateReasoningEffortButton();
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
    });

    connect(m_model, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (m_updatingCombos || text.trimmed().isEmpty()) {
            return;
        }
        switch (m_settings.provider) {
        case Provider::OpenAI:
            m_settings.openaiModel = text.trimmed();
            break;
        case Provider::OpenRouter:
            m_settings.openrouterModel = text.trimmed();
            break;
        case Provider::DeepSeek:
            m_settings.deepseekModel = text.trimmed();
            break;
        case Provider::OpenAICompatible:
            m_settings.openaiCompatibleModel = text.trimmed();
            break;
        case Provider::ClaudeCompatible:
            m_settings.claudeCompatibleModel = text.trimmed();
            break;
        case Provider::OpenCode:
            m_settings.opencodeModel = text.trimmed();
            break;
        case Provider::Acp:
            m_settings.acpModel = text.trimmed();
            break;
        case Provider::Grok:
        default:
            m_settings.grokModel = text.trimmed();
            break;
        }
        updateModelSelectorLabel();
        updateTokenDisplay();
        updateReasoningEffortButton();
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
    });

    connect(m_permission, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingCombos) return;
        m_settings.permissionMode = permissionModeFromId(m_permission->currentData().toString());
        m_agent.setSettings(m_settings);
        refreshPermissionButton();
        Q_EMIT settingsChanged(m_settings);
    });

    connect(m_sandbox, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingCombos) return;
        m_settings.sandbox = sandboxProfileFromId(m_sandbox->currentData().toString());
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
    });

    connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingCombos) return;
        applyMode(m_mode->currentData().toString());
    });

    connect(m_thinking, &QPushButton::toggled, this, [this](bool checked) {
        m_settings.thinkingMode = checked;
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
        updateThinkingButtonStyle();
    });

    // Agent signals
    connect(&m_agent, &AgentLoop::userMessage, this, &ChatWidget::addUserMessage);
    connect(&m_agent, &AgentLoop::thinkingDelta, this, [this](const QString &delta) {
        if (!m_activeAssistantWidget) {
            setStreaming(m_streamText);
        }
        if (m_thinkingBrowser) {
            appendThinkingDelta(delta);
            scrollToBottom();
        }
        setThinkingIndicator(true);
    });
    connect(&m_agent, &AgentLoop::thinkingFinished, this, [this](const QString &text) {
        // Stop thinking indicator BEFORE adding thinking block so that
        // addThinkingBlock sees m_isThinking == false and flushes the pacer
        // instead of starting a new pacing animation.
        setThinkingIndicator(false);
        addThinkingBlock(text);
    });
    connect(&m_agent, &AgentLoop::planUpdated, this, &ChatWidget::addPlanChecklist);
    connect(&m_agent, &AgentLoop::assistantDelta, this, [this](const QString &delta) {
        // Auto-collapse thinking when visible answer starts streaming only if configured
        if (m_settings.autoCollapseThinking && m_thinkingExpanded && !m_streamText.isEmpty()) {
            collapseThinkingBlock();
        }
        flushThinkingPacer();
        setThinkingIndicator(false);
        setStreaming(m_streamText + delta);
    });
    connect(&m_agent, &AgentLoop::assistantFinished, this, [this](const QString &text) {
        Q_UNUSED(text);
        freezeStreaming();
        setWorkingIndicator(false);
    });
    connect(&m_agent, &AgentLoop::activityUpdated, this, &ChatWidget::addActivityMessage);

    // Tool visibility signals (Zed-style inline tool-call cards)
    connect(&m_agent, &AgentLoop::toolStarted, this, [this](const PermissionRequest &request) {
        freezeStreaming();
        m_workingLabelBase = workingLabelForTool(request.toolName);
        setWorkingIndicator(true);
        m_turnStatus->setActivity(m_workingLabelBase);
        if (m_workingIndicator && m_isWorking) {
            m_workingIndicator->setText(m_workingLabelBase + progressiveDots(m_indicatorTick));
        }

        // A sub-agent is a long-running operation with its own live status, so
        // it gets a dedicated card instead of the flat tool-call card. The card
        // carries a Cancel button, which makes it something the user may need
        // to act on, so it lives in the intent dock above the input rather than
        // down in the transcript.
        if (request.toolName == subtaskToolName()) {
            auto *subtaskWidget = new SubtaskWidget(request.toolCallId, m_intentDock);
            connect(subtaskWidget, &SubtaskWidget::cancelRequested, this, [this](const QString &taskId) {
                m_agent.cancelSubtask(taskId);
            });
            m_subtaskWidgets.insert(request.toolCallId, subtaskWidget);
            m_subtaskOrder.append(subtaskWidget);
            addIntentWidget(subtaskWidget);
            return;
        }

        auto *toolWidget = new ToolCallWidget(request.toolCallId, m_transcriptContainer);
        toolWidget->setToolInfo(request.toolName, request.summary, request.risk);
        toolWidget->setDescribeDiff(request.describeDiff);
        toolWidget->setDurationVisible(true);
        toolWidget->setRunning();
        m_toolCallWidgets.insert(request.toolCallId, toolWidget);
        m_toolCallOrder.append(toolWidget);
        appendTranscriptWidget(toolWidget);
        applyTranscriptCollapse();

        // Track write/edit tool calls for edit tracking in AcceptEdits mode.
        // The pre-edit snapshot has to be taken here, at toolStarted, because
        // that is the last moment before the tool runs: by toolFinished the file
        // on disk already holds the new content.
        if (isFileMutatingTool(request.toolName) && m_settings.permissionMode == PermissionMode::AcceptEdits) {
            PendingEdit pending;
            pending.toolName = request.toolName;
            pending.path = request.path;
            pending.diff = request.describeDiff;
            // existed == false means write_file just created the file, which is
            // the one case where rejecting has to delete rather than restore.
            pending.existed = m_agent.documentBridge()
                && m_agent.documentBridge()->readDocument(request.path, &pending.oldContent);
            m_pendingToolCalls.insert(request.toolCallId, pending);
        }

        scrollToBottom();
    });

    connect(&m_agent, &AgentLoop::toolFinished, this, [this](const ToolResult &result) {
        if (auto *subtaskWidget = m_subtaskWidgets.value(result.toolCallId)) {
            subtaskWidget->finishAgent(result);
            // Done: the card no longer needs the user's attention, so hand it
            // back to the transcript instead of letting it sit above the input.
            retireIntentWidget(subtaskWidget);
        }
        if (auto *widget = m_toolCallWidgets.value(result.toolCallId)) {
            widget->setFinished(result);
        }
        ++m_completedToolCount;
        m_turnStatus->setCompletedToolCount(m_completedToolCount);

        // Handle edit tracking for AcceptEdits mode. A tool that did not succeed
        // must not enter the queue: there is nothing to keep or revert, and
        // showing a "pending" row for a failed write invited the user to approve
        // a change that never landed.
        if (m_settings.permissionMode == PermissionMode::AcceptEdits) {
            auto it = m_pendingToolCalls.find(result.toolCallId);
            if (it != m_pendingToolCalls.end()) {
                const PendingEdit pending = it.value();
                if (result.ok) {
                    QString newContent;
                    if (m_agent.documentBridge()) {
                        m_agent.documentBridge()->readDocument(pending.path, &newContent);
                    }
                    m_editTracker->addEdit(pending.path, pending.toolName, pending.diff,
                                           pending.oldContent, newContent, !pending.existed);
                }
                m_pendingToolCalls.erase(it);
            }
        }

        // Hide working indicator if no more tools are running
        bool anyRunning = false;
        for (auto *widget : m_toolCallWidgets) {
            if (widget && widget->isRunning()) {
                anyRunning = true;
                break;
            }
        }
        if (!anyRunning) {
            setWorkingIndicator(false);
        }
        applyTranscriptCollapse();
        scrollToBottom();
    });

    // Approval now happens on the card that triggered it. permissionNeeded fires
    // before toolStarted, so the card is created here and later reused by the
    // toolStarted handler instead of a second card appearing.
    connect(&m_agent, &AgentLoop::permissionNeeded, this, [this](const PermissionRequest &request) {
        ToolCallWidget *widget = m_toolCallWidgets.value(request.toolCallId);
        if (!widget) {
            widget = new ToolCallWidget(request.toolCallId, m_transcriptContainer);
            widget->setToolInfo(request.toolName, request.summary, request.risk);
            widget->setDescribeDiff(request.describeDiff);
            widget->setDurationVisible(true);
            m_toolCallWidgets.insert(request.toolCallId, widget);
            m_toolCallOrder.append(widget);
            appendTranscriptWidget(widget);
            applyTranscriptCollapse();
        }
        widget->showApproval();
        // Lift the Allow / Deny strip out of the card and into the dock, so the
        // decision is always on screen next to the input. The card stays
        // expanded underneath, so the diff being judged is still one scroll away.
        if (QWidget *approvalRow = widget->approvalRow()) {
            moveToIntentDock(approvalRow);
        }
        connect(widget, &ToolCallWidget::approvalChosen, this, [this, widget, request](PermissionDecision decision) {
            widget->setApprovalResolved(decision);
            updateIntentDockVisibility();
            m_agent.resolvePermission(decision);
        }, Qt::SingleShotConnection);
        m_turnStatus->setActivity(i18n("Waiting for approval"));
        m_prompt->setEnabled(false);
        updateSendButtonState();
        scrollToBottom();
    });

    connect(&m_agent, &AgentLoop::statusChanged, this, [this](const QString &status) {
        m_turnStatus->setIdleText(i18n("Enter to send · Shift+Enter for a new line"));
        m_prompt->setEnabled(!m_approvalPending());
        updateSendButtonState();
        Q_UNUSED(status)
    });

    connect(&m_agent, &AgentLoop::failed, this, [this](const QString &error) {
        freezeStreaming();
        showInfoMessage(i18n("Error: %1", error), true);
        updateSendButtonState();
    });

    // Retry status from LlmClient - show in info bar with bright brown/orange
    connect(m_agent.client(), &LlmClient::retryStatus, this, [this](const QString &message, int attempt, int maxAttempts, int delaySeconds) {
        Q_UNUSED(message);
        showInfoMessage(i18n("Retrying in %1s (attempt %2/%3)...", delaySeconds, attempt, maxAttempts), false);
    });

    connect(m_agent.client(), &LlmClient::retryScheduled, this, [this](int attempt, int maxAttempts, int delaySeconds) {
        Q_UNUSED(attempt);
        Q_UNUSED(maxAttempts);
        Q_UNUSED(delaySeconds);
        // Could show a persistent retry indicator if needed
    });

    connect(&m_agent, &AgentLoop::turnFinished, this, [this]() {
        m_prompt->setEnabled(true);
        updateSendButtonState();
        m_prompt->setFocus();
        setThinkingIndicator(false);
        setWorkingIndicator(false);
        // Refresh the context readout at the end of a turn: during one it would
        // recompute an estimate over a history that is still growing, and the
        // number the user acts on is the one for the next request.
        updateTokenDisplay();
        // Settle the status strip back to its idle hint now the turn is over.
        m_turnStatus->setBusy(false);
        // Auto-save conversation after each completed turn so it always
        // appears up-to-date in the history menu.
        if (!m_currentConversationId.isEmpty()) {
            const auto sessionData = m_agent.sessionData();
            if (!sessionData.messages.isEmpty()) {
                SessionStore::saveConversation(m_currentConversationId, sessionData, QString(),
                                              m_settings.maxSavedConversations > 0 ? m_settings.maxSavedConversations : 50);
                updateHistoryButton();
            }
        }
    });

    // Modes, MCP and checkpoints
    connect(&m_agent, &AgentLoop::modesChanged, this, [this] {
        populateModes();
        refreshModeButton();
    });
    connect(&m_agent, &AgentLoop::mcpStatusChanged, this, [this](const QString &summary) {
        Q_UNUSED(summary)
        updateMcpButton();
    });
    connect(&m_agent, &AgentLoop::mcpToolsChanged, this, [this] {
        updateMcpButton();
    });
    connect(&m_agent, &AgentLoop::mcpLogMessage, this, [this](const QString &message) {
        qWarning().noquote() << u"Kate AI MCP:"_s << message;
    });
    connect(&m_agent, &AgentLoop::checkpointCreated, this, [this](const QString &id, const QString &label) {
        Q_UNUSED(id)
        showInfoMessage(i18n("Checkpoint saved: %1", label.isEmpty() ? i18n("auto") : label), false);
    });
    connect(&m_agent, &AgentLoop::checkpointFailed, this, [this](const QString &error) {
        qWarning().noquote() << u"Kate AI checkpoint:"_s << error;
    });
    connect(&m_agent, &AgentLoop::checkpointRestored, this, [this](const QString &id) {
        showInfoMessage(i18n("Restored checkpoint %1.", id.left(8)), false);
        Q_EMIT aboutToSubmit(); // Re-read the workspace so Kate sees the rollback.
    });
    connect(&m_agent, &AgentLoop::subtaskStarted, this, [this](const QString &taskId, const QString &agentId, const QString &agentName, const QString &modeId, const QString &description) {
        Q_UNUSED(agentId)
        // The card already exists: it was created on toolStarted, which fires
        // before subtaskStarted for this tool.
        if (auto *widget = m_subtaskWidgets.value(taskId)) {
            widget->startAgent(agentName, modeId, description);
        }
        m_turnStatus->setSubtaskCount(m_agent.runningSubtaskCount());
        updateTeamButton();
        forceScrollToBottom();
    });
    connect(&m_agent, &AgentLoop::subtaskActivity, this, [this](const QString &taskId, const QString &line, bool isError) {
        if (auto *widget = m_subtaskWidgets.value(taskId)) {
            widget->appendActivity(line, isError);
        }
    });
    connect(&m_agent, &AgentLoop::subtaskFinished, this, [this](const QString &taskId, bool ok) {
        Q_UNUSED(taskId)
        Q_UNUSED(ok)
        m_turnStatus->setSubtaskCount(m_agent.runningSubtaskCount());
        updateTeamButton();
    });
    connect(&m_agent, &AgentLoop::turnFinished, this, &ChatWidget::markRunningSubtasksAbandoned);

    // Edit tracker signals
    connect(m_editTracker, &EditTracker::editAccepted, this, [this](const QString &path, const QString &toolName,
                                                                     const QString &newContent) {
        Q_UNUSED(toolName);
        Q_UNUSED(newContent);
        // Nothing to write: in AcceptEdits the file was already written when the
        // tool ran, so accepting only settles it in the queue. Say so, because
        // the button gives no other feedback.
        showInfoMessage(i18n("Kept changes to %1", QFileInfo(path).fileName()), false);
    });
    connect(m_editTracker, &EditTracker::editRejected, this, [this](const QString &path, const QString &toolName,
                                                                    const QString &oldContent) {
        Q_UNUSED(toolName);
        if (!m_agent.documentBridge()) {
            showInfoMessage(i18n("Cannot revert edit for %1: document bridge not available", path), true);
            return;
        }
        QString error;
        if (m_agent.documentBridge()->writeDocument(path, oldContent, &error)) {
            showInfoMessage(i18n("Edit reverted for %1", QFileInfo(path).fileName()), false);
        } else {
            showInfoMessage(i18n("Failed to revert edit for %1: %2", QFileInfo(path).fileName(), error), true);
        }
    });
    // Rejecting an edit to a file the agent created has to remove the file.
    // Writing the empty pre-edit content back instead would leave a zero-byte
    // file behind, which is worse than the file never having existed.
    connect(m_editTracker, &EditTracker::fileCreatedThenRejected, this, [this](const QString &path) {
        if (!QFile::remove(path)) {
            showInfoMessage(i18n("Failed to delete %1", QFileInfo(path).fileName()), true);
            return;
        }
        showInfoMessage(i18n("Deleted %1", QFileInfo(path).fileName()), false);
        Q_EMIT aboutToSubmit(); // Let Kate reload so the buffer matches the disk again.
    });
    connect(m_editTracker, &EditTracker::statusMessage, this, &ChatWidget::showInfoMessage);

    connect(&m_agent, &AgentLoop::modelsReceived, this, [this](Provider provider, const QStringList &models) {
        m_modelCatalog.insert(provider, models);
        refreshProviders();
        updateModelSelectorLabel();
        updateTokenDisplay();
        updateReasoningEffortButton();
    });

    connect(&m_agent, &AgentLoop::modelsFailed, this, [this](Provider provider, const QString &error) {
        m_modelCatalog.remove(provider);
        refreshProviders();
        updateModelSelectorLabel();
        if (provider == m_settings.provider) {
            m_turnStatus->flash(i18n("Model list unavailable: %1", error), true);
        }
    });

    updateModelSelectorLabel();
    updateTokenDisplay();
}

ChatWidget::~ChatWidget()
{
    stopThinkingPacer();
    if (m_indicatorTimer) {
        m_indicatorTimer->stop();
    }
    if (m_streamHeightTimer) {
        m_streamHeightTimer->stop();
    }

    // Save session before AgentLoop member is destroyed, using the tracked
    // conversation ID so we never silently create a duplicate active record.
    if (!m_agent.messages().isEmpty()) {
        const auto sessionData = m_agent.sessionData();
        if (!sessionData.messages.isEmpty()) {
            if (!m_currentConversationId.isEmpty()) {
                SessionStore::saveConversation(m_currentConversationId, sessionData, QString(),
                                              m_settings.maxSavedConversations > 0 ? m_settings.maxSavedConversations : 50);
            } else {
                // Fallback: create a new entry via the legacy path
                SessionStore::save(sessionData, m_settings.maxSavedConversations > 0 ? m_settings.maxSavedConversations : 50);
            }
        }
    }

    m_agent.abort();
    disconnect(&m_agent, nullptr, this, nullptr);
    disconnect(m_agent.client(), nullptr, this, nullptr);
    if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
        disconnect(m_scrollArea->verticalScrollBar(), nullptr, this, nullptr);
    }
    if (m_scrollToBottomBtn) {
        disconnect(m_scrollToBottomBtn, nullptr, this, nullptr);
        m_scrollToBottomBtn->setGraphicsEffect(nullptr);
    }
    if (m_prompt) {
        disconnect(m_prompt, nullptr, this, nullptr);
    }
}

void ChatWidget::addUserMessage(const QString &text)
{
    // Drop the welcome widget. deleteLater() left it parented and in the layout
    // until the event loop ran, so for a frame the empty-state hero sat directly
    // above the first real message with its suggestion chips still clickable;
    // clicking one of those fired a second prompt out of a dead widget.
    if (auto *welcome = m_transcriptContainer->findChild<QWidget *>(u"welcomeWidget"_s)) {
        m_transcriptLayout->removeWidget(welcome);
        delete welcome;
    }

    // Auto-update thread title on the first user message
    if (m_threadTitle && m_threadTitle->text() == i18n("New Thread")) {
        QString title = text.trimmed().split(u'\n').first();
        if (title.length() > 32) {
            title = title.left(30) + u"…";
        }
        m_threadTitle->setText(title);
    }

    auto *card = new QWidget(m_transcriptContainer);
    card->setObjectName(u"userCard"_s);
    card->setStyleSheet(ChatTheme::userCard());
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 9, 12, 9);
    cardLayout->setSpacing(4);

    auto *headerLayout = new QHBoxLayout;
    headerLayout->setContentsMargins(0, 0, 0, 0);

    auto *header = new QLabel(i18n("You"), card);
    header->setStyleSheet(ChatTheme::roleHeader());
    headerLayout->addWidget(header);
    headerLayout->addStretch();

    auto *copyBtn = createCopyButton(text, card);
    headerLayout->addWidget(copyBtn);
    cardLayout->addLayout(headerLayout);

    auto *msgLabel = new QLabel(card);
    msgLabel->setWordWrap(true);
    msgLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    msgLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    msgLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    msgLabel->setStyleSheet(ChatTheme::messageText());
    msgLabel->setText(escape(text).replace(u"\n"_s, u"<br>"_s));
    cardLayout->addWidget(msgLabel);

    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    appendTranscriptWidget(card);
    if (!m_loadingConversation) {
        forceScrollToBottom();
    }
    updateTokenDisplay();
}

void ChatWidget::addActivityMessage(const QString &text)
{
    // Skip retry messages - they are shown in the info bar above the composer
    if (text.startsWith(u"Retrying in "_s)) {
        return;
    }
    auto *pill = new QLabel(escape(text), m_transcriptContainer);
    pill->setStyleSheet(ChatTheme::hintLabel());
    appendTranscriptWidget(pill);
    scrollToBottom();
}

void ChatWidget::setStreaming(const QString &text)
{
    m_isStreaming = true;
    m_streamText = text;
    if (m_activeAssistantWidget && !m_activeAssistantBrowser) {
        m_activeAssistantWidget = nullptr;
    }
    if (!m_activeAssistantWidget) {
        m_activeAssistantWidget = new QWidget(m_transcriptContainer);
        m_activeAssistantWidget->setObjectName(u"assistantCard"_s);
        m_activeAssistantWidget->setStyleSheet(ChatTheme::assistantCard());
        auto *layout = new QVBoxLayout(m_activeAssistantWidget);
        layout->setContentsMargins(2, 2, 2, 2);
        layout->setSpacing(6);

        auto *headerLayout = new QHBoxLayout;
        headerLayout->setContentsMargins(0, 0, 0, 0);

        // A caption plus a single live dot. Two icons here competed for attention;
        // the dot alone already answers "is this still running?".
        auto *header = new QLabel(i18n("Kate AI"), m_activeAssistantWidget.data());
        header->setStyleSheet(ChatTheme::roleHeader());
        headerLayout->addWidget(header);

        // The pulse is the only moving part in an assistant turn, so it marks
        // "still streaming" without the whole block flickering.
        m_activeAssistantPulse = new QLabel(QStringLiteral("●"), m_activeAssistantWidget);
        m_activeAssistantPulse->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 9px; padding-left: 4px; }")
                                                 .arg(ChatTheme::accent()));
        headerLayout->addWidget(m_activeAssistantPulse);
        headerLayout->addStretch();

        m_activeAssistantCopyBtn = createCopyButton(QString(), m_activeAssistantWidget);
        headerLayout->addWidget(m_activeAssistantCopyBtn);
        layout->addLayout(headerLayout);

        QTextBrowser *thinkingBrowser = nullptr;
        QPushButton *thinkingToggle = nullptr;
        m_thinkingBlock = createThinkingBlock(m_activeAssistantWidget, thinkingBrowser, thinkingToggle, !m_settings.autoCollapseThinking);
        m_thinkingBrowser = thinkingBrowser;
        m_thinkingToggle = thinkingToggle;
        m_thinkingBlock->hide();
        layout->addWidget(m_thinkingBlock);

        // Structured plan checklist, rendered below the thinking block.
        m_planBlock = new QWidget(m_activeAssistantWidget);
        m_planBlock->hide();
        m_planLayout = new QVBoxLayout(m_planBlock);
        m_planLayout->setContentsMargins(4, 2, 4, 2);
        m_planLayout->setSpacing(2);
        auto *planLabel = new QLabel(i18n("Plan"), m_planBlock);
        planLabel->setStyleSheet(ChatTheme::sectionLabel());
        m_planLayout->addWidget(planLabel);
        layout->addWidget(m_planBlock);

        m_activeAssistantBrowser = new QTextBrowser(m_activeAssistantWidget);
        m_activeAssistantBrowser->setObjectName(u"assistantBrowser"_s);
        m_activeAssistantBrowser->setOpenExternalLinks(true);
        m_activeAssistantBrowser->setFrameShape(QFrame::NoFrame);
        m_activeAssistantBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_activeAssistantBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_activeAssistantBrowser->setStyleSheet(
            QStringLiteral("background: transparent; color: %1; border: none; padding: 0px;").arg(ChatTheme::textPrimary()));
        m_activeAssistantBrowser->document()->setDefaultStyleSheet(ChatTheme::messageCss());

        layout->addWidget(m_activeAssistantBrowser);
        m_activeAssistantWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        appendTranscriptWidget(m_activeAssistantWidget);
        m_isStreaming = true;
        syncIndicatorAnimation();
    }

    if (!m_activeAssistantBrowser) {
        return;
    }
    m_activeAssistantBrowser->setDocument(makeMarkdownDocument(closedMarkdown(m_streamText)));
    scheduleStreamHeightUpdate();
}

void ChatWidget::startThinkingPacer()
{
    if (!m_thinkingPacerTimer) {
        m_thinkingPacerTimer = new QTimer(this);
        m_thinkingPacerTimer->setInterval(50);
        connect(m_thinkingPacerTimer, &QTimer::timeout, this, [this]() {
            if (!m_thinkingBrowser || m_thinkingBuffer.isEmpty()) {
                stopThinkingPacer();
                return;
            }
            const int targetLen = m_thinkingBuffer.length();
            if (m_thinkingPacedLength >= targetLen) {
                if (!m_isThinking) {
                    stopThinkingPacer();
                }
                return;
            }

            const int remaining = targetLen - m_thinkingPacedLength;
            int step = 1;
            if (remaining > 300) {
                step = std::max(10, remaining / 12);
            } else if (remaining > 100) {
                step = std::max(4, remaining / 20);
            } else if (remaining > 30) {
                step = std::max(2, remaining / 25);
            } else {
                step = 1;
            }

            m_thinkingPacedLength = std::min(m_thinkingPacedLength + step, targetLen);
            updateThinkingDisplay();
        });
    }
    if (!m_thinkingPacerTimer->isActive()) {
        m_thinkingPacerTimer->start();
    }
}

void ChatWidget::stopThinkingPacer()
{
    if (m_thinkingPacerTimer && m_thinkingPacerTimer->isActive()) {
        m_thinkingPacerTimer->stop();
    }
}

void ChatWidget::flushThinkingPacer()
{
    stopThinkingPacer();
    m_thinkingPacedLength = m_thinkingBuffer.length();
    updateThinkingDisplay();
}

void ChatWidget::updateThinkingDisplay()
{
    if (!m_thinkingBrowser) {
        return;
    }
    const QString displayed = m_thinkingBuffer.left(m_thinkingPacedLength);
    m_thinkingBrowser->setDocument(makeMarkdownDocument(closedMarkdown(displayed)));
    if (m_thinkingBlock && !m_thinkingBuffer.isEmpty()) {
        m_thinkingBlock->show();
    }
    applyThinkingState(m_thinkingBlock, m_thinkingBrowser, m_thinkingToggle, m_thinkingExpanded);
}

void ChatWidget::addThinkingBlock(const QString &text)
{
    // Hidden reasoning may arrive before the first visible text delta, so
    // ensure the active assistant widget (and its thinking/plan blocks) exist.
    if (!m_activeAssistantWidget) {
        setStreaming(m_streamText);
    }
    if (!m_thinkingBrowser || !m_thinkingBlock) {
        return;
    }
    m_thinkingBuffer = text;
    m_thinkingBlock->show();
    registerThinkingBlock(m_thinkingBlock, m_thinkingBrowser, m_thinkingToggle);
    if (!m_isThinking) {
        flushThinkingPacer();
    } else {
        startThinkingPacer();
    }
    applyThinkingState(m_thinkingBlock, m_thinkingBrowser, m_thinkingToggle, m_thinkingExpanded);
    applyTranscriptCollapse();
    scrollToBottom();
}

void ChatWidget::appendThinkingDelta(const QString &delta)
{
    m_thinkingBuffer += delta;
    if (m_thinkingBlock && registerThinkingBlock(m_thinkingBlock, m_thinkingBrowser, m_thinkingToggle)) {
        applyTranscriptCollapse();
    }
    // Only auto-expand if the user hasn't manually collapsed the thinking block.
    // We track this via m_thinkingExpanded - if it's false, the user explicitly collapsed.
    if (m_thinkingExpanded) {
        startThinkingPacer();
    }
}

void ChatWidget::renderThinkingHtml()
{
    flushThinkingPacer();
}

void ChatWidget::collapseThinkingBlock()
{
    if (!m_thinkingBlock) {
        return;
    }
    m_thinkingBlock->show();
    applyThinkingState(m_thinkingBlock, m_thinkingBrowser, m_thinkingToggle, false);
}

void ChatWidget::toggleThinking()
{
    if (!m_thinkingBlock || !m_thinkingBrowser) {
        return;
    }
    applyThinkingState(m_thinkingBlock, m_thinkingBrowser, m_thinkingToggle, !m_thinkingExpanded);
}

QWidget *ChatWidget::createThinkingBlock(QWidget *parent, QTextBrowser *&browser, QPushButton *&toggle, bool initiallyExpanded)
{
    auto *block = new QWidget(parent);
    auto *tbLayout = new QVBoxLayout(block);
    tbLayout->setContentsMargins(0, 0, 0, 0);
    tbLayout->setSpacing(0);

    auto *tbHeader = new QHBoxLayout;
    toggle = new QPushButton(u"\u25b4 "_s + i18n("Reasoning"), block);
    toggle->setFlat(true);
    toggle->setCursor(Qt::PointingHandCursor);
    toggle->setStyleSheet(ChatTheme::toggleLink());
    tbHeader->addWidget(toggle);
    tbHeader->addStretch();
    tbLayout->addLayout(tbHeader);

    browser = new QTextBrowser(block);
    browser->setReadOnly(true);
    browser->setFrameShape(QFrame::NoFrame);
    browser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    browser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    browser->setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; color: %1; border: none;"
                                        " font-style: italic; font-size: 12px; padding: 4px; }")
                            .arg(ChatTheme::textMuted()));

    browser->document()->setDefaultStyleSheet(ChatTheme::thinkingCss());
    QPalette pal = browser->palette();
    pal.setColor(QPalette::Text, QColor(ChatTheme::textMuted()));
    pal.setColor(QPalette::Base, Qt::transparent);
    browser->setPalette(pal);
    tbLayout->addWidget(browser);

    connect(toggle, &QPushButton::clicked, this, [this, block, browser, toggle]() {
        const bool expanding = browser && !browser->isVisible();
        applyThinkingState(block, browser, toggle, expanding);
        if (m_thinkingBlock == block) {
            m_thinkingExpanded = expanding;
        }
    });

    applyThinkingState(block, browser, toggle, initiallyExpanded);
    return block;
}

void ChatWidget::applyThinkingState(QWidget *block, QTextBrowser *browser, QPushButton *toggle, bool expanded)
{
    if (!block) {
        return;
    }
    if (m_thinkingBlock == block) {
        m_thinkingExpanded = expanded;
    }
    if (toggle) {
        toggle->setText((expanded ? u"\u25b4 "_s : u"\u25be "_s) + i18n("Reasoning"));
        toggle->show();
    }
    const int headerH = toggle ? std::max(22, toggle->sizeHint().height()) : 22;
    if (expanded) {
        block->setMinimumHeight(0);
        block->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    if (browser) {
        if (expanded) {
            browser->show();
            const int width = browser->viewport()->width() > 40
                ? browser->viewport()->width()
                : std::max(160, block->width() - 8);
            browser->document()->setTextWidth(width);
            const int h = static_cast<int>(browser->document()->size().height()) + 20;
            browser->setFixedHeight(std::max(48, h));
            QPointer<QTextBrowser> browserWeak(browser);
            QPointer<QWidget> blockWeak(block);
            QTimer::singleShot(0, browser, [browserWeak, blockWeak]() {
                if (!browserWeak || !browserWeak->isVisible()) {
                    return;
                }
                const int laidOutWidth = browserWeak->viewport()->width() > 40
                    ? browserWeak->viewport()->width()
                    : std::max(160, blockWeak ? blockWeak->width() - 8 : 240);
                browserWeak->document()->setTextWidth(laidOutWidth);
                const int laidOutH = static_cast<int>(browserWeak->document()->size().height()) + 20;
                browserWeak->setFixedHeight(std::max(48, laidOutH));
            });
        } else {
            browser->hide();
        }
    }
    if (!expanded) {
        block->setMinimumHeight(headerH);
        block->setMaximumHeight(headerH);
    }
}

void ChatWidget::addPlanChecklist(const QJsonArray &plan)
{
    if (!m_planBlock || !m_planLayout) {
        return;
    }
    // Remove and delete all existing plan step widgets before rebuilding.
    // m_planSteps.clear() only drops the QCheckBox* keys from the hash; it
    // does not delete the widgets themselves, so omitting this loop causes
    // QCheckBox children to accumulate in m_planLayout on every plan update.
    m_planSteps.clear();
    while (m_planLayout->count() > 1) { // keep the "Plan" header label (index 0)
        QLayoutItem *item = m_planLayout->takeAt(1);
        if (item) {
            if (item->widget()) {
                // delete(), not deleteLater(): a deferred deletion leaves the row
                // parented and painting over the top of the list until the event
                // loop runs, and a plan update can rebuild several times in one
                // turn, stacking ghosts over the real steps.
                delete item->widget();
            }
            delete item;
        }
    }
    for (const QJsonValue &v : plan) {
        const QJsonObject o = v.toObject();
        const QString desc = o.value(u"description"_s).toString();
        const bool completed = o.value(u"completed"_s).toBool();
        auto *cb = new QCheckBox(desc, m_planBlock);
        cb->setChecked(completed);
        cb->setDisabled(true);
        cb->setStyleSheet(ChatTheme::planChecklist());
        m_planLayout->addWidget(cb);
        m_planSteps.insert(cb, o.value(u"id"_s).toString());
    }
    m_planBlock->show();
    scrollToBottom();
}

void ChatWidget::markPlanStepCompleted(const QString &stepId)
{
    bool matched = false;
    for (auto it = m_planSteps.cbegin(); it != m_planSteps.cend(); ++it) {
        if (it.value() == stepId) {
            if (it.key()) {
                it.key()->setChecked(true);
            }
            matched = true;
            break;
        }
    }
    // Completing the last outstanding step finishes the plan, so tick the rest
    // too. The checklist is what the user reads afterwards as a record of what
    // the run actually did; leaving the tail unchecked after a plan that is
    // demonstrably complete made it look like the agent stopped part-way.
    if (matched) {
        for (auto it = m_planSteps.cbegin(); it != m_planSteps.cend(); ++it) {
            if (it.key() && !it.key()->isChecked()) {
                return;
            }
        }
        for (auto it = m_planSteps.cbegin(); it != m_planSteps.cend(); ++it) {
            if (it.key()) {
                it.key()->setChecked(true);
            }
        }
    }
}

void ChatWidget::freezeStreaming()
{
    if (m_streamHeightTimer) {
        m_streamHeightTimer->stop();
    }
    if (m_activeAssistantBrowser && !m_streamText.isEmpty()) {
        m_activeAssistantBrowser->setDocument(makeMarkdownDocument(m_streamText));
        const int docH = std::max(30, static_cast<int>(m_activeAssistantBrowser->document()->size().height()) + 16);
        if (m_activeAssistantBrowser->height() != docH) {
            m_activeAssistantBrowser->setFixedHeight(docH);
        }
    }
    if (m_activeAssistantCopyBtn) {
        m_activeAssistantCopyBtn->setProperty("copyText", m_streamText);
        m_activeAssistantCopyBtn = nullptr;
    }
    flushThinkingPacer();
    if (m_settings.autoCollapseThinking) {
        collapseThinkingBlock();
    }
    m_isStreaming = false;
    if (m_activeAssistantPulse) {
        m_activeAssistantPulse->hide();
        m_activeAssistantPulse = nullptr;
    }
    clearStreamingPointers();
    applyTranscriptCollapse();
    syncIndicatorAnimation();
}

void ChatWidget::scrollToBottom()
{
    if (!m_scrollArea) {
        return;
    }
    auto *sb = m_scrollArea->verticalScrollBar();
    const bool canScroll = sb && !transcriptShouldFollowTail(sb->value(), sb->maximum());
    if (m_userScrolledUp && canScroll) {
        if (m_scrollToBottomBtn) {
            animateScrollButtonShow();
            m_scrollToBottomBtn->raise();
        }
        return;
    }
    if (m_userScrolledUp && !canScroll) {
        // Nothing left below, so the "jump to latest" affordance has nothing to
        // offer. Leaving it up parked the button over the newest message.
        m_userScrolledUp = false;
        if (m_scrollToBottomBtn && m_scrollToBottomBtn->isVisible()) {
            animateScrollButtonHide();
        }
    }
    forceScrollToBottom();
}

void ChatWidget::forceScrollToBottom()
{
    m_userScrolledUp = false;
    if (m_scrollToBottomBtn) {
        m_scrollToBottomBtn->hide();
    }
    if (!m_scrollArea) {
        return;
    }
    auto *sb = m_scrollArea->verticalScrollBar();
    if (!sb) {
        return;
    }
    // Move now instead of relying only on a deferred timer: this runs many times
    // per second while a response streams in, and leaving the viewport on stale
    // geometry until the timer fires is what makes the transcript jump.
    sb->setValue(sb->maximum());
    // Re-pin once Qt has flushed the pending layout, so the range read above is
    // the post-relayout one rather than a pre-grow one.
    QTimer::singleShot(0, this, [thisWeak = QPointer<ChatWidget>(this)]() {
        if (thisWeak && thisWeak->m_scrollArea) {
            auto *bar = thisWeak->m_scrollArea->verticalScrollBar();
            bar->setValue(bar->maximum());
        }
    });
}

void ChatWidget::setThinkingIndicator(bool show)
{
    if (!m_thinkingIndicator) {
        return;
    }
    if (show && !m_isThinking) {
        m_isThinking = true;
        m_indicatorTick = 0;
        m_thinkingIndicator->setText(i18n("Thinking") + progressiveDots(0));
        m_thinkingIndicator->show();
        tickIndicators();
    } else if (!show && m_isThinking) {
        m_isThinking = false;
        m_thinkingIndicator->hide();
        if (m_thinkingToggle && m_thinkingBlock) {
            applyThinkingState(m_thinkingBlock, m_thinkingBrowser, m_thinkingToggle, m_thinkingExpanded);
        }
    }
    syncIndicatorAnimation();
}

void ChatWidget::setWorkingIndicator(bool show)
{
    if (!m_workingIndicator) {
        return;
    }
    if (show && !m_isWorking) {
        m_isWorking = true;
        if (m_workingLabelBase.isEmpty()) {
            m_workingLabelBase = i18n("Working");
        }
        m_workingIndicator->setText(m_workingLabelBase + progressiveDots(0));
        m_workingIndicator->show();
        tickIndicators();
    } else if (!show && m_isWorking) {
        m_isWorking = false;
        m_workingIndicator->hide();
    }
    syncIndicatorAnimation();
}

void ChatWidget::syncIndicatorAnimation()
{
    const bool need = m_isThinking || m_isWorking || m_isStreaming;
    if (!m_indicatorTimer) {
        m_indicatorTimer = new QTimer(this);
        m_indicatorTimer->setInterval(380);
        connect(m_indicatorTimer, &QTimer::timeout, this, &ChatWidget::tickIndicators);
    }
    if (need) {
        if (!m_indicatorTimer->isActive()) {
            m_indicatorTimer->start();
        }
    } else if (m_indicatorTimer->isActive()) {
        m_indicatorTimer->stop();
    }
}

void ChatWidget::tickIndicators()
{
    m_indicatorTick = (m_indicatorTick + 1) & 1023;
    const QString dots = progressiveDots(m_indicatorTick);

    if (m_isThinking && m_thinkingIndicator) {
        m_thinkingIndicator->setText(i18n("Thinking") + dots);
        if (auto *effect = qobject_cast<QGraphicsOpacityEffect *>(m_thinkingIndicator->graphicsEffect())) {
            effect->setOpacity(0.62 + 0.38 * ((m_indicatorTick % 2 == 0) ? 1.0 : 0.0));
        }
        if (m_thinkingToggle && m_thinkingBlock) {
            const QString arrow = m_thinkingExpanded ? u"\u25b4 "_s : u"\u25be "_s;
            m_thinkingToggle->setText(arrow + i18n("Reasoning") + dots);
        }
    }

    if (m_isWorking && m_workingIndicator) {
        if (m_workingLabelBase.isEmpty()) {
            m_workingLabelBase = i18n("Working");
        }
        m_workingIndicator->setText(m_workingLabelBase + dots);
        if (auto *effect = qobject_cast<QGraphicsOpacityEffect *>(m_workingIndicator->graphicsEffect())) {
            effect->setOpacity(0.62 + 0.38 * ((m_indicatorTick % 2 == 1) ? 1.0 : 0.0));
        }
    }

    if (m_activeAssistantPulse) {
        m_activeAssistantPulse->setVisible(m_isStreaming && (m_indicatorTick % 2 == 0));
    }

    const int frame = m_indicatorTick % 4;
    for (const auto &widget : m_toolCallOrder) {
        if (widget && widget->isRunning()) {
            widget->setActivityFrame(frame);
        }
    }
}

bool ChatWidget::registerThinkingBlock(QWidget *block, QTextBrowser *browser, QPushButton *toggle)
{
    if (!block) {
        return false;
    }
    for (const auto &entry : m_thinkingBlocks) {
        if (entry.block == block) {
            return false;
        }
    }
    m_thinkingBlocks.append({block, browser, toggle});
    return true;
}

void ChatWidget::applyTranscriptCollapse()
{
    const int keep = m_settings.maxExpandedToolCards;
    const bool expandAll = keep <= 0;

    for (int i = m_thinkingBlocks.size() - 1; i >= 0; --i) {
        if (m_thinkingBlocks.at(i).block.isNull()) {
            m_thinkingBlocks.removeAt(i);
        }
    }
    int thinkingBudget = expandAll ? m_thinkingBlocks.size() : keep;
    for (int i = m_thinkingBlocks.size() - 1; i >= 0; --i) {
        const ThinkingBlockRef &entry = m_thinkingBlocks.at(i);
        const bool live = (entry.block == m_thinkingBlock);
        bool expanded = expandAll;
        if (!expandAll) {
            if (live && m_isThinking) {
                expanded = m_thinkingExpanded;
            } else if (thinkingBudget > 0) {
                expanded = true;
                --thinkingBudget;
            } else {
                expanded = false;
            }
        }
        applyThinkingState(entry.block, entry.browser, entry.toggle, expanded);
    }

    for (int i = m_toolCallOrder.size() - 1; i >= 0; --i) {
        if (m_toolCallOrder.at(i).isNull()) {
            m_toolCallOrder.removeAt(i);
        }
    }
    int toolBudget = expandAll ? m_toolCallOrder.size() : keep;
    for (int i = m_toolCallOrder.size() - 1; i >= 0; --i) {
        ToolCallWidget *widget = m_toolCallOrder.at(i);
        if (!widget) {
            continue;
        }
        if (widget->isFileEditTool() || widget->isRunning()) {
            widget->setExpanded(true);
            continue;
        }
        if (expandAll || toolBudget > 0) {
            widget->setExpanded(true);
            if (!expandAll) {
                --toolBudget;
            }
        } else {
            widget->setExpanded(false);
        }
    }
}

void ChatWidget::updateScrollButtonPosition()
{
    if (!m_scrollToBottomBtn || !m_scrollArea) {
        return;
    }
    const int btnW = m_scrollToBottomBtn->sizeHint().width() + 16;
    const int btnH = 28;
    const int x = (m_scrollArea->width() - btnW) / 2;
    const int y = m_scrollArea->height() - btnH - 12;
    m_scrollToBottomBtn->setGeometry(x, y, btnW, btnH);
    m_scrollToBottomBtn->raise();
}

void ChatWidget::animateScrollButtonShow()
{
    if (!m_scrollToBottomBtn) {
        return;
    }
    m_scrollToBottomBtn->show();
    updateScrollButtonPosition();
    m_scrollToBottomBtn->raise();

    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(m_scrollToBottomBtn->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(m_scrollToBottomBtn);
        m_scrollToBottomBtn->setGraphicsEffect(effect);
    }
    if (effect->opacity() >= 0.99) {
        return;
    }
    effect->setOpacity(0.0);

    auto *anim = new QPropertyAnimation(effect, "opacity", effect);
    anim->setDuration(150);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void ChatWidget::animateScrollButtonHide()
{
    if (!m_scrollToBottomBtn || !m_scrollToBottomBtn->isVisible()) {
        return;
    }

    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(m_scrollToBottomBtn->graphicsEffect());
    if (!effect) {
        m_scrollToBottomBtn->hide();
        return;
    }

    auto *anim = new QPropertyAnimation(effect, "opacity", effect);
    anim->setDuration(150);
    anim->setStartValue(effect->opacity());
    anim->setEndValue(0.0);
    anim->setEasingCurve(QEasingCurve::InCubic);
    QPointer<QPushButton> btn = m_scrollToBottomBtn;
    connect(anim, &QPropertyAnimation::finished, effect, [btn]() {
        if (btn) {
            btn->hide();
        }
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void ChatWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateScrollButtonPosition();
}

void ChatWidget::setCompletionWords(const QStringList &words)
{
    if (m_prompt) {
        m_prompt->setCompletionWords(words);
    }
}

QPushButton *ChatWidget::createCopyButton(const QString &textToCopy, QWidget *parent)
{
    auto *btn = new QPushButton(i18n("Copy"), parent);
    btn->setProperty("copyText", textToCopy);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedHeight(22);
    btn->setStyleSheet(
        u"QPushButton {"
        u"  color: #888888;"
        u"  background-color: transparent;"
        u"  border: 1px solid #38383e;"
        u"  border-radius: 4px;"
        u"  padding: 2px 8px;"
        u"  font-size: 11px;"
        u"}"
        u"QPushButton:hover {"
        u"  color: #ffffff;"
        u"  background-color: #2a2a2e;"
        u"  border-color: #4a4a52;"
        u"}"_s);

    connect(btn, &QPushButton::clicked, this, [btn, this]() {
        QString text = btn->property("copyText").toString();
        if (text.isEmpty()) {
            text = m_streamText;
        }
        QGuiApplication::clipboard()->setText(text);
        btn->setText(i18n("✓ Copied"));
        btn->setStyleSheet(
            u"QPushButton {"
            u"  color: #22c55e;"
            u"  background-color: #1a3320;"
            u"  border: 1px solid #22c55e;"
            u"  border-radius: 4px;"
            u"  padding: 2px 8px;"
            u"  font-size: 11px;"
            u"}"_s);
        QTimer::singleShot(2000, btn, [btnWeak = QPointer<QPushButton>(btn)]() {
            if (btnWeak) {
                btnWeak->setText(i18n("Copy"));
                btnWeak->setStyleSheet(
                    u"QPushButton {"
                    u"  color: #888888;"
                    u"  background-color: transparent;"
                    u"  border: 1px solid #38383e;"
                    u"  border-radius: 4px;"
                    u"  padding: 2px 8px;"
                    u"  font-size: 11px;"
                    u"}"
                    u"QPushButton:hover {"
                    u"  color: #ffffff;"
                    u"  background-color: #2a2a2e;"
                    u"  border-color: #4a4a52;"
                    u"}"_s);
            }
        });
    });
    return btn;
}

QWidget *ChatWidget::createWelcomeWidget()
{
    auto *welcome = new QWidget(m_transcriptContainer);
    welcome->setObjectName(u"welcomeWidget"_s);
    auto *wLayout = new QVBoxLayout(welcome);
    wLayout->setContentsMargins(16, 16, 16, 12);
    wLayout->setSpacing(8);
    wLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    auto *wIcon = new QLabel(welcome);
    wIcon->setAlignment(Qt::AlignCenter);
    wIcon->setPixmap(QIcon::fromTheme(u"dialog-information"_s).pixmap(26, 26));
    wLayout->addWidget(wIcon);

    auto *wTitle = new QLabel(i18n("Kate AI Agent"), welcome);
    wTitle->setAlignment(Qt::AlignCenter);
    wTitle->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 15px; font-weight: 600; }")
                              .arg(ChatTheme::textPrimary()));
    wLayout->addWidget(wTitle);

    auto *wSub = new QLabel(i18n("Ask questions, edit code, and explore your workspace."), welcome);
    wSub->setAlignment(Qt::AlignCenter);
    wSub->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 12px; margin-bottom: 8px; }")
                            .arg(ChatTheme::textMuted()));
    wLayout->addWidget(wSub);

    // Starter suggestion chips
    auto *chipsLayout = new QVBoxLayout;
    chipsLayout->setSpacing(6);

    // Suggestion chips. Deliberately unadorned: the wording is the affordance, and
    // a row of coloured pictograms competed with the transcript below it.
    const struct Suggestion {
        QString title;
        QString prompt;
    } suggestions[] = {
        {i18n("Explain active file"), i18n("Explain the active file and its architecture.")},
        {i18n("Find bugs & edge cases"), i18n("Inspect the current code for bugs, edge cases, and potential improvements.")},
        {i18n("Generate tests"), i18n("Write comprehensive unit tests for the code in this file.")}
    };

    for (const auto &s : suggestions) {
        auto *btn = new QPushButton(s.title, welcome);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(
            QStringLiteral(
                "QPushButton {"
                "  background-color: %1;"
                "  color: %2;"
                "  border: 1px solid %3;"
                "  border-radius: 8px;"
                "  padding: 8px 12px;"
                "  font-size: 12px;"
                "  text-align: left;"
                "}"
                "QPushButton:hover {"
                "  background-color: %4;"
                "  border-color: %5;"
                "  color: #ffffff;"
                "}")
                .arg(ChatTheme::surfaceBg(), ChatTheme::textPrimary(), ChatTheme::border(), ChatTheme::hoverBg(),
                     ChatTheme::borderStrong()));
        connect(btn, &QPushButton::clicked, this, [this, prompt = s.prompt]() {
            ask(prompt);
        });
        chipsLayout->addWidget(btn);
    }

    wLayout->addLayout(chipsLayout);
    welcome->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    return welcome;
}

// The live "Thinking" / "Working" row that always sits at the bottom of the
// transcript. Built through a helper because it has to be recreated in two
// places, and the two copies had already drifted apart.
QWidget *ChatWidget::createIndicatorsRow()
{
    auto *row = new QWidget(m_transcriptContainer);
    row->setObjectName(u"indicatorsContainer"_s);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 4, 0, 4);
    layout->setSpacing(8);
    layout->addStretch();

    // Plain text rather than an emoji badge: it re-renders on every tick at the
    // end of the transcript, so it stays low contrast and lets the tool cards
    // above it carry the actual signal.
    m_thinkingIndicator = new QLabel(i18n("Thinking") + progressiveDots(0), row);
    m_thinkingIndicator->setStyleSheet(ChatTheme::activityPill());
    attachPulseEffect(m_thinkingIndicator);
    m_thinkingIndicator->hide();
    layout->addWidget(m_thinkingIndicator);

    m_workingLabelBase = i18n("Working");
    m_workingIndicator = new QLabel(m_workingLabelBase + progressiveDots(0), row);
    m_workingIndicator->setStyleSheet(ChatTheme::activityPill());
    attachPulseEffect(m_workingIndicator);
    m_workingIndicator->hide();
    layout->addWidget(m_workingIndicator);

    return row;
}

int ChatWidget::transcriptInsertIndex() const
{
    if (!m_transcriptLayout) {
        return 0;
    }
    // Messages are appended at the end of the message area: after the leading
    // stretch spacer (which absorbs the slack and bottom-anchors the content)
    // and always before the pinned status-indicator row.
    int spacerIndex = -1;
    int indicatorsIndex = -1;
    const QWidget *indicators =
        m_indicatorsRow ? m_indicatorsRow.data() : m_thinkingIndicator ? m_thinkingIndicator->parentWidget() : nullptr;
    for (int i = 0; i < m_transcriptLayout->count(); ++i) {
        QLayoutItem *item = m_transcriptLayout->itemAt(i);
        if (!item) {
            continue;
        }
        if (item->spacerItem() && spacerIndex < 0) {
            spacerIndex = i;
        }
        if (indicators && item->widget() == indicators) {
            indicatorsIndex = i;
        }
    }
    return transcriptInsertIndexFor(TranscriptAnchor::Bottom, spacerIndex, indicatorsIndex,
                                    m_transcriptLayout->count());
}

void ChatWidget::appendTranscriptWidget(QWidget *widget)
{
    if (!m_transcriptLayout || !widget) {
        return;
    }
    m_transcriptLayout->insertWidget(transcriptInsertIndex(), widget);
}

void ChatWidget::addIntentWidget(QWidget *widget)
{
    if (!m_intentDockLayout || !widget) {
        return;
    }
    m_intentDockLayout->addWidget(widget);
    updateIntentDockVisibility();
}

void ChatWidget::moveToIntentDock(QWidget *widget, int index)
{
    if (!m_intentDockLayout || !widget) {
        return;
    }
    // insertWidget() reparents, so the widget keeps its identity, styling and
    // signal connections; only its position changes.
    if (index < 0 || index > m_intentDockLayout->count()) {
        index = m_intentDockLayout->count();
    }
    m_intentDockLayout->insertWidget(index, widget);
    widget->show();
    updateIntentDockVisibility();
}

void ChatWidget::retireIntentWidget(QWidget *widget)
{
    if (!widget) {
        return;
    }
    if (m_intentDockLayout) {
        m_intentDockLayout->removeWidget(widget);
    }
    // Back into the transcript, reparented as a normal message so the finished
    // result stays part of the thread.
    appendTranscriptWidget(widget);
    updateIntentDockVisibility();
}

void ChatWidget::updateIntentDockVisibility()
{
    if (!m_intentDock || !m_intentDockLayout) {
        return;
    }
    // Driven by whether anything is actually on screen, not by whether the
    // layout is merely non-empty: a resolved approval hides its row but leaves
    // it parented here.
    //
    // isHidden() rather than isVisible(): a child of a hidden dock reports
    // isVisible() == false, so testing visibility here would mean the dock could
    // never bring itself back.
    for (int i = 0; i < m_intentDockLayout->count(); ++i) {
        QLayoutItem *item = m_intentDockLayout->itemAt(i);
        if (item && item->widget() && !item->widget()->isHidden()) {
            m_intentDock->show();
            return;
        }
    }
    m_intentDock->hide();
}

void ChatWidget::clearIntentDock()
{
    if (!m_intentDockLayout) {
        return;
    }
    while (QLayoutItem *item = m_intentDockLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            // Delete rather than unparent: an approval row was reparented into
            // the dock, so it is no longer a child of its tool card and would
            // simply leak. Callers must clear their own bookkeeping first.
            delete widget;
        }
        delete item;
    }
    updateIntentDockVisibility();
}

void ChatWidget::showInfoMessage(const QString &message, bool isError)
{
    if (!m_infoBar) {
        return;
    }
    if (isError) {
        m_infoBar->setStyleSheet(u"QLabel { color: #ff8888; font-size: 12px; font-weight: bold; padding: 8px 12px; background: transparent; border: none; }"_s);
    } else {
        // Bright brown/orange for retries
        m_infoBar->setStyleSheet(u"QLabel { color: #ffaa00; font-size: 12px; font-weight: bold; padding: 8px 12px; background: transparent; border: none; }"_s);
    }
    m_infoBar->setText(message);
    m_infoBar->show();

    // Auto-hide after 10 seconds for retries, keep errors visible until dismissed
    if (!isError) {
        QTimer::singleShot(10000, this, [thisWeak = QPointer<ChatWidget>(this), message]() {
            if (thisWeak && thisWeak->m_infoBar && thisWeak->m_infoBar->text() == message) {
                thisWeak->m_infoBar->hide();
            }
        });
    }
}

void ChatWidget::setSettings(const Settings &settings)
{
    m_settings = settings;

    const int permIndex = m_permission->findData(permissionModeId(settings.permissionMode));
    if (permIndex >= 0) {
        m_permission->setCurrentIndex(permIndex);
    }
    const int sandboxIndex = m_sandbox->findData(sandboxProfileId(settings.sandbox));
    if (sandboxIndex >= 0) {
        m_sandbox->setCurrentIndex(sandboxIndex);
    }
    const int modeIndex = m_mode->findData(settings.agentMode);
    if (modeIndex >= 0) {
        m_mode->setCurrentIndex(modeIndex);
    }
    refreshModeButton();
    refreshPermissionButton();
    m_thinking->setChecked(settings.thinkingMode);
    updateThinkingButtonStyle();
    updateReasoningEffortButton();
    m_updatingCombos = false;

    // Propagate settings to agent
    m_agent.setSettings(settings);

    refreshProviders();
    updateModelSelectorLabel();
    updateTokenDisplay();
    updateReasoningEffortButton();
    updateMcpButton();

    for (Provider provider : {Provider::Grok, Provider::OpenAI, Provider::OpenRouter, Provider::DeepSeek, Provider::OpenAICompatible, Provider::ClaudeCompatible, Provider::Kilo, Provider::Acp, Provider::OpenCode}) {
        Settings providerSettings = settings;
        providerSettings.provider = provider;
        // Only fetch from providers that both have a key and expose a catalogue.
        if (!apiKeyFor(providerSettings).trimmed().isEmpty() && providerSupportsModelListing(provider) && !m_modelCatalog.contains(provider)) {
            m_agent.fetchModels(provider);
        }
    }
}

void ChatWidget::applyProviderToCombos()
{
    refreshModels();
    updateModelSelectorLabel();
    updateTokenDisplay();
}

void ChatWidget::refreshProviders()
{
    const bool wasUpdating = m_updatingCombos;
    m_updatingCombos = true;
    m_provider->clear();
    for (Provider provider : {Provider::Grok, Provider::OpenAI, Provider::OpenRouter, Provider::DeepSeek, Provider::OpenAICompatible, Provider::ClaudeCompatible, Provider::Kilo, Provider::Acp, Provider::OpenCode}) {
        // Only show provider if it has a valid API key configured
        Settings providerSettings = m_settings;
        providerSettings.provider = provider;
        if (!apiKeyFor(providerSettings).trimmed().isEmpty()) {
            m_provider->addItem(providerLabel(provider), providerId(provider));
        }
    }
    const int index = m_provider->findData(providerId(m_preferredProvider));
    if (index >= 0) {
        m_provider->setCurrentIndex(index);
        m_settings.provider = m_preferredProvider;
    } else if (m_provider->count() > 0) {
        m_provider->setCurrentIndex(0);
        m_settings.provider = providerFromId(m_provider->currentData().toString());
    } else {
        m_provider->addItem(i18n("Configure an API key…"), QVariant());
        m_provider->setCurrentIndex(0);
    }
    m_provider->setEnabled(m_provider->count() > 0);
    m_updatingCombos = wasUpdating;
    refreshModels();
    updateModelSelectorLabel();
    updateTokenDisplay();
    m_agent.setSettings(m_settings);
}

void ChatWidget::refreshModels()
{
    const bool wasUpdating = m_updatingCombos;
    m_updatingCombos = true;
    m_model->clear();
    // Only show models fetched from the API (no placeholder/default models)
    const QStringList models = m_modelCatalog.value(m_settings.provider);
    m_model->addItems(models);
    m_model->setEnabled(!models.isEmpty());

    // Prefer the model already stored in settings. Only fall back to the first
    // entry in the list when no model has been chosen yet. Overwriting a valid
    // selection is wrong for providers like OpenRouter whose live catalog is
    // much larger than the hard-coded defaults — otherwise picking a model
    // from the menu gets reset as soon as the catalog is cleared and
    // re-fetched (settingsChanged → setSettings → refreshModels).
    const QString currentModel = modelFor(m_settings).trimmed();
    const int index = currentModel.isEmpty() ? -1 : m_model->findText(currentModel);
    if (index >= 0) {
        m_model->setCurrentIndex(index);
    } else if (currentModel.isEmpty() && !models.isEmpty()) {
        m_model->setCurrentIndex(0);
        const QString selectedModel = models.at(0);
        switch (m_settings.provider) {
            case Provider::OpenAI:
                m_settings.openaiModel = selectedModel;
                break;
            case Provider::OpenRouter:
                m_settings.openrouterModel = selectedModel;
                break;
            case Provider::DeepSeek:
                m_settings.deepseekModel = selectedModel;
                break;
            case Provider::OpenAICompatible:
                m_settings.openaiCompatibleModel = selectedModel;
                break;
            case Provider::ClaudeCompatible:
                m_settings.claudeCompatibleModel = selectedModel;
                break;
            case Provider::OpenCode:
                m_settings.opencodeModel = selectedModel;
                break;
            case Provider::Acp:
                m_settings.acpModel = selectedModel;
                break;
            case Provider::Grok:
            default:
                m_settings.grokModel = selectedModel;
                break;
        }
    } else {
        // Keep the stored model even if it is not in the (possibly incomplete)
        // list yet — e.g. right after catalog clear while fetchModels is in flight.
        m_model->setCurrentIndex(-1);
        if (!currentModel.isEmpty() && m_model->isEditable()) {
            m_model->setEditText(currentModel);
        }
    }
    m_updatingCombos = wasUpdating;
    updateModelSelectorLabel();
    updateTokenDisplay();
}
void ChatWidget::updateModelSelectorLabel()
{
    if (!m_modelSelector) return;
    // The chip sits in a narrow row next to the mode chip, so it carries the
    // model alone. The provider (and the raw reasoning level) move into the
    // tooltip, where there is room for them and nobody has to read them twice.
    const QString model = modelFor(m_settings);
    const QString label = model.isEmpty() ? i18n("Select model") : model;
    m_modelSelector->setText(label + QStringLiteral("  ▾"));

    QStringList parts{providerLabel(m_settings.provider)};
    if (!model.isEmpty()) {
        parts.append(model);
    }
    if (!m_settings.reasoningEffort.isEmpty()) {
        parts.append(i18n("Reasoning: %1", m_settings.reasoningEffort));
    }
    m_modelSelector->setToolTip(parts.join(QStringLiteral("  ·  ")));
}

void ChatWidget::updateTokenDisplay()
{
    if (!m_tokenCount) {
        return;
    }
    // What belongs here is how full the context window is, not the model name
    // (which duplicates the chip two buttons to the left) and not the raw token
    // count (which means nothing without the denominator). Both are already
    // computed for the request that is actually sent, so reuse them rather than
    // keeping a second estimate that can disagree with the real budget.
    const int tokens = ContextManager::estimateTokens(m_agent.messages());
    const int window = m_settings.contextWindow > 0
        ? m_settings.contextWindow
        : ContextManager::contextWindowFor(modelFor(m_settings));
    if (window <= 0) {
        m_tokenCount->setText(QString());
        m_tokenCount->hide();
        return;
    }

    const int percent = qBound(0, (tokens * 100) / window, 100);
    m_tokenCount->setText(i18n("%1% ctx", percent));
    m_tokenCount->setToolTip(i18n("Context: about %1 of %2 tokens used (%3%)", tokens, window, percent));
    // Colour follows how close the limit is, so the number alone carries the
    // warning without needing to be read closely.
    const QString colour = percent >= 90 ? ChatTheme::danger() : (percent >= 70 ? ChatTheme::warning() : ChatTheme::textMuted());
    m_tokenCount->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 11px; font-family: monospace; background: transparent; border: none; }")
            .arg(colour));
    m_tokenCount->show();
}

void ChatWidget::updateThinkingButtonStyle()
{
    if (!m_thinking) return;
    // An on/off toggle, not a status light. The earlier version filled the
    // button with solid green, which made it the loudest thing in the composer
    // and pulled the eye away from the send button it sits next to.
    m_thinking->setIcon(QIcon());
    if (m_thinking->isChecked()) {
        m_thinking->setText(QStringLiteral("\u{2726}")); // filled diamond
        m_thinking->setToolTip(i18n("Thinking mode is on"));
        m_thinking->setStyleSheet(
            QStringLiteral(
                "QPushButton { color: #ffffff; background-color: %1; border: none;"
                " border-radius: 13px; padding: 0; }"
                "QPushButton:hover { background-color: %2; }")
            .arg(ChatTheme::accent(), ChatTheme::accentHover()));
    } else {
        m_thinking->setText(QStringLiteral("\u{2727}")); // hollow diamond
        m_thinking->setToolTip(i18n("Toggle thinking mode"));
        m_thinking->setStyleSheet(ChatTheme::iconButton());
    }
}

void ChatWidget::updateReasoningEffortButton()
{
    if (!m_reasoningEffort) return;

    const bool supports = modelSupportsReasoningEffort();

    QString text;
    QString toolTip;
    if (m_settings.reasoningEffort.isEmpty()) {
        text = QStringLiteral("Auto");
        toolTip = supports ? i18n("Reasoning effort: Auto (provider default)") : i18n("Reasoning effort: not supported by this model");
    } else if (m_settings.reasoningEffort == u"minimal"_s) {
        text = i18n("Minimal");
        toolTip = i18n("Reasoning effort: Minimal");
    } else if (m_settings.reasoningEffort == u"low"_s) {
        text = i18n("Low");
        toolTip = i18n("Reasoning effort: Low");
    } else if (m_settings.reasoningEffort == u"medium"_s) {
        text = i18n("Medium");
        toolTip = i18n("Reasoning effort: Medium");
    } else if (m_settings.reasoningEffort == u"high"_s) {
        text = i18n("High");
        toolTip = i18n("Reasoning effort: High");
    } else {
        text = m_settings.reasoningEffort;
        toolTip = i18n("Reasoning effort: %1", m_settings.reasoningEffort);
    }

    m_reasoningEffort->setText(text);
    m_reasoningEffort->setToolTip(toolTip);

    // Same pill as the mode and model chips, so the three read as one control
    // group. An unsupported model is dimmed rather than hidden: hiding it moved
    // the send button sideways every time the model changed.
    if (!supports) {
        m_reasoningEffort->setStyleSheet(
            QStringLiteral(
                "QPushButton {"
                "  background-color: transparent;"
                "  color: #5a5a62;"
                "  border: 1px solid %1;"
                "  border-radius: 13px;"
                "  padding: 3px 10px;"
                "  font-size: 11px;"
                "}"
                "QPushButton:hover { border-color: %2; color: %3; }")
            .arg(ChatTheme::border(), ChatTheme::borderStrong(), ChatTheme::textMuted()));
        return;
    }
    if (m_settings.reasoningEffort.isEmpty()) {
        m_reasoningEffort->setStyleSheet(
            QStringLiteral(
                "QPushButton {"
                "  background-color: %1;"
                "  color: %2;"
                "  border: 1px solid %3;"
                "  border-radius: 13px;"
                "  padding: 3px 10px;"
                "  font-size: 11px;"
                "}"
                "QPushButton:hover { background-color: %4; }")
            .arg(ChatTheme::cardBg(), ChatTheme::textMuted(), ChatTheme::border(), ChatTheme::hoverBg()));
    } else {
        m_reasoningEffort->setStyleSheet(
            QStringLiteral(
                "QPushButton {"
                "  background-color: %1;"
                "  color: #ffffff;"
                "  border: 1px solid %2;"
                "  border-radius: 13px;"
                "  padding: 3px 10px;"
                "  font-size: 11px;"
                "  font-weight: 600;"
                "}"
                "QPushButton:hover { background-color: %2; }")
            .arg(ChatTheme::accent(), ChatTheme::accentHover()));
    }
}

bool ChatWidget::modelSupportsReasoningEffort() const
{
    const QString model = modelFor(m_settings).toLower();
    const Provider provider = m_settings.provider;

    // Model names are no longer matched against a hard-coded list. The only
    // signal used here is the fetched catalogue: if the provider listed the
    // model, and its name advertises reasoning, offer the control. A model the
    // catalogue has not returned simply does not get the menu, which is the
    // honest answer rather than a guess from a stale list.
    const QStringList models = m_modelCatalog.value(provider);
    const QString wanted = model.toLower();
    for (const QString &candidate : models) {
        if (candidate.toLower() != wanted) {
            continue;
        }
        const QString lower = candidate.toLower();
        return lower.contains(u"reason"_s) || lower.startsWith(u"o1"_s) || lower.startsWith(u"o3"_s)
            || lower.startsWith(u"o4"_s) || lower.contains(u"think"_s);
    }

    return false;
}

void ChatWidget::showReasoningEffortMenu()
{
    if (!m_reasoningEffort || !modelSupportsReasoningEffort()) {
        return;
    }

    QMenu menu(this);
    menu.setStyleSheet(
        u"QMenu {"
        u"  background-color: #252528;"
        u"  color: #cccccc;"
        u"  border: 1px solid #3c3c40;"
        u"  border-radius: 6px;"
        u"  padding: 4px;"
        u"}"
        u"QMenu::item {"
        u"  padding: 6px 18px 6px 12px;"
        u"  border-radius: 4px;"
        u"}"
        u"QMenu::item:selected {"
        u"  background-color: #007acc;"
        u"  color: #ffffff;"
        u"}"
        u"QMenu::separator {"
        u"  height: 1px;"
        u"  background-color: #38383e;"
        u"  margin: 4px 0;"
        u"}"_s);

    auto *reasoningGroup = new QActionGroup(this);
    const QStringList reasoningLevels = {QString(), QStringLiteral("minimal"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
    const QStringList reasoningLabels = {i18n("Auto (provider default)"), i18n("Minimal"), i18n("Low"), i18n("Medium"), i18n("High")};
    const QStringList reasoningIcons = {u"🧠"_s, u"1"_s, u"2"_s, u"3"_s, u"4"_s};

    for (int i = 0; i < reasoningLevels.size(); ++i) {
        auto *action = menu.addAction(reasoningIcons[i] + u"  "_s + reasoningLabels[i]);
        action->setCheckable(true);
        action->setChecked(m_settings.reasoningEffort == reasoningLevels[i]);
        action->setData(reasoningLevels[i]);
        reasoningGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, effort = reasoningLevels[i]]() {
            m_settings.reasoningEffort = effort;
            updateReasoningEffortButton();
            updateModelSelectorLabel();
            m_agent.setSettings(m_settings);
            Q_EMIT settingsChanged(m_settings);
        });
    }

    menu.exec(m_reasoningEffort->mapToGlobal(QPoint(0, m_reasoningEffort->height() + 2)));
}

void ChatWidget::showModelMenu()
{
    if (m_modelMenu) {
        m_modelMenu->deleteLater();
    }
    m_modelMenuProviderMenus.clear();
    m_modelMenuFlatActions.clear();
    m_modelMenuNoMatchAction = nullptr;
    m_modelFilter.clear();

    m_modelMenu = new QMenu(this);
    m_modelFilterEdit = nullptr;
    m_modelMenu->setStyleSheet(
        u"QMenu {"
        u"  background-color: #252528;"
        u"  color: #cccccc;"
        u"  border: 1px solid #3c3c40;"
        u"  border-radius: 6px;"
        u"  padding: 4px;"
        u"}"
        u"QMenu::item {"
        u"  padding: 6px 18px 6px 12px;"
        u"  border-radius: 4px;"
        u"}"
        u"QMenu::item:selected {"
        u"  background-color: #007acc;"
        u"  color: #ffffff;"
        u"}"
        u"QMenu::separator {"
        u"  height: 1px;"
        u"  background-color: #38383e;"
        u"  margin: 4px 0;"
        u"}"_s);

    auto *filterEdit = new QLineEdit(m_modelMenu);
    m_modelFilterEdit = filterEdit;
    // Reset the keyboard cursor: each time the menu is opened it starts at "no
    // entry highlighted", so Down always starts from the top of the list rather
    // than resuming wherever the last selection was.
    m_modelMenuSelection = -1;
    filterEdit->setPlaceholderText(i18n("Filter models..."));
    filterEdit->setClearButtonEnabled(true);
    // Width is left to the menu's own size hint plus the fixed minimum applied in
    // rebuildModelMenuProviderSubmenus(). A hard 300px here fought that minimum:
    // with a short provider list the box overflowed the popup, and with a long one
    // it capped the width the menu was trying to grow to.
    filterEdit->setMinimumWidth(220);
    filterEdit->setStyleSheet(
        u"QLineEdit {"
        u"  background-color: #1a1a1a;"
        u"  color: #e4e4e4;"
        u"  border: 1px solid #38383e;"
        u"  border-radius: 4px;"
        u"  padding: 6px 10px;"
        u"  font-size: 12px;"
        u"}"
        u"QLineEdit:focus {"
        u"  border-color: #007acc;"
        u"}"_s);
    connect(filterEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_modelFilter = text;
        applyModelMenuFilter();
    });
    connect(filterEdit, &QLineEdit::returnPressed, this, [this]() {
        for (QAction *act : m_modelMenuFlatActions) {
            if (act->isVisible() && act->isEnabled()) {
                act->trigger();
                return;
            }
        }
        for (QMenu *pMenu : m_modelMenuProviderMenus) {
            if (!pMenu->menuAction()->isVisible()) {
                continue;
            }
            for (QAction *act : pMenu->actions()) {
                if (act->isVisible() && act->isEnabled() && act->isCheckable()) {
                    act->trigger();
                    return;
                }
            }
        }
    });
    // Up/Down walks the matches without leaving the filter box. The QLineEdit
    // swallows arrow keys, so without this a long model list could only be
    // walked by retyping a filter after every selection.
    auto *nextModel = new QShortcut(QKeySequence(Qt::Key_Down), m_modelMenu);
    nextModel->setContext(Qt::WidgetWithChildrenShortcut);
    connect(nextModel, &QShortcut::activated, this, [this]() {
        moveModelMenuSelection(1);
    });
    auto *prevModel = new QShortcut(QKeySequence(Qt::Key_Up), m_modelMenu);
    prevModel->setContext(Qt::WidgetWithChildrenShortcut);
    connect(prevModel, &QShortcut::activated, this, [this]() {
        moveModelMenuSelection(-1);
    });
    auto *filterAction = new QWidgetAction(m_modelMenu);
    filterAction->setDefaultWidget(filterEdit);
    m_modelMenu->addAction(filterAction);
    m_modelMenu->addSeparator();

    rebuildModelMenuProviderSubmenus();
    applyModelMenuFilter();

    connect(m_modelMenu, &QMenu::aboutToHide, this, [this]() {
        m_modelFilter.clear();
        m_modelMenuProviderMenus.clear();
        m_modelMenuFlatActions.clear();
        m_modelMenuNoMatchAction = nullptr;
        m_modelFilterEdit = nullptr;
        m_modelMenu->deleteLater();
        m_modelMenu = nullptr;
    });

    filterEdit->setFocus(Qt::ActiveWindowFocusReason);
    m_modelMenu->exec(m_modelSelector->mapToGlobal(QPoint(0, m_modelSelector->height() + 2)));
}

void ChatWidget::rebuildModelMenuProviderSubmenus()
{
    if (!m_modelMenu) {
        return;
    }

    m_modelMenuProviderMenus.clear();
    m_modelMenuFlatActions.clear();
    m_modelMenuNoMatchAction = nullptr;

    const QList<Provider> providers = {
        Provider::Grok,
        Provider::OpenAI,
        Provider::OpenRouter,
        Provider::DeepSeek,
        Provider::OpenAICompatible,
        Provider::ClaudeCompatible,
        Provider::Kilo,
        Provider::Acp,
        Provider::OpenCode
    };

    const QString currentModel = modelFor(m_settings);

    for (Provider p : providers) {
        Settings providerSettings = m_settings;
        providerSettings.provider = p;
        if (apiKeyFor(providerSettings).trimmed().isEmpty()) {
            continue;
        }

        auto *pMenu = m_modelMenu->addMenu(providerLabel(p));
        pMenu->setStyleSheet(m_modelMenu->styleSheet());
        m_modelMenuProviderMenus.append(pMenu);

        const QStringList models = m_modelCatalog.value(p);
        if (models.isEmpty()) {
            auto *act = pMenu->addAction(i18n("Fetching models..."));
            act->setEnabled(false);
            act->setData(QStringLiteral("__placeholder__"));
        } else {
            for (const QString &m : models) {
                auto *act = pMenu->addAction(m);
                act->setCheckable(true);
                act->setChecked(m_settings.provider == p && currentModel == m);
                connect(act, &QAction::triggered, this, [this, p, m]() {
                    selectModel(p, m);
                });

                auto *flat = new QAction(u"%1  ·  %2"_s.arg(providerLabel(p), m), m_modelMenu);
                flat->setCheckable(true);
                flat->setChecked(m_settings.provider == p && currentModel == m);
                flat->setVisible(false);
                flat->setProperty("kateai_model", m);
                connect(flat, &QAction::triggered, this, [this, p, m]() {
                    selectModel(p, m);
                });
                m_modelMenuFlatActions.append(flat);
            }
        }
    }

    for (QAction *flat : m_modelMenuFlatActions) {
        m_modelMenu->addAction(flat);
    }

    m_modelMenuNoMatchAction = m_modelMenu->addAction(i18n("No matching models"));
    m_modelMenuNoMatchAction->setEnabled(false);
    m_modelMenuNoMatchAction->setVisible(false);

    m_modelMenu->addSeparator();

    auto *reasoningMenu = m_modelMenu->addMenu(i18n("Reasoning Effort"));
    reasoningMenu->setStyleSheet(m_modelMenu->styleSheet());
    auto *reasoningGroup = new QActionGroup(this);
    const QStringList reasoningLevels = {QString(), QStringLiteral("minimal"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
    const QStringList reasoningLabels = {i18n("Default (Auto)"), i18n("Minimal"), i18n("Low"), i18n("Medium"), i18n("High")};
    for (int i = 0; i < reasoningLevels.size(); ++i) {
        auto *action = reasoningMenu->addAction(reasoningLabels[i]);
        action->setCheckable(true);
        action->setChecked(m_settings.reasoningEffort == reasoningLevels[i]);
        action->setData(reasoningLevels[i]);
        reasoningGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, effort = reasoningLevels[i]]() {
            m_settings.reasoningEffort = effort;
            updateModelSelectorLabel();
            updateReasoningEffortButton();
            m_agent.setSettings(m_settings);
            Q_EMIT settingsChanged(m_settings);
        });
    }

    auto *configAct = m_modelMenu->addAction(i18n("Configure Providers & Models…"));
    connect(configAct, &QAction::triggered, this, &ChatWidget::configureRequested);

    // Hidden flat actions do not contribute to QMenu's size hint, so pin the
    // popup width to the widest entry once. Without this the menu resizes on
    // every keystroke and the filter box visibly jumps around. The floor is the
    // filter box's own minimum so it can never be clipped by a narrow catalogue.
    int widest = m_modelFilterEdit ? m_modelFilterEdit->minimumSizeHint().width() : 0;
    const QFontMetrics fm(m_modelMenu->font());
    for (QAction *act : m_modelMenu->actions()) {
        widest = qMax(widest, fm.horizontalAdvance(act->text()));
    }
    m_modelMenu->setMinimumWidth(qBound(240, widest + 24, 560));
}

void ChatWidget::applyModelMenuFilter()
{
    if (!m_modelMenu) {
        return;
    }

    const QString filter = m_modelFilter.trimmed();
    const bool filtering = !filter.isEmpty();
    int visibleMatches = 0;

    // Every visibility toggle makes QMenu recalculate its action rects and
    // repaint; batching them behind frozen updates stops the filter box from
    // flickering while typing.
    m_modelMenu->setUpdatesEnabled(false);

    for (QMenu *pMenu : m_modelMenuProviderMenus) {
        pMenu->menuAction()->setVisible(!filtering);
    }

    for (QAction *act : m_modelMenuFlatActions) {
        if (!filtering) {
            act->setVisible(false);
            continue;
        }
        const QString model = act->property("kateai_model").toString();
        const bool match = act->text().contains(filter, Qt::CaseInsensitive)
            || model.contains(filter, Qt::CaseInsensitive);
        act->setVisible(match);
        if (match) {
            ++visibleMatches;
        }
    }

    if (m_modelMenuNoMatchAction) {
        m_modelMenuNoMatchAction->setVisible(filtering && visibleMatches == 0);
    }

    m_modelMenu->setUpdatesEnabled(true);
    m_modelMenu->update();

    // QMenu hands focus back to itself when the action set changes; keep the
    // caret in the filter box so typing is never interrupted. Only while the
    // user is actually filtering: once they have arrowed onto an entry, taking
    // the caret back would fight the selection they are making.
    if (m_modelFilterEdit && !m_modelFilterEdit->hasFocus() && m_modelMenu->isVisible()
        && m_modelMenuSelection < 0) {
        m_modelFilterEdit->setFocus(Qt::OtherFocusReason);
    }
}

void ChatWidget::moveModelMenuSelection(int delta)
{
    if (!m_modelMenu) {
        return;
    }
    // The candidate list depends on whether a filter is active: unfiltered, the
    // entries are the provider submenus; filtered, they are the flat actions.
    QList<QAction *> candidates;
    if (!m_modelFilter.trimmed().isEmpty()) {
        for (QAction *act : m_modelMenuFlatActions) {
            if (act->isVisible()) {
                candidates.append(act);
            }
        }
    } else {
        for (QMenu *pMenu : m_modelMenuProviderMenus) {
            if (pMenu->menuAction()->isVisible()) {
                candidates.append(pMenu->menuAction());
            }
        }
    }
    if (candidates.isEmpty()) {
        return;
    }

    const int count = candidates.size();
    m_modelMenuSelection = m_modelMenuSelection < 0
        ? (delta > 0 ? 0 : count - 1)
        : (m_modelMenuSelection + delta + count) % count;
    m_modelMenu->setActiveAction(candidates.at(m_modelMenuSelection));
}

void ChatWidget::selectModel(Provider provider, const QString &model)
{
    m_settings.provider = provider;
    m_preferredProvider = provider;
    switch (provider) {
    case Provider::OpenAI:
        m_settings.openaiModel = model;
        break;
    case Provider::OpenRouter:
        m_settings.openrouterModel = model;
        break;
    case Provider::DeepSeek:
        m_settings.deepseekModel = model;
        break;
    case Provider::OpenAICompatible:
        m_settings.openaiCompatibleModel = model;
        break;
    case Provider::ClaudeCompatible:
        m_settings.claudeCompatibleModel = model;
        break;
    case Provider::OpenCode:
        m_settings.opencodeModel = model;
        break;
    case Provider::Kilo:
        m_settings.kiloModel = model;
        break;
    case Provider::Acp:
        m_settings.acpModel = model;
        break;
    case Provider::Grok:
    default:
        m_settings.grokModel = model;
        break;
    }
    updateModelSelectorLabel();
    updateTokenDisplay();
    applyProviderToCombos();
    updateReasoningEffortButton();
    m_agent.setSettings(m_settings);
    Q_EMIT settingsChanged(m_settings);
    if (m_modelMenu) {
        m_modelMenu->close();
    }
}

void ChatWidget::showSettingsMenu()
{
    QMenu menu(this);
    menu.setStyleSheet(
        u"QMenu {"
        u"  background-color: #252528;"
        u"  color: #cccccc;"
        u"  border: 1px solid #3c3c40;"
        u"  border-radius: 6px;"
        u"  padding: 4px;"
        u"}"
        u"QMenu::item {"
        u"  padding: 6px 18px 6px 12px;"
        u"  border-radius: 4px;"
        u"}"
        u"QMenu::item:selected {"
        u"  background-color: #007acc;"
        u"  color: #ffffff;"
        u"}"
        u"QMenu::separator {"
        u"  height: 1px;"
        u"  background-color: #38383e;"
        u"  margin: 4px 0;"
        u"}"_s);

    // Permission Mode
    auto *permMenu = menu.addMenu(i18n("Permission Mode"));
    permMenu->setStyleSheet(menu.styleSheet());
    auto *permGroup = new QActionGroup(this);
    for (int i = 0; i < m_permission->count(); ++i) {
        auto *action = permMenu->addAction(m_permission->itemText(i));
        action->setCheckable(true);
        action->setChecked(m_permission->currentIndex() == i);
        action->setData(i);
        permGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, i]() {
            m_permission->setCurrentIndex(i);
        });
    }

    // Sandbox Profile
    auto *sandboxMenu = menu.addMenu(i18n("Sandbox Profile"));
    sandboxMenu->setStyleSheet(menu.styleSheet());
    auto *sandboxGroup = new QActionGroup(this);
    for (int i = 0; i < m_sandbox->count(); ++i) {
        auto *action = sandboxMenu->addAction(m_sandbox->itemText(i));
        action->setCheckable(true);
        action->setChecked(m_sandbox->currentIndex() == i);
        action->setData(i);
        sandboxGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, i]() {
            m_sandbox->setCurrentIndex(i);
        });
    }

    // Plan Mode
    auto *planAction = menu.addAction(i18n("Plan Mode (Read-only)"));
    planAction->setCheckable(true);
    planAction->setChecked(m_settings.planMode);
    connect(planAction, &QAction::triggered, this, [this](bool checked) {
        m_settings.planMode = checked;
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
        refreshModeButton();
    });

    // Agent Mode
    auto *modeMenu = menu.addMenu(i18n("Mode"));
    modeMenu->setStyleSheet(menu.styleSheet());
    auto *modeGroup = new QActionGroup(this);
    for (int i = 0; i < m_mode->count(); ++i) {
        auto *action = modeMenu->addAction(m_mode->itemText(i));
        action->setCheckable(true);
        action->setChecked(m_mode->currentIndex() == i);
        modeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, i]() {
            m_mode->setCurrentIndex(i);
        });
    }
    const QStringList customModes = m_agent.modeRegistry()->customModeIds();
    if (!customModes.isEmpty()) {
        modeMenu->addSeparator();
        for (const QString &id : customModes) {
            const int index = m_mode->findData(id);
            if (index < 0) {
                continue;
            }
            auto *action = modeMenu->addAction(m_mode->itemText(index));
            action->setCheckable(true);
            action->setChecked(m_mode->currentIndex() == index);
            modeGroup->addAction(action);
            connect(action, &QAction::triggered, this, [this, index]() {
                m_mode->setCurrentIndex(index);
            });
        }
    }

    // Auto-approve tools that still prompt
    auto *autoApproveMenu = menu.addMenu(i18n("Auto-approve tools"));
    autoApproveMenu->setStyleSheet(menu.styleSheet());
    for (const QString &tool : allBuiltInToolNames()) {
        auto *action = autoApproveMenu->addAction(tool);
        action->setCheckable(true);
        action->setChecked(m_settings.autoApproveTools.contains(tool));
        connect(action, &QAction::triggered, this, [this, tool, action]() {
            applyAutoApproveTool(tool, action->isChecked());
        });
    }
    autoApproveMenu->addSeparator();
    if (m_agent.mcpManager()) {
        for (const McpTool &tool : m_agent.mcpManager()->tools()) {
            const QString qualified = tool.qualifiedName();
            auto *action = autoApproveMenu->addAction(qualified);
            action->setCheckable(true);
            action->setChecked(m_agent.mcpManager()->autoApprovedTools().contains(qualified));
            connect(action, &QAction::triggered, this, [this, action, tool] {
                setMcpToolAutoApproved(tool, action->isChecked());
            });
        }
    }

    // Thinking Mode
    auto *thinkingAction = menu.addAction(i18n("Thinking Mode"));
    thinkingAction->setCheckable(true);
    thinkingAction->setChecked(m_settings.thinkingMode);
    connect(thinkingAction, &QAction::triggered, this, [this](bool checked) {
        m_thinking->setChecked(checked);
    });

    menu.addSeparator();
    auto *fullSettingsAction = menu.addAction(i18n("Full Configuration…"));
    connect(fullSettingsAction, &QAction::triggered, this, &ChatWidget::configureRequested);

    menu.exec(m_configure->mapToGlobal(QPoint(0, m_configure->height() + 2)));
}

void ChatWidget::populateModes()
{
    const QString previous = m_mode->currentData().toString();
    m_updatingCombos = true;
    m_mode->clear();
    for (const ModeDefinition &mode : m_agent.modeRegistry()->modes()) {
        m_mode->addItem(mode.name, mode.id);
    }
    int index = previous.isEmpty() ? -1 : m_mode->findData(previous);
    if (index < 0) {
        index = m_mode->findData(m_settings.agentMode);
    }
    if (index < 0) {
        index = m_mode->findData(QStringLiteral("code"));
    }
    m_mode->setCurrentIndex(qMax(0, index));
    m_updatingCombos = false;
}

void ChatWidget::refreshModeButton()
{
    const ModeDefinition mode = m_agent.modeRegistry()->modeOrDefault(m_settings.agentMode);
    QString label = mode.name;
    if (m_settings.planMode) {
        label += i18n(" · Plan");
    }
    m_modeButton->setText(label);
    m_modeButton->setToolTip(mode.description.isEmpty() ? i18n("Agent mode") : mode.description);
}

void ChatWidget::refreshPermissionButton()
{
    if (!m_permissionButton) {
        return;
    }
    m_permissionButton->setText(permissionModeLabel(m_settings.permissionMode));
}

void ChatWidget::showPermissionMenu()
{
    QMenu menu(this);
    menu.setStyleSheet(
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"_s);

    auto *group = new QActionGroup(&menu);
    for (int i = 0; i < m_permission->count(); ++i) {
        QAction *action = menu.addAction(m_permission->itemText(i));
        action->setCheckable(true);
        action->setChecked(m_permission->currentIndex() == i);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, i]() {
            m_permission->setCurrentIndex(i);
        });
    }
    menu.exec(m_permissionButton->mapToGlobal(QPoint(0, -m_permissionButton->height())));
}

void ChatWidget::applyMode(const QString &modeId)
{
    if (modeId.isEmpty() || modeId == m_settings.agentMode) {
        return;
    }
    m_settings.agentMode = modeId;
    m_agent.setMode(modeId);
    m_agent.setSettings(m_settings);
    refreshModeButton();
    Q_EMIT settingsChanged(m_settings);
    showInfoMessage(i18n("Mode: %1", m_agent.activeMode().name), false);
}

void ChatWidget::applyAutoApproveTool(const QString &toolName, bool enabled)
{
    if (enabled) {
        if (!m_settings.autoApproveTools.contains(toolName)) {
            m_settings.autoApproveTools.append(toolName);
        }
    } else {
        m_settings.autoApproveTools.removeAll(toolName);
    }
    m_agent.setSettings(m_settings);
    Q_EMIT settingsChanged(m_settings);
}

void ChatWidget::setMcpToolAutoApproved(const McpTool &tool, bool enabled)
{
    McpManager *manager = m_agent.mcpManager();
    if (!manager) {
        return;
    }
    QList<McpServerConfig> updated = manager->servers();
    for (McpServerConfig &config : updated) {
        if (config.name != tool.server) {
            continue;
        }
        if (enabled) {
            if (!config.alwaysAllow.contains(tool.name)) {
                config.alwaysAllow.append(tool.name);
            }
        } else {
            config.alwaysAllow.removeAll(tool.name);
        }
    }
    manager->saveToDisk();
    manager->setServers(updated);
}

void ChatWidget::showModeMenu()
{
    QMenu menu(this);
    menu.setStyleSheet(
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
        u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);

    auto *group = new QActionGroup(&menu);
    const QList<ModeDefinition> modes = m_agent.modeRegistry()->modes();
    bool addedSeparator = false;
    for (const ModeDefinition &mode : modes) {
        if (!mode.builtIn && !addedSeparator) {
            menu.addSeparator();
            addedSeparator = true;
        }
        QAction *action = menu.addAction(mode.icon.isEmpty() ? mode.name : QStringLiteral("%1  %2").arg(mode.icon, mode.name));
        action->setCheckable(true);
        action->setChecked(mode.id == m_settings.agentMode);
        action->setToolTip(mode.description);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, id = mode.id] {
            applyMode(id);
        });
    }

    menu.addSeparator();
    QAction *planAction = menu.addAction(i18n("Plan Mode (Read-only)"));
    planAction->setCheckable(true);
    planAction->setChecked(m_settings.planMode);
    connect(planAction, &QAction::triggered, this, [this](bool checked) {
        m_settings.planMode = checked;
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
        refreshModeButton();
    });

    const QStringList customModes = m_agent.modeRegistry()->customModeIds();
    if (!customModes.isEmpty()) {
        menu.addSeparator();
        QAction *manage = menu.addAction(i18n("Custom modes live in .kateai/modes/"));
        manage->setEnabled(false);
    }

    menu.exec(m_modeButton->mapToGlobal(QPoint(0, m_modeButton->height() + 2)));
}

void ChatWidget::updateTeamButton()
{
    const int running = m_agent.runningSubtaskCount();
    m_teamButton->setText(running > 0 ? QStringLiteral("%1 running").arg(running) : QString());
    m_teamButton->setToolTip(running > 0 ? i18np("%1 sub-agent running", "%1 sub-agents running", running) : i18n("Agent team"));
    // Only lit while something is actually running, so a badge that is always
    // on stops meaning anything.
    m_teamButton->setStyleSheet(
        running > 0
            ? QStringLiteral(
                  "QPushButton { background: transparent; border: 1px solid transparent; border-radius: 6px;"
                  " color: %1; padding: 0 7px; font-size: 11px; }"
                  "QPushButton:hover { background-color: %2; border-color: %3; }")
                  .arg(ChatTheme::accent(), ChatTheme::hoverBg(), ChatTheme::border())
            : ChatTheme::iconButton());
}

void ChatWidget::markRunningSubtasksAbandoned()
{
    // A turn can end with sub-agents still in flight (abort, budget stop).
    // Their cards must not be left showing a running spinner forever.
    bool changed = false;
    const QStringList ids = m_agent.runningSubtaskIds();
    for (const QString &taskId : ids) {
        if (auto *widget = m_subtaskWidgets.value(taskId)) {
            widget->markAbandoned(i18n("Stopped when the turn ended."));
            // Abandoned means over; the card belongs back in the transcript.
            retireIntentWidget(widget);
            changed = true;
        }
    }
    if (changed) {
        updateTeamButton();
    }
}

void ChatWidget::showTeamMenu()
{
    QMenu menu(this);
    menu.setStyleSheet(
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
        u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);

    const int limit = qMax(1, m_settings.maxParallelSubtasks);
    QAction *header = menu.addAction(i18n("Sub-agents: %1 of %2 slots in use", m_agent.runningSubtaskCount(), limit));
    header->setEnabled(false);

    // A slot is free only when no card is currently marked running.
    int busyCards = 0;
    for (auto it = m_subtaskWidgets.constBegin(); it != m_subtaskWidgets.constEnd(); ++it) {
        if (it.value() && it.value()->isRunning()) {
            ++busyCards;
        }
    }
    if (busyCards > 0) {
        menu.addSeparator();
        QAction *cancelAll = menu.addAction(i18n("Cancel all running sub-agents"));
        connect(cancelAll, &QAction::triggered, this, [this] {
            m_agent.cancelSubtask();
            markRunningSubtasksAbandoned();
            updateTeamButton();
        });
    }

    menu.addSeparator();
    const AgentTeam *team = m_agent.agentTeam();
    QAction *rosterHeader = menu.addAction(i18n("Roster"));
    rosterHeader->setEnabled(false);
    for (const AgentProfile &profile : team->agents()) {
        QAction *action = menu.addAction(QStringLiteral("  %1  ·  %2").arg(profile.name, profile.modeId));
        action->setEnabled(false);
        action->setToolTip(profile.description);
    }

    menu.addSeparator();
    QAction *configure = menu.addAction(i18n("Configure team…"));
    connect(configure, &QAction::triggered, this, &ChatWidget::configureRequested);

    menu.exec(m_teamButton->mapToGlobal(QPoint(0, m_teamButton->height() + 2)));
}

void ChatWidget::showMcpMenu()
{
    QMenu menu(this);
    menu.setStyleSheet(
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
        u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);

    McpManager *manager = m_agent.mcpManager();
    if (!manager) {
        menu.exec(m_mcpButton->mapToGlobal(QPoint(0, m_mcpButton->height() + 2)));
        return;
    }

    QAction *enableAction = menu.addAction(i18n("MCP enabled"));
    enableAction->setCheckable(true);
    enableAction->setChecked(manager->isEnabled());
    connect(enableAction, &QAction::triggered, this, [this, manager](bool checked) {
        m_settings.mcpEnabled = checked;
        manager->setEnabled(checked);
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
        updateMcpButton();
    });

    const QList<McpServerConfig> servers = manager->servers();
    if (servers.isEmpty()) {
        QAction *empty = menu.addAction(i18n("No MCP servers configured"));
        empty->setEnabled(false);
    } else {
        menu.addSeparator();
        for (const McpServerConfig &config : servers) {
            QAction *action = menu.addAction(QStringLiteral("%1  ·  %2").arg(config.name, config.transportId()));
            action->setCheckable(true);
            action->setChecked(config.enabled);
            action->setToolTip(config.command.isEmpty() ? config.url : config.command);
            connect(action, &QAction::triggered, this, [this, manager, name = config.name](bool checked) {
                QList<McpServerConfig> updated = manager->servers();
                for (McpServerConfig &entry : updated) {
                    if (entry.name == name) {
                        entry.enabled = checked;
                    }
                }
                manager->setServers(updated);
            });
        }
    }

    menu.addSeparator();
    QAction *reload = menu.addAction(i18n("Reconnect all servers"));
    connect(reload, &QAction::triggered, this, [manager] {
        manager->disconnectAll();
        manager->connectAll();
    });
    QAction *configure = menu.addAction(i18n("Configure servers…"));
    connect(configure, &QAction::triggered, this, &ChatWidget::configureRequested);

    menu.exec(m_mcpButton->mapToGlobal(QPoint(0, m_mcpButton->height() + 2)));
}

void ChatWidget::updateMcpButton()
{
    McpManager *manager = m_agent.mcpManager();
    if (!manager) {
        m_mcpButton->setText(QString());
        return;
    }
    const QString summary = manager->statusSummary();
    m_mcpButton->setText(manager->readyServers().isEmpty() ? QString() : QStringLiteral("●"));
    m_mcpButton->setToolTip(summary);
    m_mcpButton->setStyleSheet(
        manager->readyServers().isEmpty()
            ? u"QPushButton { background: transparent; border: 1px solid transparent; border-radius: 4px; color: #888888; }"
              u"QPushButton:hover { background-color: #2e2e32; border-color: #3c3c40; }"_s
            : u"QPushButton { background: transparent; border: 1px solid transparent; border-radius: 4px; color: #22c55e; }"
              u"QPushButton:hover { background-color: #2e2e32; border-color: #3c3c40; }"_s);
}

void ChatWidget::showCheckpointMenu()
{
    QMenu menu(this);
    menu.setStyleSheet(
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
        u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);

    if (!m_settings.checkpointsEnabled) {
        QAction *off = menu.addAction(i18n("Checkpoints are disabled"));
        off->setEnabled(false);
        QAction *enable = menu.addAction(i18n("Enable checkpoints"));
        connect(enable, &QAction::triggered, this, [this] {
            m_settings.checkpointsEnabled = true;
            m_agent.setSettings(m_settings);
            Q_EMIT settingsChanged(m_settings);
        });
        menu.exec(m_checkpointButton->mapToGlobal(QPoint(0, m_checkpointButton->height() + 2)));
        return;
    }

    QAction *create = menu.addAction(i18n("Create checkpoint now"));
    connect(create, &QAction::triggered, this, [this] {
        const QString label = i18n("Manual checkpoint");
        QString error;
        const QString id = m_agent.createCheckpoint(label);
        if (id.isEmpty()) {
            showInfoMessage(i18n("Could not create a checkpoint: %1", error.isEmpty() ? i18n("git is unavailable") : error), true);
        } else {
            showInfoMessage(i18n("Checkpoint created."), false);
        }
    });

    const QList<CheckpointInfo> checkpoints = m_agent.checkpoints();
    if (checkpoints.isEmpty()) {
        QAction *empty = menu.addAction(i18n("No checkpoints yet"));
        empty->setEnabled(false);
    } else {
        menu.addSeparator();
        for (const CheckpointInfo &info : checkpoints) {
            const QString stamp = info.createdAt.isValid()
                ? info.createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                : info.shortId;
            QAction *action = menu.addAction(QStringLiteral("%1  ·  %2").arg(info.shortId, stamp));
            action->setToolTip(info.label);
            connect(action, &QAction::triggered, this, [this, info] {
                showCheckpointMenuFor(info);
            });
        }
    }

    menu.exec(m_checkpointButton->mapToGlobal(QPoint(0, m_checkpointButton->height() + 2)));
}

void ChatWidget::showCheckpointMenuFor(const CheckpointInfo &info)
{
    QMenu menu(this);
    menu.setStyleSheet(
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"_s);

    QAction *viewDiff = menu.addAction(i18n("Show changes since this checkpoint"));
    connect(viewDiff, &QAction::triggered, this, [this, info] {
        const QString diff = m_agent.diffAgainstCheckpoint(info.id);
        if (diff.trimmed().isEmpty()) {
            showInfoMessage(i18n("No changes since this checkpoint."), false);
        } else {
            addActivityMessage(diff);
            forceScrollToBottom();
        }
    });

    QAction *restore = menu.addAction(i18n("Restore files to this checkpoint…"));
    restore->setEnabled(!m_agent.isBusy());
    connect(restore, &QAction::triggered, this, [this, info] {
        QMessageBox::StandardButton answer = QMessageBox::question(this,
                                                                   i18n("Restore checkpoint"),
                                                                   i18n("Restore the workspace to checkpoint %1?\n\n"
                                                                       "Files changed since then will be overwritten and files "
                                                                       "added afterwards will be removed. Your own git "
                                                                       "history is not touched.")
                                                                       .arg(info.shortId),
                                                                   QMessageBox::Yes | QMessageBox::No,
                                                                   QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
        QString error;
        if (m_agent.restoreCheckpoint(info.id, &error)) {
            showInfoMessage(i18n("Restored checkpoint %1.", info.shortId), false);
        } else {
            showInfoMessage(i18n("Restore failed: %1", error), true);
        }
    });

    menu.exec(QCursor::pos());
}


void ChatWidget::submit()
{
    const QString text = m_prompt->toPlainText().trimmed();
    if (text.isEmpty() || m_agent.isBusy()) {
        return;
    }
    m_prompt->addHistory(text);
    Q_EMIT aboutToSubmit();
    m_prompt->clear();
    if (m_infoBar) {
        m_infoBar->hide();
    }

    // Start the live counters for this turn.
    m_completedToolCount = 0;
    m_turnStatus->reset();
    m_turnStatus->setBusy(true);
    forceScrollToBottom();
    updateSendButtonState();
    m_agent.start(text);
    updateSendButtonState();
}

bool ChatWidget::m_approvalPending() const
{
    for (auto *widget : m_toolCallWidgets) {
        if (widget && widget->isAwaitingApproval()) {
            return true;
        }
    }
    return false;
}

void ChatWidget::updateSendButtonState()
{
    const bool busy = m_agent.isBusy();
    const bool promptEmpty = m_prompt && m_prompt->toPlainText().trimmed().isEmpty();
    const bool canClick = busy || !promptEmpty;

    m_send->setEnabled(canClick);

    // One round button that morphs between send and stop, the way an inline
    // chat composer does: the position never moves, so the target is learned
    // once. Glyphs are drawn rather than emoji so they stay centred at any DPI.
    const QString glyph = busy ? QStringLiteral("■") : QStringLiteral("▲");
    const int glyphSize = busy ? 11 : 13;
    const QString radius = QStringLiteral("border-radius: 15px;");

    if (busy) {
        m_send->setText(glyph);
        m_send->setStyleSheet(
            QStringLiteral("QPushButton { color: #ffffff; background-color: %1; font-size: %2px; border: none; %3 }"
                           "QPushButton:hover { background-color: %4; }")
                .arg(ChatTheme::danger(), QString::number(glyphSize), radius, ChatTheme::accentHover()));
        m_send->setToolTip(i18n("Stop response"));
    } else {
        m_send->setText(glyph);
        if (canClick) {
            m_send->setStyleSheet(
                QStringLiteral("QPushButton { color: #ffffff; background-color: %1; font-size: %2px; border: none; %3 }"
                               "QPushButton:hover { background-color: %4; }")
                    .arg(ChatTheme::accent(), QString::number(glyphSize), radius, ChatTheme::accentHover()));
        } else {
            m_send->setStyleSheet(
                QStringLiteral("QPushButton { color: #55555c; background-color: %1; font-size: %2px;"
                               " border: 1px solid %3; %4 }")
                    .arg(ChatTheme::hoverBg(), QString::number(glyphSize), ChatTheme::border(), radius));
        }
        m_send->setToolTip(i18n("Send message"));
    }
}

void ChatWidget::focusPrompt()
{
    m_prompt->setFocus();
}

void ChatWidget::ask(const QString &text)
{
    m_prompt->setPlainText(text);
    submit();
}

QString ChatWidget::escape(const QString &text)
{
    return text.toHtmlEscaped();
}

QString ChatWidget::markdownToHtml(const QString &text)
{
    return makeMarkdownDocument(text)->toHtml();
}

QString ChatWidget::closedMarkdown(const QString &text)
{
    return closeMarkdown(text);
}

void ChatWidget::scheduleStreamHeightUpdate()
{
    if (!m_streamHeightTimer) {
        m_streamHeightTimer = new QTimer(this);
        m_streamHeightTimer->setSingleShot(true);
        m_streamHeightTimer->setInterval(50);
        connect(m_streamHeightTimer, &QTimer::timeout, this, [this]() {
            if (!m_activeAssistantBrowser) {
                return;
            }
            const int docH = std::max(30, static_cast<int>(m_activeAssistantBrowser->document()->size().height()) + 16);
            // Only resize when the height actually moved. An unconditional
            // setFixedHeight() re-pins the size constraint, which invalidates the
            // whole transcript layout and repaints it - up to 20 times a second
            // while streaming, which reads as a flicker.
            if (m_activeAssistantBrowser->height() != docH) {
                m_activeAssistantBrowser->setFixedHeight(docH);
            }
            scrollToBottom();
        });
    }
    if (!m_streamHeightTimer->isActive()) {
        m_streamHeightTimer->start();
    }
}

void ChatWidget::clearStreamingPointers()
{
    stopThinkingPacer();
    m_activeAssistantWidget = nullptr;
    m_activeAssistantBrowser = nullptr;
    m_activeAssistantPulse = nullptr;
    m_thinkingBlock = nullptr;
    m_thinkingBrowser = nullptr;
    m_thinkingToggle = nullptr;
    m_planBlock = nullptr;
    m_planLayout = nullptr;
    m_thinkingBuffer.clear();
    m_thinkingPacedLength = 0;
    m_thinkingExpanded = !m_settings.autoCollapseThinking;
    m_streamText.clear();
    m_isStreaming = false;
}

void ChatWidget::resetThinkingState()
{
    m_thinkingBuffer.clear();
    m_thinkingPacedLength = 0;
    m_thinkingExpanded = !m_settings.autoCollapseThinking;
    stopThinkingPacer();
}

bool ChatWidget::isInternalUserMessage(const QString &text)
{
    return text.startsWith(u"[KateAI agent controller]"_s);
}

void ChatWidget::clearTranscriptContents()
{
    if (m_streamHeightTimer) {
        m_streamHeightTimer->stop();
    }
    stopThinkingPacer();
    if (m_indicatorTimer) {
        m_indicatorTimer->stop();
    }

    qDeleteAll(m_toolCallWidgets);
    m_toolCallWidgets.clear();
    m_toolCallOrder.clear();
    for (auto it = m_subtaskWidgets.begin(); it != m_subtaskWidgets.end(); ++it) {
        delete it.value();
    }
    m_subtaskWidgets.clear();
    m_subtaskOrder.clear();
    // Detach any approval rows still docked; they belong to the cards deleted
    // above and must not be left parented to the dock.
    clearIntentDock();
    m_thinkingBlocks.clear();
    m_planSteps.clear();
    clearStreamingPointers();
    resetThinkingState();

    if (!m_transcriptLayout) {
        return;
    }

    QWidget *indicators = m_indicatorsRow ? m_indicatorsRow.data() : nullptr;
    if (!indicators && m_thinkingIndicator) {
        indicators = m_thinkingIndicator->parentWidget();
    }
    QList<QLayoutItem *> kept;
    while (m_transcriptLayout->count() > 0) {
        QLayoutItem *item = m_transcriptLayout->takeAt(0);
        if (!item) {
            break;
        }
        if (item->spacerItem()) {
            kept.append(item);
            continue;
        }
        QWidget *w = item->widget();
        if (w && w == indicators) {
            kept.append(item);
            continue;
        }
        if (w) {
            w->hide();
            w->setParent(nullptr);
            delete w;
        }
        delete item;
    }
    for (QLayoutItem *item : kept) {
        m_transcriptLayout->addItem(item);
    }
}

void ChatWidget::reflowTranscriptMedia()
{
    if (!m_transcriptContainer) {
        return;
    }
    int contentWidth = 240;
    if (m_scrollArea && m_scrollArea->viewport()) {
        contentWidth = std::max(160, m_scrollArea->viewport()->width() - 32);
    }

    const auto browsers = m_transcriptContainer->findChildren<QTextBrowser *>(u"assistantBrowser"_s);
    for (QTextBrowser *browser : browsers) {
        if (!browser) {
            continue;
        }
        browser->document()->setTextWidth(contentWidth);
        const int docH = std::max(30, static_cast<int>(browser->document()->size().height()) + 16);
        // Only re-pin the size constraint when the height actually moved. An
        // unconditional setFixedHeight() invalidates the transcript layout and
        // repaints it, so every panel resize flickered the whole conversation.
        if (browser->height() != docH) {
            browser->setFixedHeight(docH);
        }
    }

    for (const auto &widget : m_toolCallOrder) {
        if (widget) {
            widget->reflowNow();
        }
    }
}

void ChatWidget::rebuildTranscript()
{
    m_permissionBar->hideBar();
    clearTranscriptContents();
    m_thinkingExpanded = false;

    // Recreate the indicator row if it was ever removed. clearTranscriptContents()
    // keeps it, so this is a safety net rather than the normal path, but it has to
    // be correct: the previous version assigned the new "Thinking" label to
    // m_workingIndicator and then dereferenced the still-null m_thinkingIndicator,
    // which is a null-pointer write on the one path that exists to recover from a
    // broken state.
    if (!m_thinkingIndicator || !m_workingIndicator) {
        m_indicatorsRow = createIndicatorsRow();
    }

    const auto &messages = m_agent.messages();
    if (messages.isEmpty()) {
        m_transcriptLayout->insertWidget(transcriptInsertIndex(), createWelcomeWidget());
        if (m_threadTitle) {
            m_threadTitle->setText(i18n("New Thread"));
        }
        forceScrollToBottom();
        return;
    }

    // Rebuild transcript from messages
    // Track tool call widgets by toolCallId to connect Role::Tool results
    QHash<QString, ToolCallWidget *> rebuiltToolWidgets;

    for (const auto &msg : messages) {
        switch (msg.role) {
            case ChatMessage::Role::User:
                if (!isInternalUserMessage(msg.content)) {
                    addUserMessage(msg.content);
                }
                break;
            case ChatMessage::Role::Assistant:
                // For assistant messages, recreate the widget with full content
                {
                    const bool hasVisibleText = !msg.content.trimmed().isEmpty();
                    const bool hasThinking = !msg.thinking.isEmpty();
                    if (!hasVisibleText && !hasThinking) {
                        // Tool-only turns have no Kate AI bubble; cards are rebuilt below.
                    } else {
                    auto *assistantWidget = new QWidget(m_transcriptContainer);
                    auto *layout = new QVBoxLayout(assistantWidget);
                    layout->setContentsMargins(4, 4, 4, 4);
                    layout->setSpacing(4);

                    auto *headerLayout = new QHBoxLayout;
                    headerLayout->setContentsMargins(0, 0, 0, 0);

                    auto *icon = new QLabel(u"⚡"_s, assistantWidget);
                    icon->setStyleSheet(u"color: #3b82f6; font-size: 12px;"_s);
                    headerLayout->addWidget(icon);

                    auto *label = new QLabel(i18n("KATE AI"), assistantWidget);
                    label->setStyleSheet(u"color: #3b82f6; font-size: 10px; font-weight: bold; letter-spacing: 0.5px;"_s);
                    headerLayout->addWidget(label);
                    headerLayout->addStretch();

                    auto *copyBtn = createCopyButton(msg.content, assistantWidget);
                    headerLayout->addWidget(copyBtn);
                    layout->addLayout(headerLayout);

                    if (!msg.thinking.isEmpty()) {
                        QTextBrowser *thinkingBrowser = nullptr;
                        QPushButton *thinkingToggle = nullptr;
                        auto *thinkingBlock = createThinkingBlock(assistantWidget, thinkingBrowser, thinkingToggle, false);
                        if (thinkingBrowser) {
                            thinkingBrowser->setDocument(makeMarkdownDocument(closedMarkdown(msg.thinking)));
                        }
                        thinkingBlock->show();
                        layout->addWidget(thinkingBlock);
                        registerThinkingBlock(thinkingBlock, thinkingBrowser, thinkingToggle);
                    }

                    // Add plan checklist if present
                    if (!msg.plan.isEmpty()) {
                        addPlanChecklist(msg.plan);
                    }

                    auto *browser = new QTextBrowser(assistantWidget);
                    browser->setObjectName(u"assistantBrowser"_s);
                    browser->setOpenExternalLinks(true);
                    browser->setFrameShape(QFrame::NoFrame);
                    browser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                    browser->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
                    browser->setStyleSheet(u"background: transparent; color: #d4d4d4; border: none; padding: 0px;"_s);
                    browser->document()->setDefaultStyleSheet(
                        u"body { color: #d4d4d4; font-family: sans-serif; font-size: 13px; margin: 0; padding: 0; }"
                        u"pre { background-color: #222225; color: #e4e4e4; padding: 10px 12px; border-radius: 6px; border: 1px solid #333338; font-family: monospace; font-size: 12px; margin: 8px 0; }"
                        u"code { font-family: monospace; font-size: 12px; background-color: #28282d; color: #e4e4e4; padding: 2px 5px; border-radius: 3px; }"
                        u"p { margin-bottom: 8px; line-height: 1.5; }"
                        u"ul, ol { margin-bottom: 8px; padding-left: 20px; }"
                        u"li { margin-bottom: 4px; }"
                        u"blockquote { border-left: 3px solid #3b82f6; padding-left: 10px; color: #888; margin: 8px 0; }"
                        u"a { color: #3b82f6; text-decoration: none; }"_s);
                    if (hasVisibleText) {
                        browser->setDocument(makeMarkdownDocument(msg.content));
                    } else {
                        browser->hide();
                    }
                    layout->addWidget(browser);

                    assistantWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
                    appendTranscriptWidget(assistantWidget);
                    }
                }

                // Recreate tool call widgets for tool calls made by this assistant message
                if (!msg.toolCalls.isEmpty()) {
                    for (const auto &toolCallVal : msg.toolCalls) {
                        const QJsonObject toolCallObj = toolCallVal.toObject();
                        const QString toolCallId = toolCallObj.value(u"id"_s).toString();
                        if (toolCallId.isEmpty() || rebuiltToolWidgets.contains(toolCallId)) {
                            continue;
                        }
                        const QJsonObject functionObj = toolCallObj.value(u"function"_s).toObject();
                        const QString toolName = functionObj.value(u"name"_s).toString();
                        const QString argumentsJson = functionObj.value(u"arguments"_s).toString();

                        // A restored sub-task keeps its own card, matching what
                        // the live transcript shows.
                        if (toolName == subtaskToolName()) {
                            QJsonObject restoredArgs;
                            const QJsonDocument restoredDoc = QJsonDocument::fromJson(argumentsJson.toUtf8());
                            if (!restoredDoc.isNull() && restoredDoc.isObject()) {
                                restoredArgs = restoredDoc.object();
                            }
                            auto *subtaskWidget = new SubtaskWidget(toolCallId, m_intentDock);
                            connect(subtaskWidget, &SubtaskWidget::cancelRequested, this, [this](const QString &taskId) {
                                m_agent.cancelSubtask(taskId);
                            });
                            const QString agentName = restoredArgs.value(u"agent"_s).toString();
                            subtaskWidget->startAgent(agentName.isEmpty() ? i18n("Sub-agent") : agentName,
                                                      restoredArgs.value(u"mode"_s).toString(),
                                                      restoredArgs.value(u"description"_s).toString());
                            m_subtaskWidgets.insert(toolCallId, subtaskWidget);
                            m_subtaskOrder.append(subtaskWidget);
                            addIntentWidget(subtaskWidget);
                            rebuiltToolWidgets.insert(toolCallId, nullptr);
                            continue;
                        }

                        // Create tool call widget in finished state (will be updated with result if available)
                        auto *toolWidget = new ToolCallWidget(toolCallId, m_transcriptContainer);

                        ToolRisk risk = ToolRisk::Read;
                        if (isFileMutatingTool(toolName)) {
                            risk = ToolRisk::Write;
                        } else if (toolName == u"bash"_s) {
                            risk = ToolRisk::Execute;
                        }

                        QJsonObject argsObj;
                        const QJsonDocument argsDoc = QJsonDocument::fromJson(argumentsJson.toUtf8());
                        if (!argsDoc.isNull() && argsDoc.isObject()) {
                            argsObj = argsDoc.object();
                        }
                        const QString summary = shellCommandFor(toolName, argsObj);

                        toolWidget->setToolInfo(toolName, summary, risk);

                        if (toolName == u"edit_file"_s) {
                            const QString path = argsObj.value(u"path"_s).toString();
                            toolWidget->setDescribeDiff(unifiedDiff(path,
                                argsObj.value(u"old_string"_s).toString(),
                                argsObj.value(u"new_string"_s).toString()));
                        } else if (toolName == u"multi_edit_file"_s || toolName == u"multi_replace_file_content"_s) {
                            QString path = argsObj.value(u"path"_s).toString();
                            if (path.isEmpty()) {
                                path = argsObj.value(u"TargetFile"_s).toString();
                            }
                            QJsonArray edits = argsObj.value(u"edits"_s).toArray();
                            if (edits.isEmpty()) {
                                edits = argsObj.value(u"chunks"_s).toArray();
                            }
                            if (edits.isEmpty()) {
                                edits = argsObj.value(u"ReplacementChunks"_s).toArray();
                            }
                            QString oldCombined;
                            QString newCombined;
                            for (const QJsonValue &v : edits) {
                                const QJsonObject c = v.toObject();
                                const QString o = c.value(u"old_string"_s).toString().isEmpty() ? c.value(u"TargetContent"_s).toString() : c.value(u"old_string"_s).toString();
                                const QString n = c.value(u"new_string"_s).toString().isEmpty() ? c.value(u"ReplacementContent"_s).toString() : c.value(u"new_string"_s).toString();
                                if (!oldCombined.isEmpty()) {
                                    oldCombined += u"\n---\n"_s;
                                    newCombined += u"\n---\n"_s;
                                }
                                oldCombined += o;
                                newCombined += n;
                            }
                            toolWidget->setDescribeDiff(unifiedDiff(path, oldCombined, newCombined));
                        } else if (toolName == u"write_file"_s) {
                            const QString path = argsObj.value(u"path"_s).toString();
                            toolWidget->setDescribeDiff(unifiedDiff(path, QString(),
                                argsObj.value(u"content"_s).toString()));
                        }

                        // Mark as finished (result will be filled in by Role::Tool message if available)
                        ToolResult dummyResult;
                        dummyResult.toolCallId = toolCallId;
                        dummyResult.name = toolName;
                        dummyResult.output = QString(); // Will be filled by Role::Tool message
                        dummyResult.ok = true;
                        toolWidget->setFinished(dummyResult);

                        m_toolCallWidgets.insert(toolCallId, toolWidget);
                        m_toolCallOrder.append(toolWidget);
                        rebuiltToolWidgets.insert(toolCallId, toolWidget);
                        appendTranscriptWidget(toolWidget);
                    }
                }
                break;
            case ChatMessage::Role::Tool:
                // Tool result message - update the corresponding tool call widget with the actual result
                if (!msg.toolCallId.isEmpty()
                                    && (rebuiltToolWidgets.contains(msg.toolCallId) || m_subtaskWidgets.contains(msg.toolCallId))) {
                    ToolResult result;
                    result.toolCallId = msg.toolCallId;
                    result.name = msg.name;
                    result.output = msg.content;
                    result.ok = true; // Assume success; the content contains formatted result
                    if (auto *subtaskWidget = m_subtaskWidgets.value(msg.toolCallId)) {
                        subtaskWidget->finishAgent(result);
                        break;
                    }
                    auto *toolWidget = rebuiltToolWidgets.value(msg.toolCallId);
                    // A restored sub-task is registered with a nullptr value to
                    // mark "this id is accounted for", so this can legitimately
                    // be null here.
                    if (toolWidget) {
                        toolWidget->setFinished(result);
                    }
                }
                break;
            case ChatMessage::Role::System:
                // System messages are not shown in transcript
                break;
        }
    }

    // Clear streaming-turn pointers after the history loop. rebuildTranscript
    // reconstructs finished messages, not a live streaming turn. Leaving these
    // non-null would make setStreaming() skip creating a fresh widget for the
    // next turn, appending new text into a completed historical message instead.
    m_activeAssistantWidget = nullptr;
    m_activeAssistantBrowser = nullptr;

    // Restore current thinking/plan state if there's an active turn
    const auto sessionData = m_agent.sessionData();
    restoreCurrentTurn(sessionData);

    applyTranscriptCollapse();
    updateTokenDisplay();
    updateModelSelectorLabel();
    QTimer::singleShot(0, this, [thisWeak = QPointer<ChatWidget>(this)]() {
        if (!thisWeak) {
            return;
        }
        thisWeak->reflowTranscriptMedia();
        thisWeak->forceScrollToBottom();
    });
}

void ChatWidget::restoreCurrentTurn(const SessionStore::SessionData &sessionData)
{
    // Restore the current turn's thinking/plan state without using streaming infrastructure
    // This is for the active (unfinished) turn that was in progress when Kate was closed
    QString lastAssistantThinking;
    const auto &messages = m_agent.messages();
    for (int i = messages.size() - 1; i >= 0; --i) {
        if (messages.at(i).role == ChatMessage::Role::Assistant) {
            lastAssistantThinking = messages.at(i).thinking;
            break;
        }
    }
    if (!sessionData.currentThinking.isEmpty()
        && sessionData.currentThinking != lastAssistantThinking) {
        // Create a thinking block widget directly (not via addThinkingBlock which is for streaming)
        if (!m_thinkingBlock) {
            QTextBrowser *thinkingBrowser = nullptr;
            QPushButton *thinkingToggle = nullptr;
            // Active turn being restored: respect autoCollapseThinking setting
            m_thinkingBlock = createThinkingBlock(m_transcriptContainer, thinkingBrowser, thinkingToggle, !m_settings.autoCollapseThinking);
            m_thinkingBrowser = thinkingBrowser;
            m_thinkingToggle = thinkingToggle;
            appendTranscriptWidget(m_thinkingBlock);
        }
        m_thinkingBuffer = sessionData.currentThinking;
        m_thinkingPacedLength = m_thinkingBuffer.length();
        updateThinkingDisplay();
        m_thinkingBlock->show();
        registerThinkingBlock(m_thinkingBlock, m_thinkingBrowser, m_thinkingToggle);
        // applyThinkingState already called by createThinkingBlock with correct initial state
    }

    if (!sessionData.currentPlan.isEmpty() && sessionData.planShown) {
        if (!m_planBlock) {
            m_planBlock = new QWidget(m_transcriptContainer);
            m_planBlock->hide();
            m_planLayout = new QVBoxLayout(m_planBlock);
            m_planLayout->setContentsMargins(4, 2, 4, 2);
            m_planLayout->setSpacing(2);
            auto *planLabel = new QLabel(i18n("Plan"), m_planBlock);
            planLabel->setStyleSheet(u"color: #888888; font-size: 10px; font-weight: bold; letter-spacing: 0.5px;"_s);
            m_planLayout->addWidget(planLabel);
            appendTranscriptWidget(m_planBlock);
        }
        addPlanChecklist(sessionData.currentPlan);
        m_planBlock->show();
    }
}

void ChatWidget::updateHistoryButton()
{
    if (!m_historyButton) {
        return;
    }
    const int maxConversations = m_settings.maxSavedConversations > 0 ? m_settings.maxSavedConversations : 50;
    const int count = SessionStore::listConversations(maxConversations).size();
    if (count > 0) {
        m_historyButton->setToolTip(i18n("Conversation History (%1)", count));
        // Show count as a small overlay text on the button when > 1
        m_historyButton->setText(count > 1 ? QString::number(count) : QString());
    } else {
        m_historyButton->setToolTip(i18n("Conversation History"));
        m_historyButton->setText(QString());
    }
}

void ChatWidget::showConversationHistory()
{
    // Always rebuild the menu so it reflects the current state.
    if (!m_historyMenu) {
        m_historyMenu = new QMenu(this);
    } else {
        m_historyMenu->clear();
    }

    const int maxConversations = m_settings.maxSavedConversations > 0 ? m_settings.maxSavedConversations : 50;
    const auto conversations = SessionStore::listConversations(maxConversations);

    if (conversations.isEmpty()) {
        auto *emptyAction = m_historyMenu->addAction(i18n("No conversations yet"));
        emptyAction->setEnabled(false);

        m_historyMenu->addSeparator();
        auto *newConvAction = m_historyMenu->addAction(QIcon::fromTheme(u"list-add"_s), i18n("New Conversation"));
        connect(newConvAction, &QAction::triggered, this, &ChatWidget::newChat);
    } else {
        for (const auto &conv : conversations) {
            QString displayText = conv.title;
            if (conv.isActive || conv.id == m_currentConversationId) {
                displayText = u"\u2713 "_s + displayText;
            }
            auto *action = m_historyMenu->addAction(displayText);
            action->setData(conv.id);
            // Use a non-checkable action with a triggered() connection so the
            // switch always fires regardless of the checked-state parity.
            const QString convId = conv.id;
            connect(action, &QAction::triggered, this, [this, convId]() {
                switchToConversation(convId);
            });
        }

        m_historyMenu->addSeparator();

        auto *newConvAction = m_historyMenu->addAction(QIcon::fromTheme(u"list-add"_s), i18n("New Conversation"));
        connect(newConvAction, &QAction::triggered, this, &ChatWidget::newChat);

        auto *clearAllAction = m_historyMenu->addAction(QIcon::fromTheme(u"edit-clear"_s), i18n("Clear All History"));
        connect(clearAllAction, &QAction::triggered, this, [this]() {
            SessionStore::clearAllConversations();
            m_currentConversationId.clear();
            newChat();
        });
    }

    // Show menu below the history button
    if (m_historyButton) {
        m_historyMenu->exec(m_historyButton->mapToGlobal(QPoint(0, m_historyButton->height())));
    }
}

void ChatWidget::switchToConversation(const QString &conversationId)
{
    if (m_loadingConversation || conversationId == m_currentConversationId) {
        return;
    }

    // Save current conversation before switching
    if (!m_agent.messages().isEmpty()) {
        const auto sessionData = m_agent.sessionData();
        if (!sessionData.messages.isEmpty()) {
            SessionStore::saveConversation(m_currentConversationId, sessionData, QString(), m_settings.maxSavedConversations > 0 ? m_settings.maxSavedConversations : 50);
        }
    }

    m_loadingConversation = true;

    // Load the new conversation
    const auto sessionData = SessionStore::loadConversation(conversationId);
    m_agent.restoreSession(sessionData);
    // Pending edits belong to the turn that produced them. Carrying them into a
    // different conversation meant "Reject All" could revert files that thread
    // never touched, on the strength of a decision made about another one.
    m_pendingToolCalls.clear();
    m_editTracker->clear();
    m_currentConversationId = conversationId;
    SessionStore::setActiveConversation(conversationId);

    // Rebuild transcript
    rebuildTranscript();

    // Update thread title
    const auto conversations = SessionStore::listConversations(0);
    for (const auto &conv : conversations) {
        if (conv.id == conversationId) {
            if (m_threadTitle) {
                m_threadTitle->setText(conv.title);
            }
            break;
        }
    }

    m_loadingConversation = false;
    updateHistoryButton();
    Q_EMIT conversationChanged(conversationId);
}

void ChatWidget::deleteConversation(const QString &conversationId)
{
    const bool wasActive = (conversationId == m_currentConversationId);
    SessionStore::deleteConversation(conversationId);

    if (wasActive) {
        // Switch to most recent conversation or create new
        const auto conversations = SessionStore::listConversations(1);
        if (!conversations.isEmpty()) {
            switchToConversation(conversations.first().id);
        } else {
            newChat();
        }
    }
}

void ChatWidget::setCurrentConversationId(const QString &conversationId)
{
    m_currentConversationId = conversationId;
}

// Create a new blank conversation and reset the chat UI.
void ChatWidget::newChat()
{
    m_agent.abort();

    // Save the current conversation before clearing so it remains in history.
    if (!m_agent.messages().isEmpty()) {
        const auto sessionData = m_agent.sessionData();
        if (!sessionData.messages.isEmpty()) {
            // Allocate an ID if we somehow still don't have one.
            if (m_currentConversationId.isEmpty()) {
                m_currentConversationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            }
            SessionStore::saveConversation(m_currentConversationId, sessionData, QString(),
                                          m_settings.maxSavedConversations > 0 ? m_settings.maxSavedConversations : 50);
        }
    }

    m_agent.resetConversation();
    m_agent.clearSession();
    m_permissionBar->hideBar();
    // Drop half-recorded edits from the aborted turn: their tool calls will
    // never report a result, so leaving them queued would strand a row in the
    // tracker that no button could act on.
    m_pendingToolCalls.clear();
    m_editTracker->clear();
    if (m_infoBar) {
        m_infoBar->hide();
    }

    // Allocate a fresh conversation ID.  The new conversation will only be
    // registered in the history list once saveConversation() is called with
    // real messages, so no empty ghost entry appears immediately.
    m_currentConversationId = SessionStore::createNewConversation();

    // Update history button to reflect any newly saved previous conversation.
    updateHistoryButton();

    // Rebuild transcript from scratch to ensure it matches the cleared agent state
    rebuildTranscript();

    if (m_threadTitle) {
        m_threadTitle->setText(i18n("New Thread"));
    }
    m_prompt->clear();
    m_prompt->setEnabled(true);
    updateSendButtonState();
    updateTokenDisplay();
    updateModelSelectorLabel();
    forceScrollToBottom();
    m_prompt->setFocus();
}

} // namespace KateAi
