#include "chatwidget.h"

#include "permissionbar.h"
#include "promptedit.h"
#include "settings.h"
#include "toolcallwidget.h"

#include <KLocalizedString>

#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

ChatWidget::ChatWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 1. Zed-style Header / Toolbar
    m_toolbar = new QWidget(this);
    auto *toolbarLayout = new QHBoxLayout(m_toolbar);
    toolbarLayout->setContentsMargins(10, 6, 10, 6);
    toolbarLayout->setSpacing(8);

    // Unified Model Selector button
    m_modelSelector = new QPushButton(this);
    m_modelSelector->setCursor(Qt::PointingHandCursor);
    m_modelSelector->setStyleSheet(
        u"QPushButton {"
        u"  background-color: #262628;"
        u"  color: #cccccc;"
        u"  border: 1px solid #3c3c40;"
        u"  border-radius: 4px;"
        u"  padding: 4px 10px;"
        u"  font-size: 12px;"
        u"  font-weight: 500;"
        u"  text-align: left;"
        u"}"
        u"QPushButton:hover {"
        u"  background-color: #2e2e32;"
        u"  border-color: #4a4a50;"
        u"  color: #ffffff;"
        u"}"_s);
    toolbarLayout->addWidget(m_modelSelector);

    // Thread title label
    m_threadTitle = new QLabel(i18n("New Thread"), this);
    m_threadTitle->setStyleSheet(u"QLabel { color: #888888; font-size: 12px; font-weight: 500; padding-left: 4px; }"_s);
    toolbarLayout->addWidget(m_threadTitle);

    toolbarLayout->addStretch();

    // New Chat button
    m_newChat = new QPushButton(QIcon::fromTheme(u"list-add"_s), QString(), this);
    m_newChat->setToolTip(i18n("New Thread"));
    m_newChat->setFixedSize(26, 26);
    m_newChat->setCursor(Qt::PointingHandCursor);
    m_newChat->setStyleSheet(
        u"QPushButton {"
        u"  background: transparent;"
        u"  border: 1px solid transparent;"
        u"  border-radius: 4px;"
        u"}"
        u"QPushButton:hover {"
        u"  background-color: #2e2e32;"
        u"  border-color: #3c3c40;"
        u"}"_s);
    toolbarLayout->addWidget(m_newChat);

    // Settings / Configure button
    m_configure = new QPushButton(QIcon::fromTheme(u"settings-configure"_s), QString(), this);
    m_configure->setToolTip(i18n("Settings"));
    m_configure->setFixedSize(26, 26);
    m_configure->setCursor(Qt::PointingHandCursor);
    m_configure->setStyleSheet(
        u"QPushButton {"
        u"  background: transparent;"
        u"  border: 1px solid transparent;"
        u"  border-radius: 4px;"
        u"}"
        u"QPushButton:hover {"
        u"  background-color: #2e2e32;"
        u"  border-color: #3c3c40;"
        u"}"_s);
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
    m_mode->addItem(i18n("Agent"), false);
    m_mode->addItem(i18n("Plan"), true);

    m_thinking = new QPushButton(this);
    m_thinking->setCheckable(true);

    m_stop = new QPushButton(this);
    m_stop->setVisible(false);
    m_stop->setEnabled(false);

    // 2. Zed-style Transcript Area (Scroll Area with Cards & Tool Widgets)
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(
        u"QScrollArea {"
        u"  background-color: #181818;"
        u"  border: none;"
        u"}"
        u"QScrollBar:vertical {"
        u"  background: transparent;"
        u"  width: 8px;"
        u"  margin: 0;"
        u"}"
        u"QScrollBar::handle:vertical {"
        u"  background: #333338;"
        u"  border-radius: 4px;"
        u"  min-height: 24px;"
        u"}"
        u"QScrollBar::handle:vertical:hover {"
        u"  background: #4a4a52;"
        u"}"
        u"QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        u"  height: 0;"
        u"}"_s);

    m_transcriptContainer = new QWidget(m_scrollArea);
    m_transcriptContainer->setStyleSheet(u"background-color: #181818;"_s);
    m_transcriptLayout = new QVBoxLayout(m_transcriptContainer);
    m_transcriptLayout->setContentsMargins(14, 14, 14, 14);
    m_transcriptLayout->setSpacing(10);

    // Initial empty state welcome widget
    auto *welcome = new QWidget(m_transcriptContainer);
    welcome->setObjectName(u"welcomeWidget"_s);
    auto *wLayout = new QVBoxLayout(welcome);
    wLayout->setContentsMargins(20, 40, 20, 20);
    wLayout->setAlignment(Qt::AlignCenter);

    auto *wIcon = new QLabel(u"⚡"_s, welcome);
    wIcon->setAlignment(Qt::AlignCenter);
    wIcon->setStyleSheet(u"font-size: 26px; color: #3b82f6; margin-bottom: 6px;"_s);
    wLayout->addWidget(wIcon);

    auto *wTitle = new QLabel(i18n("Kate AI Agent"), welcome);
    wTitle->setAlignment(Qt::AlignCenter);
    wTitle->setStyleSheet(u"color: #e4e4e4; font-size: 15px; font-weight: bold;"_s);
    wLayout->addWidget(wTitle);

    auto *wSub = new QLabel(i18n("Ask questions, edit code, and explore your workspace."), welcome);
    wSub->setAlignment(Qt::AlignCenter);
    wSub->setStyleSheet(u"color: #777777; font-size: 12px; margin-top: 4px;"_s);
    wLayout->addWidget(wSub);

    m_transcriptLayout->addWidget(welcome);
    m_transcriptLayout->addStretch();
    m_scrollArea->setWidget(m_transcriptContainer);
    root->addWidget(m_scrollArea, 1);

    // 3. Permission Bar (Zed-style Inline Consent)
    m_permissionBar = new PermissionBar(this);
    root->addWidget(m_permissionBar);

    // 4. Composer Area (Zed-style Input Box)
    auto *composerContainer = new QWidget(this);
    composerContainer->setStyleSheet(
        u"QWidget {"
        u"  background-color: #1a1a1a;"
        u"  border-top: 1px solid #282828;"
        u"}"_s);
    auto *composerLayout = new QVBoxLayout(composerContainer);
    composerLayout->setContentsMargins(12, 8, 12, 8);
    composerLayout->setSpacing(4);

    auto *composerCard = new QWidget(composerContainer);
    composerCard->setObjectName(u"composerCard"_s);
    composerCard->setStyleSheet(
        u"QWidget#composerCard {"
        u"  background-color: #232326;"
        u"  border: 1px solid #38383e;"
        u"  border-radius: 8px;"
        u"}"_s);
    auto *composerCardLayout = new QVBoxLayout(composerCard);
    composerCardLayout->setContentsMargins(10, 8, 10, 6);
    composerCardLayout->setSpacing(4);

    m_prompt = new PromptEdit(composerCard);
    m_prompt->setStyleSheet(
        u"QPlainTextEdit {"
        u"  background: transparent;"
        u"  color: #e4e4e4;"
        u"  border: none;"
        u"  padding: 2px;"
        u"  font-size: 13px;"
        u"}"_s);
    composerCardLayout->addWidget(m_prompt);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(2, 0, 2, 2);

    m_tokenCount = new QLabel(composerCard);
    m_tokenCount->setStyleSheet(u"QLabel { color: #666; font-size: 11px; }"_s);
    bottomRow->addWidget(m_tokenCount);
    bottomRow->addStretch();

    // Thinking mode toggle button
    m_thinking->setParent(composerCard);
    m_thinking->setVisible(true);
    m_thinking->setCheckable(true);
    m_thinking->setFixedSize(28, 28);
    m_thinking->setCursor(Qt::PointingHandCursor);
    m_thinking->setToolTip(i18n("Toggle thinking mode"));
    updateThinkingButtonStyle();
    bottomRow->addWidget(m_thinking);

    m_send = new QPushButton(composerCard);
    m_send->setFixedSize(28, 28);
    m_send->setCursor(Qt::PointingHandCursor);
    updateSendButtonState();
    bottomRow->addWidget(m_send);

    composerCardLayout->addLayout(bottomRow);
    composerLayout->addWidget(composerCard);

    m_status = new QLabel(i18n("Enter to send · Shift+Enter for a new line"), composerContainer);
    m_status->setStyleSheet(u"QLabel { color: #555555; font-size: 11px; margin-left: 4px; }"_s);
    composerLayout->addWidget(m_status);

    root->addWidget(composerContainer);

    // Signal connections
    connect(m_prompt, &PromptEdit::submitRequested, this, &ChatWidget::submit);
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
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
    });

    connect(m_model, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (m_updatingCombos) {
            return;
        }
        switch (m_settings.provider) {
        case Provider::OpenAI:
            m_settings.openaiModel = text.trimmed();
            break;
        case Provider::OpenRouter:
            m_settings.openrouterModel = text.trimmed();
            break;
        case Provider::OpenAICompatible:
            m_settings.openaiCompatibleModel = text.trimmed();
            break;
        case Provider::ClaudeCompatible:
            m_settings.claudeCompatibleModel = text.trimmed();
            break;
        case Provider::Grok:
        default:
            m_settings.grokModel = text.trimmed();
            break;
        }
        updateModelSelectorLabel();
        updateTokenDisplay();
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
    });

    connect(m_permission, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingCombos) return;
        m_settings.permissionMode = permissionModeFromId(m_permission->currentData().toString());
        m_agent.setSettings(m_settings);
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
        m_settings.planMode = m_mode->currentData().toBool();
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
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
            m_thinkingBrowser->setMarkdown(escape(m_thinkingBrowser->toPlainText() + delta));
            const int h = static_cast<int>(m_thinkingBrowser->document()->size().height()) + 12;
            m_thinkingBrowser->setFixedHeight(std::max(20, h));
            if (m_thinkingBlock) {
                m_thinkingBlock->setMaximumHeight(std::max(20, h));
            }
            scrollToBottom();
        }
    });
    connect(&m_agent, &AgentLoop::thinkingFinished, this, &ChatWidget::addThinkingBlock);
    connect(&m_agent, &AgentLoop::planUpdated, this, &ChatWidget::addPlanChecklist);
    connect(&m_agent, &AgentLoop::assistantDelta, this, [this](const QString &delta) {
        setStreaming(m_streamText + delta);
    });
    connect(&m_agent, &AgentLoop::assistantFinished, this, [this](const QString &text) {
        Q_UNUSED(text);
        freezeStreaming();
    });
    connect(&m_agent, &AgentLoop::activityUpdated, this, &ChatWidget::addActivityMessage);

    // Tool visibility signals (Zed-style inline tool-call cards)
    connect(&m_agent, &AgentLoop::toolStarted, this, [this](const PermissionRequest &request) {
        freezeStreaming();
        auto *toolWidget = new ToolCallWidget(request.toolCallId, m_transcriptContainer);
        toolWidget->setToolInfo(request.toolName, request.summary, request.risk);
        toolWidget->setDescribeDiff(request.describeDiff);
        toolWidget->setRunning();
        m_toolCallWidgets.insert(request.toolCallId, toolWidget);
        m_transcriptLayout->insertWidget(m_transcriptLayout->count() - 1, toolWidget);
        scrollToBottom();
    });

    connect(&m_agent, &AgentLoop::toolFinished, this, [this](const ToolResult &result) {
        if (auto *widget = m_toolCallWidgets.value(result.toolCallId)) {
            widget->setFinished(result);
        }
        scrollToBottom();
    });

    connect(&m_agent, &AgentLoop::permissionNeeded, this, [this](const PermissionRequest &request) {
        m_permissionBar->showRequest(request);
        m_prompt->setEnabled(false);
        updateSendButtonState();
        scrollToBottom();
    });

    connect(&m_agent, &AgentLoop::statusChanged, this, [this](const QString &status) {
        m_status->setText(status.isEmpty() ? i18n("Enter to send · Shift+Enter for a new line") : status);
        m_prompt->setEnabled(!m_permissionBar->isVisible());
        updateSendButtonState();
    });

    connect(&m_agent, &AgentLoop::failed, this, [this](const QString &error) {
        freezeStreaming();
        auto *errCard = new QWidget(m_transcriptContainer);
        errCard->setStyleSheet(u"QWidget { background-color: #261b1b; border: 1px solid #5a2020; border-left: 4px solid #ef4444; border-radius: 6px; }"_s);
        auto *l = new QVBoxLayout(errCard);
        l->setContentsMargins(12, 10, 12, 10);
        auto *errLabel = new QLabel(errCard);
        errLabel->setWordWrap(true);
        errLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        errLabel->setStyleSheet(u"color: #ff8888; font-size: 12px; font-weight: bold; background: transparent; border: none;"_s);
        errLabel->setText(i18n("Error: %1", error));
        l->addWidget(errLabel);
        m_transcriptLayout->insertWidget(m_transcriptLayout->count() - 1, errCard);
        scrollToBottom();
        updateSendButtonState();
    });

    connect(&m_agent, &AgentLoop::turnFinished, this, [this]() {
        m_prompt->setEnabled(true);
        updateSendButtonState();
        m_prompt->setFocus();
    });

    connect(&m_agent, &AgentLoop::modelsReceived, this, [this](Provider provider, const QStringList &models) {
        m_modelCatalog.insert(provider, models);
        refreshProviders();
        updateModelSelectorLabel();
        updateTokenDisplay();
    });

    connect(&m_agent, &AgentLoop::modelsFailed, this, [this](Provider provider, const QString &error) {
        m_modelCatalog.remove(provider);
        refreshProviders();
        updateModelSelectorLabel();
        if (provider == m_settings.provider) {
            m_status->setText(i18n("Model list unavailable: %1", error));
        }
    });

    updateModelSelectorLabel();
    updateTokenDisplay();
}

