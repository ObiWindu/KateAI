/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "configpage.h"
#include "plugin.h"
#include "settings.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

KateAiConfigPage::KateAiConfigPage(QWidget *parent, KateAiPlugin *plugin)
    : KTextEditor::ConfigPage(parent)
    , m_plugin(plugin)
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto *tabs = new QTabWidget(this);
    rootLayout->addWidget(tabs);

    auto makeKey = [this]() {
        auto *edit = new QLineEdit(this);
        edit->setEchoMode(QLineEdit::Password);
        edit->setClearButtonEnabled(true);
        return edit;
    };

    // ==========================================
    // TAB 1: AI Providers & Models
    // ==========================================
    auto *providersScroll = new QScrollArea(tabs);
    providersScroll->setWidgetResizable(true);
    providersScroll->setFrameShape(QFrame::NoFrame);
    auto *providersWidget = new QWidget(providersScroll);
    auto *providersLayout = new QVBoxLayout(providersWidget);
    providersLayout->setContentsMargins(8, 8, 8, 8);
    providersLayout->setSpacing(10);

    // Default provider dropdown
    auto *defaultProviderForm = new QFormLayout;
    m_provider = new QComboBox(providersWidget);
    m_provider->addItem(providerLabel(Provider::Grok), providerId(Provider::Grok));
    m_provider->addItem(providerLabel(Provider::OpenAI), providerId(Provider::OpenAI));
    m_provider->addItem(providerLabel(Provider::OpenRouter), providerId(Provider::OpenRouter));
    m_provider->addItem(providerLabel(Provider::OpenAICompatible), providerId(Provider::OpenAICompatible));
    m_provider->addItem(providerLabel(Provider::ClaudeCompatible), providerId(Provider::ClaudeCompatible));
    m_provider->addItem(providerLabel(Provider::Kilo), providerId(Provider::Kilo));
    defaultProviderForm->addRow(i18n("Default Provider:"), m_provider);
    providersLayout->addLayout(defaultProviderForm);

    auto addProviderGroup = [&](const QString &title, QLineEdit *key, QLineEdit *model, QLineEdit *url = nullptr) {
        auto *group = new QGroupBox(title, providersWidget);
        auto *gForm = new QFormLayout(group);
        gForm->addRow(i18n("API Key:"), key);
        gForm->addRow(i18n("Default Model:"), model);
        if (url) {
            gForm->addRow(i18n("Endpoint URL:"), url);
        }
        providersLayout->addWidget(group);
    };

    m_grokKey = makeKey();
    m_grokModel = new QLineEdit(this);
    addProviderGroup(i18n("xAI (Grok)"), m_grokKey, m_grokModel);

    m_openaiKey = makeKey();
    m_openaiModel = new QLineEdit(this);
    addProviderGroup(i18n("OpenAI"), m_openaiKey, m_openaiModel);

    m_openrouterKey = makeKey();
    m_openrouterModel = new QLineEdit(this);
    addProviderGroup(i18n("OpenRouter"), m_openrouterKey, m_openrouterModel);

    m_openaiCompatibleKey = makeKey();
    m_openaiCompatibleModel = new QLineEdit(this);
    m_openaiCompatibleUrl = new QLineEdit(this);
    m_openaiCompatibleUrl->setPlaceholderText(u"http://localhost:11434/v1"_s);
    addProviderGroup(i18n("OpenAI Compatible (Ollama, LocalAI, vLLM)"), m_openaiCompatibleKey, m_openaiCompatibleModel, m_openaiCompatibleUrl);

    m_claudeCompatibleKey = makeKey();
    m_claudeCompatibleModel = new QLineEdit(this);
    m_claudeCompatibleUrl = new QLineEdit(this);
    m_claudeCompatibleUrl->setPlaceholderText(u"https://api.anthropic.com/v1"_s);
    addProviderGroup(i18n("Claude Compatible (Anthropic, Bedrock)"), m_claudeCompatibleKey, m_claudeCompatibleModel, m_claudeCompatibleUrl);

    m_kiloKey = makeKey();
    m_kiloModel = new QLineEdit(this);
    m_kiloUrl = new QLineEdit(this);
    m_kiloUrl->setPlaceholderText(u"https://api.kilo.ai/v1"_s);
    addProviderGroup(i18n("Kilo"), m_kiloKey, m_kiloModel, m_kiloUrl);

    providersLayout->addStretch();
    providersScroll->setWidget(providersWidget);
    tabs->addTab(providersScroll, i18n("AI Providers"));

    // ==========================================
    // TAB 2: Security & Permissions
    // ==========================================
    auto *securityWidget = new QWidget(tabs);
    auto *securityForm = new QFormLayout(securityWidget);
    securityForm->setContentsMargins(12, 12, 12, 12);
    securityForm->setSpacing(8);

    m_permission = new QComboBox(securityWidget);
    m_permission->addItem(permissionModeLabel(PermissionMode::Ask), permissionModeId(PermissionMode::Ask));
    m_permission->addItem(permissionModeLabel(PermissionMode::AcceptEdits), permissionModeId(PermissionMode::AcceptEdits));
    m_permission->addItem(permissionModeLabel(PermissionMode::AlwaysApprove), permissionModeId(PermissionMode::AlwaysApprove));
    securityForm->addRow(i18n("Permission mode:"), m_permission);

    m_sandbox = new QComboBox(securityWidget);
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Workspace), sandboxProfileId(SandboxProfile::Workspace));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::ReadOnly), sandboxProfileId(SandboxProfile::ReadOnly));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Strict), sandboxProfileId(SandboxProfile::Strict));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Off), sandboxProfileId(SandboxProfile::Off));
    securityForm->addRow(i18n("Sandbox profile:"), m_sandbox);

    m_timeout = new QSpinBox(securityWidget);
    m_timeout->setRange(1, 600);
    m_timeout->setSuffix(i18n(" s"));
    securityForm->addRow(i18n("Command timeout:"), m_timeout);

    m_deny = new QPlainTextEdit(securityWidget);
    m_deny->setPlaceholderText(i18n("One glob per line, e.g. **/secrets/**"));
    m_deny->setMaximumHeight(100);
    m_deny->setStyleSheet(
        u"QPlainTextEdit { background: transparent; color: #e4e4e4; border: 1px solid #38383e; border-radius: 4px; padding: 4px; font-size: 13px; }"
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
        u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);
    securityForm->addRow(i18n("Extra deny globs:"), m_deny);

    m_maxExpandedToolCards = new QSpinBox(securityWidget);
    m_maxExpandedToolCards->setRange(0, 200);
    m_maxExpandedToolCards->setSpecialValueText(i18n("Always expand all"));
    m_maxExpandedToolCards->setSuffix(i18n(" cards"));
    securityForm->addRow(i18n("Expanded tool cards before auto-collapse:"), m_maxExpandedToolCards);

    tabs->addTab(securityWidget, i18n("Security & Permissions"));

    // ==========================================
    // TAB 3: Agent & Context
    // ==========================================
    auto *agentScroll = new QScrollArea(tabs);
    agentScroll->setWidgetResizable(true);
    agentScroll->setFrameShape(QFrame::NoFrame);
    auto *agentWidget = new QWidget(agentScroll);
    auto *agentForm = new QFormLayout(agentWidget);
    agentForm->setContentsMargins(12, 12, 12, 12);
    agentForm->setSpacing(8);

    m_planMode = new QCheckBox(i18n("Only allow read-only tools and ask for an implementation plan"), agentWidget);
    agentForm->addRow(i18n("Plan mode:"), m_planMode);

    m_projectInstructions = new QCheckBox(i18n("Load KATEAI.md from workspace root"), agentWidget);
    agentForm->addRow(i18n("Project instructions:"), m_projectInstructions);

    m_speed = new QComboBox(agentWidget);
    m_speed->addItem(i18n("Slow"), 0);
    m_speed->addItem(i18n("Medium"), 1);
    m_speed->addItem(i18n("Fast"), 2);
    agentForm->addRow(i18n("Message speed:"), m_speed);

    m_thinkingMode = new QCheckBox(i18n("Enable thinking mode (model outputs reasoning before answer)"), agentWidget);
    agentForm->addRow(i18n("Thinking mode:"), m_thinkingMode);

    m_maxIter = new QSpinBox(agentWidget);
    m_maxIter->setRange(1, 500);
    agentForm->addRow(i18n("Max tool calls per turn:"), m_maxIter);

    m_maxModelRequests = new QSpinBox(agentWidget);
    m_maxModelRequests->setRange(1, 500);
    agentForm->addRow(i18n("Max model requests per task:"), m_maxModelRequests);

    m_requestsPerMinute = new QSpinBox(agentWidget);
    m_requestsPerMinute->setRange(1, 60);
    agentForm->addRow(i18n("Model requests per minute:"), m_requestsPerMinute);

    m_system = new QPlainTextEdit(agentWidget);
    m_system->setPlaceholderText(i18n("Extra system prompt (optional)"));
    m_system->setMaximumHeight(80);
    m_system->setStyleSheet(
        u"QPlainTextEdit { background: transparent; color: #e4e4e4; border: 1px solid #38383e; border-radius: 4px; padding: 4px; font-size: 13px; }"
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
        u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);
    agentForm->addRow(i18n("Extra instructions:"), m_system);

    // Context compression settings
    auto *compressionSeparator = new QLabel(i18n("--- Context Compression ---"), agentWidget);
    compressionSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    agentForm->addRow(compressionSeparator);

    m_compressionLevel = new QSpinBox(agentWidget);
    m_compressionLevel->setRange(0, 3);
    m_compressionLevel->setSuffix(i18n(" (0=full, 1=summary, 2=minimal, 3=ultra-minimal)"));
    agentForm->addRow(i18n("Context compression level:"), m_compressionLevel);

    m_maxGraphNodes = new QSpinBox(agentWidget);
    m_maxGraphNodes->setRange(10, 200);
    agentForm->addRow(i18n("Max project graph nodes:"), m_maxGraphNodes);

    m_maxGraphEdges = new QSpinBox(agentWidget);
    m_maxGraphEdges->setRange(10, 500);
    agentForm->addRow(i18n("Max project graph edges:"), m_maxGraphEdges);

    m_compressGraph = new QCheckBox(i18n("Compress project graph information"), agentWidget);
    agentForm->addRow(m_compressGraph);

    m_includeFileContents = new QCheckBox(i18n("Include file contents in project graph"), agentWidget);
    agentForm->addRow(m_includeFileContents);

    m_maxFileContentLength = new QSpinBox(agentWidget);
    m_maxFileContentLength->setRange(100, 5000);
    m_maxFileContentLength->setSuffix(i18n(" chars"));
    agentForm->addRow(i18n("Max file content length:"), m_maxFileContentLength);

    m_compressEditorContext = new QCheckBox(i18n("Compress editor context"), agentWidget);
    agentForm->addRow(m_compressEditorContext);

    m_maxEditorContextLength = new QSpinBox(agentWidget);
    m_maxEditorContextLength->setRange(50, 500);
    m_maxEditorContextLength->setSuffix(i18n(" chars"));
    agentForm->addRow(i18n("Max editor context length:"), m_maxEditorContextLength);

    m_compressProjectInstructions = new QCheckBox(i18n("Compress project instructions (KATEAI.md)"), agentWidget);
    agentForm->addRow(m_compressProjectInstructions);

    m_maxProjectInstructionsLength = new QSpinBox(agentWidget);
    m_maxProjectInstructionsLength->setRange(500, 10000);
    m_maxProjectInstructionsLength->setSuffix(i18n(" chars"));
    agentForm->addRow(i18n("Max project instructions length:"), m_maxProjectInstructionsLength);

    m_compressSystemPrompt = new QCheckBox(i18n("Compress system prompt"), agentWidget);
    agentForm->addRow(m_compressSystemPrompt);

    m_maxSystemPromptLength = new QSpinBox(agentWidget);
    m_maxSystemPromptLength->setRange(256, 2048);
    m_maxSystemPromptLength->setSuffix(i18n(" chars"));
    agentForm->addRow(i18n("Max system prompt length:"), m_maxSystemPromptLength);

    // Optimal Intelligence Parameters
    auto *intelligenceSeparator = new QLabel(i18n("--- Optimal Intelligence Parameters ---"), agentWidget);
    intelligenceSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    agentForm->addRow(intelligenceSeparator);

    m_temperature = new QDoubleSpinBox(agentWidget);
    m_temperature->setRange(0.0, 2.0);
    m_temperature->setSingleStep(0.05);
    m_temperature->setDecimals(2);
    agentForm->addRow(i18n("Temperature (0.0-2.0):"), m_temperature);

    m_topP = new QDoubleSpinBox(agentWidget);
    m_topP->setRange(0.0, 1.0);
    m_topP->setSingleStep(0.05);
    m_topP->setDecimals(2);
    agentForm->addRow(i18n("Top-p (0.0-1.0):"), m_topP);

    m_maxTokens = new QSpinBox(agentWidget);
    m_maxTokens->setRange(0, 100000);
    m_maxTokens->setSpecialValueText(i18n("Auto (provider default)"));
    agentForm->addRow(i18n("Max tokens (0=auto):"), m_maxTokens);

    m_reasoningEffort = new QComboBox(agentWidget);
    m_reasoningEffort->addItem(i18n("Auto (provider default)"), QString());
    m_reasoningEffort->addItem(i18n("Minimal"), u"minimal"_s);
    m_reasoningEffort->addItem(i18n("Low"), u"low"_s);
    m_reasoningEffort->addItem(i18n("Medium"), u"medium"_s);
    m_reasoningEffort->addItem(i18n("High"), u"high"_s);
    agentForm->addRow(i18n("Reasoning effort:"), m_reasoningEffort);

    m_selfCritique = new QCheckBox(i18n("Ask model to self-critique before finishing"), agentWidget);
    agentForm->addRow(m_selfCritique);

    m_parallelToolCalls = new QCheckBox(i18n("Allow parallel tool calls"), agentWidget);
    agentForm->addRow(m_parallelToolCalls);

    m_verbosity = new QComboBox(agentWidget);
    m_verbosity->addItem(i18n("Terse"), 0);
    m_verbosity->addItem(i18n("Normal"), 1);
    m_verbosity->addItem(i18n("Detailed"), 2);
    agentForm->addRow(i18n("Verbosity:"), m_verbosity);

    // Enhanced Intelligence Parameters
    auto *enhancedSeparator = new QLabel(i18n("--- Enhanced Intelligence ---"), agentWidget);
    enhancedSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    agentForm->addRow(enhancedSeparator);

    m_structuredThinking = new QCheckBox(i18n("Require structured thinking block before response"), agentWidget);
    agentForm->addRow(i18n("Structured thinking:"), m_structuredThinking);

    m_structuredPlanning = new QCheckBox(i18n("Require structured plan after thinking"), agentWidget);
    agentForm->addRow(i18n("Structured planning:"), m_structuredPlanning);

    m_autoCollapseThinking = new QCheckBox(i18n("Auto-collapse thinking once answer starts"), agentWidget);
    agentForm->addRow(i18n("Auto-collapse thinking:"), m_autoCollapseThinking);

    m_showPlanAsChecklist = new QCheckBox(i18n("Show plan as interactive checklist"), agentWidget);
    agentForm->addRow(i18n("Show plan checklist:"), m_showPlanAsChecklist);

    m_maxThinkingTokens = new QSpinBox(agentWidget);
    m_maxThinkingTokens->setRange(512, 32768);
    m_maxThinkingTokens->setSuffix(i18n(" tokens"));
    agentForm->addRow(i18n("Max thinking tokens:"), m_maxThinkingTokens);

    m_maxPlanSteps = new QSpinBox(agentWidget);
    m_maxPlanSteps->setRange(5, 30);
    agentForm->addRow(i18n("Max plan steps:"), m_maxPlanSteps);

    m_requireVerification = new QCheckBox(i18n("Require verification after file mutations"), agentWidget);
    agentForm->addRow(i18n("Require verification:"), m_requireVerification);

    m_maxVerificationAttempts = new QSpinBox(agentWidget);
    m_maxVerificationAttempts->setRange(1, 5);
    agentForm->addRow(i18n("Max verification attempts:"), m_maxVerificationAttempts);

    m_adaptiveTemperature = new QCheckBox(i18n("Adjust temperature based on task phase"), agentWidget);
    agentForm->addRow(i18n("Adaptive temperature:"), m_adaptiveTemperature);

    m_explorationTemperature = new QDoubleSpinBox(agentWidget);
    m_explorationTemperature->setRange(0.0, 2.0);
    m_explorationTemperature->setSingleStep(0.05);
    m_explorationTemperature->setDecimals(2);
    agentForm->addRow(i18n("Exploration temperature:"), m_explorationTemperature);

    m_exploitationTemperature = new QDoubleSpinBox(agentWidget);
    m_exploitationTemperature->setRange(0.0, 2.0);
    m_exploitationTemperature->setSingleStep(0.05);
    m_exploitationTemperature->setDecimals(2);
    agentForm->addRow(i18n("Exploitation temperature:"), m_exploitationTemperature);

    m_enablePlanUpdates = new QCheckBox(i18n("Allow plan updates during execution"), agentWidget);
    agentForm->addRow(i18n("Enable plan updates:"), m_enablePlanUpdates);

    m_narrativeProgress = new QCheckBox(i18n("Natural language progress narration"), agentWidget);
    agentForm->addRow(i18n("Narrative progress:"), m_narrativeProgress);

    // Context management for performance
    auto *contextSeparator = new QLabel(i18n("--- Context Management ---"), agentWidget);
    contextSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    agentForm->addRow(contextSeparator);

    m_smartContextTruncation = new QCheckBox(i18n("Intelligently truncate old context"), agentWidget);
    agentForm->addRow(i18n("Smart context truncation:"), m_smartContextTruncation);

    m_contextWindowReserve = new QSpinBox(agentWidget);
    m_contextWindowReserve->setRange(1024, 32768);
    m_contextWindowReserve->setSuffix(i18n(" tokens"));
    agentForm->addRow(i18n("Context window reserve:"), m_contextWindowReserve);

    m_compressOldMessages = new QCheckBox(i18n("Compress messages beyond window"), agentWidget);
    agentForm->addRow(i18n("Compress old messages:"), m_compressOldMessages);

    m_compressionThreshold = new QSpinBox(agentWidget);
    m_compressionThreshold->setRange(512, 16384);
    m_compressionThreshold->setSuffix(i18n(" chars"));
    agentForm->addRow(i18n("Compression threshold:"), m_compressionThreshold);

    agentScroll->setWidget(agentWidget);
    tabs->addTab(agentScroll, i18n("Agent & Context"));

    // Connect markChanged
    const auto markChanged = [this]() {
        Q_EMIT changed();
    };

    connect(m_provider, &QComboBox::currentIndexChanged, this, markChanged);
    connect(m_grokKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_openrouterKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiCompatibleKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_claudeCompatibleKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_grokModel, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiModel, &QLineEdit::textChanged, this, markChanged);
    connect(m_openrouterModel, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiCompatibleModel, &QLineEdit::textChanged, this, markChanged);
    connect(m_claudeCompatibleModel, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiCompatibleUrl, &QLineEdit::textChanged, this, markChanged);
    connect(m_claudeCompatibleUrl, &QLineEdit::textChanged, this, markChanged);

    connect(m_permission, &QComboBox::currentIndexChanged, this, markChanged);
    connect(m_sandbox, &QComboBox::currentIndexChanged, this, markChanged);
    connect(m_timeout, &QSpinBox::valueChanged, this, markChanged);
    connect(m_deny, &QPlainTextEdit::textChanged, this, markChanged);
    connect(m_maxExpandedToolCards, &QSpinBox::valueChanged, this, markChanged);

    connect(m_planMode, &QCheckBox::toggled, this, markChanged);
    connect(m_projectInstructions, &QCheckBox::toggled, this, markChanged);
    connect(m_speed, &QComboBox::currentIndexChanged, this, markChanged);
    connect(m_maxIter, &QSpinBox::valueChanged, this, markChanged);
    connect(m_maxModelRequests, &QSpinBox::valueChanged, this, markChanged);
    connect(m_requestsPerMinute, &QSpinBox::valueChanged, this, markChanged);
    connect(m_system, &QPlainTextEdit::textChanged, this, markChanged);

    connect(m_compressionLevel, &QSpinBox::valueChanged, this, markChanged);
    connect(m_maxGraphNodes, &QSpinBox::valueChanged, this, markChanged);
    connect(m_maxGraphEdges, &QSpinBox::valueChanged, this, markChanged);
    connect(m_compressGraph, &QCheckBox::toggled, this, markChanged);
    connect(m_includeFileContents, &QCheckBox::toggled, this, markChanged);
    connect(m_maxFileContentLength, &QSpinBox::valueChanged, this, markChanged);
    connect(m_compressEditorContext, &QCheckBox::toggled, this, markChanged);
    connect(m_maxEditorContextLength, &QSpinBox::valueChanged, this, markChanged);
    connect(m_compressProjectInstructions, &QCheckBox::toggled, this, markChanged);
    connect(m_maxProjectInstructionsLength, &QSpinBox::valueChanged, this, markChanged);
    connect(m_compressSystemPrompt, &QCheckBox::toggled, this, markChanged);
    connect(m_maxSystemPromptLength, &QSpinBox::valueChanged, this, markChanged);

    connect(m_thinkingMode, &QCheckBox::toggled, this, markChanged);
    connect(m_temperature, &QDoubleSpinBox::valueChanged, this, markChanged);
    connect(m_topP, &QDoubleSpinBox::valueChanged, this, markChanged);
    connect(m_maxTokens, &QSpinBox::valueChanged, this, markChanged);
    connect(m_reasoningEffort, &QComboBox::currentIndexChanged, this, markChanged);
    connect(m_selfCritique, &QCheckBox::toggled, this, markChanged);
    connect(m_parallelToolCalls, &QCheckBox::toggled, this, markChanged);
    connect(m_verbosity, &QComboBox::currentIndexChanged, this, markChanged);

    // Enhanced Intelligence Parameters
    connect(m_structuredThinking, &QCheckBox::toggled, this, markChanged);
    connect(m_structuredPlanning, &QCheckBox::toggled, this, markChanged);
    connect(m_autoCollapseThinking, &QCheckBox::toggled, this, markChanged);
    connect(m_showPlanAsChecklist, &QCheckBox::toggled, this, markChanged);
    connect(m_maxThinkingTokens, &QSpinBox::valueChanged, this, markChanged);
    connect(m_maxPlanSteps, &QSpinBox::valueChanged, this, markChanged);
    connect(m_requireVerification, &QCheckBox::toggled, this, markChanged);
    connect(m_maxVerificationAttempts, &QSpinBox::valueChanged, this, markChanged);
    connect(m_adaptiveTemperature, &QCheckBox::toggled, this, markChanged);
    connect(m_explorationTemperature, &QDoubleSpinBox::valueChanged, this, markChanged);
    connect(m_exploitationTemperature, &QDoubleSpinBox::valueChanged, this, markChanged);
    connect(m_enablePlanUpdates, &QCheckBox::toggled, this, markChanged);
    connect(m_narrativeProgress, &QCheckBox::toggled, this, markChanged);

    // Context Management
    connect(m_smartContextTruncation, &QCheckBox::toggled, this, markChanged);
    connect(m_contextWindowReserve, &QSpinBox::valueChanged, this, markChanged);
    connect(m_compressOldMessages, &QCheckBox::toggled, this, markChanged);
    connect(m_compressionThreshold, &QSpinBox::valueChanged, this, markChanged);

    reset();
}