void ChatWidget::addUserMessage(const QString &text)
{
    // Remove welcome widget if present
    if (auto *welcome = m_transcriptContainer->findChild<QWidget *>(u"welcomeWidget"_s)) {
        welcome->deleteLater();
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
    card->setStyleSheet(
        u"QWidget {"
        u"  background-color: #232326;"
        u"  border: 1px solid #333338;"
        u"  border-radius: 6px;"
        u"}"_s);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 10, 12, 10);
    cardLayout->setSpacing(6);

    auto *header = new QLabel(i18n("YOU"), card);
    header->setStyleSheet(u"color: #888888; font-size: 10px; font-weight: bold; letter-spacing: 0.5px; border: none; background: transparent;"_s);
    cardLayout->addWidget(header);

    auto *msgLabel = new QLabel(card);
    msgLabel->setWordWrap(true);
    msgLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    msgLabel->setStyleSheet(u"color: #e4e4e4; font-size: 13px; line-height: 1.5; border: none; background: transparent;"_s);
    msgLabel->setText(escape(text).replace(u"\n"_s, u"<br>"_s));
    cardLayout->addWidget(msgLabel);

    m_transcriptLayout->insertWidget(m_transcriptLayout->count() - 1, card);
    scrollToBottom();
}

void ChatWidget::addActivityMessage(const QString &text)
{
    auto *pill = new QLabel(escape(text), m_transcriptContainer);
    pill->setStyleSheet(u"color: #777777; font-size: 11px; font-style: italic; padding: 2px 4px;"_s);
    m_transcriptLayout->insertWidget(m_transcriptLayout->count() - 1, pill);
    scrollToBottom();
}

void ChatWidget::setStreaming(const QString &text)
{
    m_streamText = text;
    if (!m_activeAssistantWidget) {
        m_activeAssistantWidget = new QWidget(m_transcriptContainer);
        auto *layout = new QVBoxLayout(m_activeAssistantWidget);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        auto *header = new QLabel(i18n("KATE AI"), m_activeAssistantWidget);
        header->setStyleSheet(u"color: #3b82f6; font-size: 10px; font-weight: bold; letter-spacing: 0.5px;"_s);
        layout->addWidget(header);

        // Collapsible hidden-reasoning block. Collapsed by default: the user
        // sees the visible answer, not the internal chain-of-thought.
        m_thinkingBlock = new QWidget(m_activeAssistantWidget);
        m_thinkingBlock->setMaximumHeight(0);
        auto *tbLayout = new QVBoxLayout(m_thinkingBlock);
        tbLayout->setContentsMargins(0, 0, 0, 0);
        tbLayout->setSpacing(0);

        auto *tbHeader = new QHBoxLayout;
        m_thinkingToggle = new QPushButton(u"\u25b4 "_s + i18n("Reasoning"), m_thinkingBlock);
        m_thinkingToggle->setFlat(true);
        m_thinkingToggle->setCursor(Qt::PointingHandCursor);
        m_thinkingToggle->setStyleSheet(
            u"QPushButton { color: #888888; font-size: 11px; font-style: italic; border: none; text-align: left; }"
            u"QPushButton:hover { color: #aaaaaa; }"_s);
        connect(m_thinkingToggle, &QPushButton::clicked, this, &ChatWidget::toggleThinking);
        tbHeader->addWidget(m_thinkingToggle);
        tbHeader->addStretch();
        tbLayout->addLayout(tbHeader);

        m_thinkingBrowser = new QTextBrowser(m_thinkingBlock);
        m_thinkingBrowser->setReadOnly(true);
        m_thinkingBrowser->setFrameShape(QFrame::NoFrame);
        m_thinkingBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_thinkingBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_thinkingBrowser->setStyleSheet(
            u"QTextBrowser { background: transparent; color: #888888; border: none;"
            u"  font-style: italic; font-size: 12px; padding: 0 4px; }"_s);
        m_thinkingBrowser->document()->setDefaultStyleSheet(
            u"body { color: #888888; font-style: italic; font-size: 12px; margin: 0; padding: 0; }"
            u"p { margin-bottom: 4px; }"_s);
        tbLayout->addWidget(m_thinkingBrowser);
        layout->addWidget(m_thinkingBlock);

        // Structured plan checklist, rendered below the thinking block.
        m_planBlock = new QWidget(m_activeAssistantWidget);
        m_planBlock->hide();
        m_planLayout = new QVBoxLayout(m_planBlock);
        m_planLayout->setContentsMargins(4, 2, 4, 2);
        m_planLayout->setSpacing(2);
        auto *planLabel = new QLabel(i18n("Plan"), m_planBlock);
        planLabel->setStyleSheet(u"color: #888888; font-size: 10px; font-weight: bold; letter-spacing: 0.5px;"_s);
        m_planLayout->addWidget(planLabel);
        layout->addWidget(m_planBlock);

        m_activeAssistantBrowser = new QTextBrowser(m_activeAssistantWidget);
        m_activeAssistantBrowser->setOpenExternalLinks(true);
        m_activeAssistantBrowser->setFrameShape(QFrame::NoFrame);
        m_activeAssistantBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_activeAssistantBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_activeAssistantBrowser->setStyleSheet(u"background: transparent; color: #d4d4d4; border: none; padding: 0px;"_s);
        m_activeAssistantBrowser->document()->setDefaultStyleSheet(
            u"body { color: #d4d4d4; font-family: sans-serif; font-size: 13px; margin: 0; padding: 0; }"
            u"pre { background-color: #222225; color: #e4e4e4; padding: 10px 12px; border-radius: 6px; border: 1px solid #333338; font-family: monospace; font-size: 12px; margin: 8px 0; }"
            u"code { font-family: monospace; font-size: 12px; background-color: #28282d; color: #e4e4e4; padding: 2px 5px; border-radius: 3px; }"
            u"p { margin-bottom: 8px; line-height: 1.5; }"
            u"ul, ol { margin-bottom: 8px; padding-left: 20px; }"
            u"li { margin-bottom: 4px; }"
            u"blockquote { border-left: 3px solid #3b82f6; padding-left: 10px; color: #888; margin: 8px 0; }"
            u"a { color: #3b82f6; text-decoration: none; }"_s);

        layout->addWidget(m_activeAssistantBrowser);
        m_transcriptLayout->insertWidget(m_transcriptLayout->count() - 1, m_activeAssistantWidget);
    }

    m_activeAssistantBrowser->setMarkdown(m_streamText);
    const int docH = static_cast<int>(m_activeAssistantBrowser->document()->size().height()) + 16;
    m_activeAssistantBrowser->setFixedHeight(std::max(30, docH));
    scrollToBottom();
}