KateAiConfigPage::~KateAiConfigPage() = default;

QString KateAiConfigPage::name() const
{
    return i18n("Kate AI");
}

QString KateAiConfigPage::fullName() const
{
    return i18n("Kate AI Configuration");
}

QIcon KateAiConfigPage::icon() const
{
    return QIcon::fromTheme(u"help-hint"_s);
}

void KateAiConfigPage::apply()
{
    Settings s = m_plugin->settings();
    s.provider = providerFromId(m_provider->currentData().toString());
    s.grokApiKey = m_grokKey->text();
    s.openaiApiKey = m_openaiKey->text();
    s.openrouterApiKey = m_openrouterKey->text();
    s.openaiCompatibleApiKey = m_openaiCompatibleKey->text();
    s.claudeCompatibleApiKey = m_claudeCompatibleKey->text();
    s.kiloApiKey = m_kiloKey->text();
    s.grokModel = m_grokModel->text().trimmed();
    s.openaiModel = m_openaiModel->text().trimmed();
    s.openrouterModel = m_openrouterModel->text().trimmed();
    s.openaiCompatibleModel = m_openaiCompatibleModel->text().trimmed();
    s.claudeCompatibleModel = m_claudeCompatibleModel->text().trimmed();
    s.kiloModel = m_kiloModel->text().trimmed();
    s.openaiCompatibleUrl = m_openaiCompatibleUrl->text().trimmed();
    s.claudeCompatibleUrl = m_claudeCompatibleUrl->text().trimmed();
    s.kiloUrl = m_kiloUrl->text().trimmed();

    s.permissionMode = permissionModeFromId(m_permission->currentData().toString());
    s.sandbox = sandboxProfileFromId(m_sandbox->currentData().toString());
    s.maxToolCalls = m_maxIter->value();
    s.maxIterations = s.maxToolCalls;
    s.maxModelRequests = m_maxModelRequests->value();
    s.requestsPerMinute = m_requestsPerMinute->value();
    s.bashTimeoutMs = m_timeout->value() * 1000;
    s.maxExpandedToolCards = m_maxExpandedToolCards->value();
    s.planMode = m_planMode->isChecked();
    s.loadProjectInstructions = m_projectInstructions->isChecked();
    s.thinkingMode = m_thinkingMode->isChecked();
    s.extraSystemPrompt = m_system->toPlainText();
    s.extraDenyGlobs = m_deny->toPlainText().split(u'\n', Qt::SkipEmptyParts);
    s.messageSpeed = m_speed->currentData().toInt();

    s.contextCompressionLevel = m_compressionLevel->value();
    s.maxGraphNodes = m_maxGraphNodes->value();
    s.maxGraphEdges = m_maxGraphEdges->value();
    s.compressProjectGraph = m_compressGraph->isChecked();
    s.includeFileContents = m_includeFileContents->isChecked();
    s.maxFileContentLength = m_maxFileContentLength->value();
    s.compressEditorContext = m_compressEditorContext->isChecked();
    s.maxEditorContextLength = m_maxEditorContextLength->value();
    s.compressProjectInstructions = m_compressProjectInstructions->isChecked();
    s.maxProjectInstructionsLength = m_maxProjectInstructionsLength->value();
    s.compressSystemPrompt = m_compressSystemPrompt->isChecked();
    s.maxSystemPromptLength = m_maxSystemPromptLength->value();
    s.messageSpeed = m_speed->currentData().toInt();

    // Optimal Intelligence Parameters
    s.temperature = m_temperature->value();
    s.topP = m_topP->value();
    s.maxTokens = m_maxTokens->value();
    s.reasoningEffort = m_reasoningEffort->currentData().toString();
    s.selfCritique = m_selfCritique->isChecked();
    s.parallelToolCalls = m_parallelToolCalls->isChecked();
    s.verbosity = m_verbosity->currentData().toInt();

    // Enhanced Intelligence Parameters
    s.structuredThinking = m_structuredThinking->isChecked();
    s.structuredPlanning = m_structuredPlanning->isChecked();
    s.autoCollapseThinking = m_autoCollapseThinking->isChecked();
    s.showPlanAsChecklist = m_showPlanAsChecklist->isChecked();
    s.maxThinkingTokens = m_maxThinkingTokens->value();
    s.maxPlanSteps = m_maxPlanSteps->value();
    s.requireVerification = m_requireVerification->isChecked();
    s.maxVerificationAttempts = m_maxVerificationAttempts->value();
    s.adaptiveTemperature = m_adaptiveTemperature->isChecked();
    s.explorationTemperature = m_explorationTemperature->value();
    s.exploitationTemperature = m_exploitationTemperature->value();
    s.enablePlanUpdates = m_enablePlanUpdates->isChecked();
    s.narrativeProgress = m_narrativeProgress->isChecked();

    // Context Management
    s.smartContextTruncation = m_smartContextTruncation->isChecked();
    s.contextWindowReserve = m_contextWindowReserve->value();
    s.compressOldMessages = m_compressOldMessages->isChecked();
    s.compressionThreshold = m_compressionThreshold->value();

    m_plugin->setSettings(s);
}