void ChatWidget::addThinkingBlock(const QString &text)
{
    // Hidden reasoning may arrive before the first visible text delta, so
    // ensure the active assistant widget (and its thinking/plan blocks) exist.
    if (!m_activeAssistantWidget) {
        setStreaming(m_streamText);
    }
    if (!m_thinkingBrowser) {
        return;
    }
    m_thinkingBrowser->setMarkdown(escape(text));
    const int h = static_cast<int>(m_thinkingBrowser->document()->size().height()) + 12;
    m_thinkingBrowser->setFixedHeight(std::max(20, h));
    m_thinkingBlock->setMaximumHeight(std::max(20, h));
    m_thinkingExpanded = true;
    m_thinkingToggle->setText(u"\u25be "_s + i18n("Reasoning"));
    scrollToBottom();
}

void ChatWidget::collapseThinkingBlock()
{
    if (!m_thinkingBlock) {
        return;
    }
    // Collapse the hidden reasoning once the visible answer starts streaming,
    // so the user is not forced to wade through chain-of-thought.
    m_thinkingBlock->setMaximumHeight(0);
    m_thinkingExpanded = false;
    if (m_thinkingToggle) {
        m_thinkingToggle->setText(u"\u25b4 "_s + i18n("Reasoning"));
    }
}

void ChatWidget::toggleThinking()
{
    if (!m_thinkingBlock) {
        return;
    }
    m_thinkingExpanded = !m_thinkingExpanded;
    if (m_thinkingExpanded) {
        const int h = static_cast<int>(m_thinkingBrowser->document()->size().height()) + 12;
        m_thinkingBlock->setMaximumHeight(std::max(20, h));
        m_thinkingToggle->setText(u"\u25be "_s + i18n("Reasoning"));
    } else {
        m_thinkingBlock->setMaximumHeight(0);
        m_thinkingToggle->setText(u"\u25b4 "_s + i18n("Reasoning"));
    }
}

void ChatWidget::addPlanChecklist(const QJsonArray &plan)
{
    if (!m_planBlock || !m_planLayout) {
        return;
    }
    m_planSteps.clear();
    for (const QJsonValue &v : plan) {
        const QJsonObject o = v.toObject();
        const QString desc = o.value(u"description"_s).toString();
        const bool completed = o.value(u"completed"_s).toBool();
        auto *cb = new QCheckBox(desc, m_planBlock);
        cb->setChecked(completed);
        cb->setDisabled(true);
        cb->setStyleSheet(u"QCheckBox { color: #b0b0b0; font-size: 12px; }"
                          u"QCheckBox::indicator { width: 14px; height: 14px; }"_s);
        m_planLayout->addWidget(cb);
        m_planSteps.insert(cb, o.value(u"id"_s).toString());
    }
    m_planBlock->show();
    scrollToBottom();
}