void KateAiConfigPage::reset()
{
    const Settings s = m_plugin->settings();
    m_provider->setCurrentIndex(std::max(0, m_provider->findData(providerId(s.provider))));
    m_grokKey->setText(s.grokApiKey);
    m_openaiKey->setText(s.openaiApiKey);
    m_openrouterKey->setText(s.openrouterApiKey);
    m_openaiCompatibleKey->setText(s.openaiCompatibleApiKey);
    m_claudeCompatibleKey->setText(s.claudeCompatibleApiKey);
    m_kiloKey->setText(s.kiloApiKey);
    m_grokModel->setText(s.grokModel);
    m_openaiModel->setText(s.openaiModel);
    m_openrouterModel->setText(s.openrouterModel);
    m_openaiCompatibleModel->setText(s.openaiCompatibleModel);
    m_claudeCompatibleModel->setText(s.claudeCompatibleModel);
    m_kiloModel->setText(s.kiloModel);
    m_openaiCompatibleUrl->setText(s.openaiCompatibleUrl);
    m_claudeCompatibleUrl->setText(s.claudeCompatibleUrl);
    m_kiloUrl->setText(s.kiloUrl);

    m_permission->setCurrentIndex(std::max(0, m_permission->findData(permissionModeId(s.permissionMode))));
    m_sandbox->setCurrentIndex(std::max(0, m_sandbox->findData(sandboxProfileId(s.sandbox))));
    m_timeout->setValue(std::max(1, s.bashTimeoutMs / 1000));
    m_deny->setPlainText(s.extraDenyGlobs.join(u'\n'));
    m_maxExpandedToolCards->setValue(s.maxExpandedToolCards);

    m_maxIter->setValue(s.maxToolCalls);
    m_maxModelRequests->setValue(s.maxModelRequests);
    m_requestsPerMinute->setValue(s.requestsPerMinute);
    m_planMode->setChecked(s.planMode);
    m_projectInstructions->setChecked(s.loadProjectInstructions);
    m_thinkingMode->setChecked(s.thinkingMode);
    m_system->setPlainText(s.extraSystemPrompt);
    m_speed->setCurrentIndex(m_speed->findData(s.messageSpeed));

    m_compressionLevel->setValue(s.contextCompressionLevel);
    m_maxGraphNodes->setValue(s.maxGraphNodes);
    m_maxGraphEdges->setValue(s.maxGraphEdges);
    m_compressGraph->setChecked(s.compressProjectGraph);
    m_includeFileContents->setChecked(s.includeFileContents);
    m_maxFileContentLength->setValue(s.maxFileContentLength);
    m_compressEditorContext->setChecked(s.compressEditorContext);
    m_maxEditorContextLength->setValue(s.maxEditorContextLength);
    m_compressProjectInstructions->setChecked(s.compressProjectInstructions);
    m_maxProjectInstructionsLength->setValue(s.maxProjectInstructionsLength);
    m_compressSystemPrompt->setChecked(s.compressSystemPrompt);
    m_maxSystemPromptLength->setValue(s.maxSystemPromptLength);

    // Optimal Intelligence Parameters
    m_temperature->setValue(s.temperature);
    m_topP->setValue(s.topP);
    m_maxTokens->setValue(s.maxTokens);
    int reasoningIdx = m_reasoningEffort->findData(s.reasoningEffort);
    m_reasoningEffort->setCurrentIndex(std::max(0, reasoningIdx));
    m_selfCritique->setChecked(s.selfCritique);
    m_parallelToolCalls->setChecked(s.parallelToolCalls);
    m_verbosity->setCurrentIndex(m_verbosity->findData(s.verbosity));

    // Enhanced Intelligence Parameters
    m_structuredThinking->setChecked(s.structuredThinking);
    m_structuredPlanning->setChecked(s.structuredPlanning);
    m_autoCollapseThinking->setChecked(s.autoCollapseThinking);
    m_showPlanAsChecklist->setChecked(s.showPlanAsChecklist);
    m_maxThinkingTokens->setValue(s.maxThinkingTokens);
    m_maxPlanSteps->setValue(s.maxPlanSteps);
    m_requireVerification->setChecked(s.requireVerification);
    m_maxVerificationAttempts->setValue(s.maxVerificationAttempts);
    m_adaptiveTemperature->setChecked(s.adaptiveTemperature);
    m_explorationTemperature->setValue(s.explorationTemperature);
    m_exploitationTemperature->setValue(s.exploitationTemperature);
    m_enablePlanUpdates->setChecked(s.enablePlanUpdates);
    m_narrativeProgress->setChecked(s.narrativeProgress);

    // Context Management
    m_smartContextTruncation->setChecked(s.smartContextTruncation);
    m_contextWindowReserve->setValue(s.contextWindowReserve);
    m_compressOldMessages->setChecked(s.compressOldMessages);
    m_compressionThreshold->setValue(s.compressionThreshold);
}

void KateAiConfigPage::defaults()
{
    m_plugin->setSettings(Settings{});
    reset();
}

} // namespace KateAi