void ChatWidget::markPlanStepCompleted(const QString &stepId)
{
    for (QCheckBox *cb : m_planSteps.keys()) {
        if (m_planSteps.value(cb) == stepId) {
            cb->setChecked(true);
            break;
        }
    }
}

void ChatWidget::freezeStreaming()
{
    if (m_activeAssistantBrowser && !m_streamText.isEmpty()) {
        m_activeAssistantBrowser->setMarkdown(m_streamText);
        const int docH = static_cast<int>(m_activeAssistantBrowser->document()->size().height()) + 16;
        m_activeAssistantBrowser->setFixedHeight(std::max(30, docH));
    }
    collapseThinkingBlock();
    m_activeAssistantWidget = nullptr;
    m_activeAssistantBrowser = nullptr;
    m_thinkingBlock = nullptr;
    m_thinkingBrowser = nullptr;
    m_thinkingToggle = nullptr;
    m_planBlock = nullptr;
    m_planLayout = nullptr;
    m_streamText.clear();
}

void ChatWidget::scrollToBottom()
{
    QTimer::singleShot(10, this, [this]() {
        if (m_scrollArea) {
            m_scrollArea->verticalScrollBar()->setValue(m_scrollArea->verticalScrollBar()->maximum());
        }
    });
}

void ChatWidget::newChat()
{
    m_agent.abort();
    m_agent.resetConversation();
    m_permissionBar->hideBar();

    // Clear transcript items except the bottom stretch
    QLayoutItem *child;
    while (m_transcriptLayout->count() > 1 && (child = m_transcriptLayout->takeAt(0))) {
        if (child->widget()) {
            child->widget()->deleteLater();
        }
        delete child;
    }

    m_toolCallWidgets.clear();
    m_activeAssistantWidget = nullptr;
    m_activeAssistantBrowser = nullptr;
    m_streamText.clear();

    // Recreate welcome widget
    auto *welcome = new QWidget(m_transcriptContainer);
    welcome->setObjectName(u"welcomeWidget"_s);
    auto *wLayout = new QVBoxLayout(welcome);
    wLayout->setContentsMargins(20, 40, 20, 20);
    wLayout->setAlignment(Qt::AlignCenter);

    auto *wIcon = new QLabel(u"⚡"_s, welcome);
    wIcon->setAlignment(Qt::AlignCenter);
    wIcon->setStyleSheet(u"font-size: 26px; color: #3b82f6; margin-bottom: 6px;"_s);
    wLayout->addWidget(wIcon);

    auto *wTitle = new QLabel(i18n("Kate AI Agent"), welcome);
    wTitle->setAlignment(Qt::AlignCenter);
    wTitle->setStyleSheet(u"color: #e4e4e4; font-size: 15px; font-weight: bold;"_s);
    wLayout->addWidget(wTitle);

    auto *wSub = new QLabel(i18n("Ask questions, edit code, and explore your workspace."), welcome);
    wSub->setAlignment(Qt::AlignCenter);
    wSub->setStyleSheet(u"color: #777777; font-size: 12px; margin-top: 4px;"_s);
    wLayout->addWidget(wSub);

    m_transcriptLayout->insertWidget(0, welcome);

    if (m_threadTitle) {
        m_threadTitle->setText(i18n("New Thread"));
    }
    m_prompt->clear();
    m_prompt->setEnabled(true);
    updateSendButtonState();
    updateTokenDisplay();
    m_prompt->setFocus();
}

void ChatWidget::setSettings(const Settings &settings)
{
    m_settings = settings;
    m_preferredProvider = settings.provider;
    m_modelCatalog.clear();
    m_updatingCombos = true;

    const int permIndex = m_permission->findData(permissionModeId(settings.permissionMode));
    if (permIndex >= 0) {
        m_permission->setCurrentIndex(permIndex);
    }
    const int sandboxIndex = m_sandbox->findData(sandboxProfileId(settings.sandbox));
    if (sandboxIndex >= 0) {
        m_sandbox->setCurrentIndex(sandboxIndex);
    }
    const int modeIndex = m_mode->findData(settings.planMode);
    if (modeIndex >= 0) {
        m_mode->setCurrentIndex(modeIndex);
    }
    m_thinking->setChecked(settings.thinkingMode);
    updateThinkingButtonStyle();
    m_updatingCombos = false;

    refreshProviders();
    updateModelSelectorLabel();
    updateTokenDisplay();

    for (Provider provider : {Provider::Grok, Provider::OpenAI, Provider::OpenRouter, Provider::OpenAICompatible, Provider::ClaudeCompatible}) {
        Settings providerSettings = settings;
        providerSettings.provider = provider;
        if (!apiKeyFor(providerSettings).trimmed().isEmpty()) {
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
    for (Provider provider : {Provider::Grok, Provider::OpenAI, Provider::OpenRouter, Provider::OpenAICompatible, Provider::ClaudeCompatible}) {
        if (m_modelCatalog.contains(provider) || !apiKeyFor(m_settings).trimmed().isEmpty()) {
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
    m_provider->setEnabled(!m_modelCatalog.isEmpty());
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
    const QStringList allModels = m_modelCatalog.value(m_settings.provider, defaultModels(m_settings.provider));
    QStringList models = allModels;
    if (!m_modelFilter.isEmpty()) {
        models.clear();
        for (const QString &m : allModels) {
            if (m.contains(m_modelFilter, Qt::CaseInsensitive)) {
                models.append(m);
            }
        }
    }
    m_model->addItems(models);
    m_model->setEnabled(!allModels.isEmpty());

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
            case Provider::OpenAICompatible:
                m_settings.openaiCompatibleModel = selectedModel;
                break;
            case Provider::ClaudeCompatible:
                m_settings.claudeCompatibleModel = selectedModel;
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
    const QString pLabel = providerLabel(m_settings.provider);
    const QString model = modelFor(m_settings);
    m_modelSelector->setText(u"%1: %2  ▾"_s.arg(pLabel, model.isEmpty() ? i18n("Select model") : model));
}

void ChatWidget::updateTokenDisplay()
{
    if (!m_tokenCount) return;
    const QString m = modelFor(m_settings);
    m_tokenCount->setText(m.isEmpty() ? QString() : m);
}

void ChatWidget::updateThinkingButtonStyle()
{
    if (!m_thinking) return;
    if (m_thinking->isChecked()) {
        m_thinking->setText(u"💡"_s);
        m_thinking->setStyleSheet(
            u"QPushButton {"
            u"  color: #fbbf24;"
            u"  background-color: #2e2e32;"
            u"  border: 1px solid #3c3c40;"
            u"  border-radius: 4px;"
            u"  font-size: 14px;"
            u"}"
            u"QPushButton:hover {"
            u"  background-color: #3a3a3e;"
            u"  border-color: #4a4a50;"
            u"}"_s);
    } else {
        m_thinking->setText(u"💭"_s);
        m_thinking->setStyleSheet(
            u"QPushButton {"
            u"  color: #888888;"
            u"  background-color: #2e2e32;"
            u"  border: 1px solid #3c3c40;"
            u"  border-radius: 4px;"
            u"  font-size: 14px;"
            u"}"
            u"QPushButton:hover {"
            u"  background-color: #3a3a3e;"
            u"  border-color: #4a4a50;"
            u"  color: #aaaaaa;"
            u"}"_s);
    }
}

void ChatWidget::showModelMenu()
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

    const QList<Provider> providers = {
        Provider::Grok,
        Provider::OpenAI,
        Provider::OpenRouter,
        Provider::OpenAICompatible,
        Provider::ClaudeCompatible
    };

    for (Provider p : providers) {
        auto *pMenu = menu.addMenu(providerLabel(p));
        pMenu->setStyleSheet(menu.styleSheet());
        const QStringList models = m_modelCatalog.value(p, defaultModels(p));
        const QString currentModel = modelFor(m_settings);

        for (const QString &m : models) {
            auto *act = pMenu->addAction(m);
            act->setCheckable(true);
            act->setChecked(m_settings.provider == p && currentModel == m);
            connect(act, &QAction::triggered, this, [this, p, m]() {
                m_settings.provider = p;
                m_preferredProvider = p;
                switch (p) {
                case Provider::OpenAI:
                    m_settings.openaiModel = m;
                    break;
                case Provider::OpenRouter:
                    m_settings.openrouterModel = m;
                    break;
                case Provider::OpenAICompatible:
                    m_settings.openaiCompatibleModel = m;
                    break;
                case Provider::ClaudeCompatible:
                    m_settings.claudeCompatibleModel = m;
                    break;
                case Provider::Grok:
                default:
                    m_settings.grokModel = m;
                    break;
                }
                updateModelSelectorLabel();
                updateTokenDisplay();
                applyProviderToCombos();
                m_agent.setSettings(m_settings);
                Q_EMIT settingsChanged(m_settings);
            });
        }
    }

    menu.addSeparator();
    auto *configAct = menu.addAction(i18n("Configure Providers & Models…"));
    connect(configAct, &QAction::triggered, this, &ChatWidget::configureRequested);

    menu.exec(m_modelSelector->mapToGlobal(QPoint(0, m_modelSelector->height() + 2)));
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
        m_mode->setCurrentIndex(checked ? 1 : 0);
    });

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

void ChatWidget::submit()
{
    const QString text = m_prompt->toPlainText().trimmed();
    if (text.isEmpty() || m_agent.isBusy()) {
        return;
    }
    m_prompt->clear();
    updateSendButtonState();
    m_agent.start(text);
    updateSendButtonState();
}

void ChatWidget::updateSendButtonState()
{
    const bool busy = m_agent.isBusy();
    const bool promptEmpty = m_prompt && m_prompt->toPlainText().trimmed().isEmpty();
    const bool canClick = busy || !promptEmpty;

    m_send->setEnabled(canClick);

    if (busy) {
        m_send->setText(u"■"_s);
        m_send->setStyleSheet(
            u"QPushButton {"
            u"  color: #ffffff;"
            u"  background-color: #e74c3c;"
            u"  font-size: 13px;"
            u"  border: none;"
            u"  border-radius: 4px;"
            u"}"
            u"QPushButton:hover { background-color: #ff6b5a; }"_s);
        m_send->setToolTip(i18n("Stop response"));
    } else {
        m_send->setText(u"▲"_s);
        if (canClick) {
            m_send->setStyleSheet(
                u"QPushButton {"
                u"  color: #ffffff;"
                u"  background-color: #007acc;"
                u"  font-size: 13px;"
                u"  border: none;"
                u"  border-radius: 4px;"
                u"}"
                u"QPushButton:hover { background-color: #0062a3; }"_s);
        } else {
            m_send->setStyleSheet(
                u"QPushButton {"
                u"  color: #555555;"
                u"  background-color: #2e2e32;"
                u"  font-size: 13px;"
                u"  border: 1px solid #38383e;"
                u"  border-radius: 4px;"
                u"}"_s);
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
    QTextDocument doc;
    doc.setMarkdown(text);
    return doc.toHtml();
}

} // namespace KateAi
