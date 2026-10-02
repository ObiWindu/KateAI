/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "configpage.h"
#include "plugin.h"
#include "settings.h"
#include "llmclient.h"
#include "mcp.h"
#include "modes.h"
#include "agentteam.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTreeWidget>

#include <KLocalizedString>

#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QDebug>

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
    m_provider->addItem(providerLabel(Provider::DeepSeek), providerId(Provider::DeepSeek));
    m_provider->addItem(providerLabel(Provider::OpenAICompatible), providerId(Provider::OpenAICompatible));
    m_provider->addItem(providerLabel(Provider::ClaudeCompatible), providerId(Provider::ClaudeCompatible));
        m_provider->addItem(providerLabel(Provider::OpenCode), providerId(Provider::OpenCode));
    m_provider->addItem(providerLabel(Provider::Acp), providerId(Provider::Acp));
    defaultProviderForm->addRow(i18n("Default Provider:"), m_provider);
    providersLayout->addLayout(defaultProviderForm);

    auto addProviderGroup = [&](const QString &title, QLineEdit *key, QComboBox *model, QLineEdit *url = nullptr) {
        auto *group = new QGroupBox(title, providersWidget);
        auto *gForm = new QFormLayout(group);
        gForm->addRow(i18n("API Key:"), key);
        gForm->addRow(i18n("Default Model:"), model);
        if (url) {
            gForm->addRow(i18n("Endpoint URL:"), url);
        }
        providersLayout->addWidget(group);
    };

    auto makeModelCombo = [this, providersWidget]() {
        auto *combo = new QComboBox(providersWidget);
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
        return combo;
    };

    m_grokKey = makeKey();
    m_grokModel = makeModelCombo();
    addProviderGroup(i18n("xAI (Grok)"), m_grokKey, m_grokModel);

    m_openaiKey = makeKey();
    m_openaiModel = makeModelCombo();
    addProviderGroup(i18n("OpenAI"), m_openaiKey, m_openaiModel);

    m_openrouterKey = makeKey();
    m_openrouterModel = makeModelCombo();
    addProviderGroup(i18n("OpenRouter"), m_openrouterKey, m_openrouterModel);

    m_deepseekKey = makeKey();
    m_deepseekModel = makeModelCombo();
    m_deepseekUrl = new QLineEdit(this);
    m_deepseekUrl->setPlaceholderText(u"https://api.deepseek.com"_s);
    addProviderGroup(i18n("DeepSeek"), m_deepseekKey, m_deepseekModel, m_deepseekUrl);

    m_openaiCompatibleKey = makeKey();
    m_openaiCompatibleModel = makeModelCombo();
    m_openaiCompatibleUrl = new QLineEdit(this);
    m_openaiCompatibleUrl->setPlaceholderText(u"http://localhost:11434/v1"_s);
    addProviderGroup(i18n("OpenAI Compatible (Ollama, LocalAI, vLLM)"), m_openaiCompatibleKey, m_openaiCompatibleModel, m_openaiCompatibleUrl);

    m_claudeCompatibleKey = makeKey();
    m_claudeCompatibleModel = makeModelCombo();
    m_claudeCompatibleUrl = new QLineEdit(this);
    m_claudeCompatibleUrl->setPlaceholderText(u"https://api.anthropic.com/v1"_s);
    addProviderGroup(i18n("Claude Compatible (Anthropic, Bedrock)"), m_claudeCompatibleKey, m_claudeCompatibleModel, m_claudeCompatibleUrl);

        m_opencodeKey = makeKey();
        m_opencodeModel = makeModelCombo();
        m_opencodeUrl = new QLineEdit(this);
        m_opencodeUrl->setPlaceholderText(u"https://opencode.ai/zen/v1"_s);
        addProviderGroup(i18n("OpenCode Zen (curated coding models)"), m_opencodeKey, m_opencodeModel, m_opencodeUrl);

    m_acpKey = makeKey();
    m_acpModel = makeModelCombo();
    m_acpUrl = new QLineEdit(this);
    m_acpUrl->setPlaceholderText(u"http://localhost:8080"_s);
    m_apiFormat = new QComboBox(providersWidget);
    m_apiFormat->addItem(apiFormatLabel(ApiFormat::OpenAICompatible), apiFormatId(ApiFormat::OpenAICompatible));
    m_apiFormat->addItem(apiFormatLabel(ApiFormat::AnthropicCompatible), apiFormatId(ApiFormat::AnthropicCompatible));
    m_apiFormat->addItem(apiFormatLabel(ApiFormat::AcpNative), apiFormatId(ApiFormat::AcpNative));
    auto *acpGroup = new QGroupBox(i18n("ACP (Agent Communication Protocol)"), providersWidget);
    auto *acpForm = new QFormLayout(acpGroup);
    acpForm->addRow(i18n("API Key:"), m_acpKey);
    acpForm->addRow(i18n("Default Model:"), m_acpModel);
    acpForm->addRow(i18n("Endpoint URL:"), m_acpUrl);
    acpForm->addRow(i18n("API Format:"), m_apiFormat);
    providersLayout->addWidget(acpGroup);

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
    securityForm->addRow(i18n("Recent Kate AI chats kept expanded:"), m_maxExpandedToolCards);

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

    m_maxSavedConversations = new QSpinBox(agentWidget);
    m_maxSavedConversations->setRange(0, 1000);
    m_maxSavedConversations->setSpecialValueText(i18n("Unlimited"));
    agentForm->addRow(i18n("Max saved conversations:"), m_maxSavedConversations);

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

    // --- Modes, auto-approve, and rules ---------------------------------------
    auto *modesScroll = new QScrollArea(tabs);
    modesScroll->setWidgetResizable(true);
    auto *modesWidget = new QWidget(modesScroll);
    auto *modesForm = new QFormLayout(modesWidget);
    modesForm->setContentsMargins(12, 12, 12, 12);
    modesForm->setSpacing(8);

    auto *modesSeparator = new QLabel(i18n("--- Default mode ---"), modesWidget);
    modesSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    modesForm->addRow(modesSeparator);

    m_agentMode = new QComboBox(modesWidget);
    for (const ModeDefinition &mode : ModeRegistry::builtInModes()) {
        m_agentMode->addItem(mode.name, mode.id);
    }
    modesForm->addRow(i18n("Mode used when a chat starts:"), m_agentMode);

    auto *modeHelp = new QLabel(i18n("Ask and Architect are read-only. Orchestrator delegates all work to sub-agents. "
                                     "Add your own modes as Markdown files in <b>.kateai/modes/</b>."),
                                modesWidget);
    modeHelp->setWordWrap(true);
    modeHelp->setStyleSheet(u"color: #888888;"_s);
    modesForm->addRow(modeHelp);

    auto *approveSeparator = new QLabel(i18n("--- Auto-approve (never prompt) ---"), modesWidget);
    approveSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    modesForm->addRow(approveSeparator);

    m_autoApproveTools = new QWidget(modesWidget);
    auto *approveLayout = new QVBoxLayout(m_autoApproveTools);
    approveLayout->setContentsMargins(0, 0, 0, 0);
    for (const QString &tool : allBuiltInToolNames()) {
        auto *box = new QCheckBox(tool, m_autoApproveTools);
        m_autoApproveBoxes.insert(tool, box);
        approveLayout->addWidget(box);
    }
    modesForm->addRow(i18n("Tools:"), m_autoApproveTools);

    auto *rulesSeparator = new QLabel(i18n("--- Rules ---"), modesWidget);
    rulesSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    modesForm->addRow(rulesSeparator);

    m_loadAgentRules = new QCheckBox(i18n("Load project rules (.kateai/rules, .clinerules, AGENTS.md)"), modesWidget);
    modesForm->addRow(i18n("Project rules:"), m_loadAgentRules);

    m_globalRules = new QPlainTextEdit(modesWidget);
    m_globalRules->setPlaceholderText(i18n("Rules applied to every workspace before the project rules."));
    m_globalRules->setMaximumHeight(90);
    modesForm->addRow(i18n("Global rules:"), m_globalRules);

    modesScroll->setWidget(modesWidget);
    tabs->addTab(modesScroll, i18n("Modes & Tools"));

    // --- MCP servers ----------------------------------------------------------
    auto *mcpScroll = new QScrollArea(tabs);
    mcpScroll->setWidgetResizable(true);
    auto *mcpWidget = new QWidget(mcpScroll);
    auto *mcpForm = new QFormLayout(mcpWidget);
    mcpForm->setContentsMargins(12, 12, 12, 12);
    mcpForm->setSpacing(8);

    m_mcpEnabled = new QCheckBox(i18n("Enable MCP servers"), mcpWidget);
    mcpForm->addRow(i18n("MCP:"), m_mcpEnabled);

    m_mcpAutoConnect = new QCheckBox(i18n("Connect configured servers when a workspace opens"), mcpWidget);
    mcpForm->addRow(i18n("Startup:"), m_mcpAutoConnect);

    m_mcpTimeout = new QSpinBox(mcpWidget);
    m_mcpTimeout->setRange(1000, 1800000);
    m_mcpTimeout->setSingleStep(1000);
    m_mcpTimeout->setSuffix(i18n(" ms"));
    mcpForm->addRow(i18n("Tool timeout:"), m_mcpTimeout);

    auto *mcpServersSeparator = new QLabel(i18n("--- Servers ---"), mcpWidget);
    mcpServersSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    mcpForm->addRow(mcpServersSeparator);

    m_mcpServers = new QTableWidget(mcpWidget);
    m_mcpServers->setColumnCount(3);
    m_mcpServers->setHorizontalHeaderLabels({i18n("Name"), i18n("Transport"), i18n("Endpoint")});
    m_mcpServers->horizontalHeader()->setStretchLastSection(true);
    m_mcpServers->verticalHeader()->setVisible(false);
    m_mcpServers->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mcpServers->setMinimumHeight(180);
    mcpForm->addRow(m_mcpServers);

    auto *mcpButtons = new QWidget(mcpWidget);
    auto *mcpButtonLayout = new QHBoxLayout(mcpButtons);
    mcpButtonLayout->setContentsMargins(0, 0, 0, 0);
    auto *mcpAdd = new QPushButton(i18n("Add…"), mcpButtons);
    auto *mcpEdit = new QPushButton(i18n("Edit…"), mcpButtons);
    auto *mcpRemove = new QPushButton(i18n("Remove"), mcpButtons);
    mcpButtonLayout->addWidget(mcpAdd);
    mcpButtonLayout->addWidget(mcpEdit);
    mcpButtonLayout->addWidget(mcpRemove);
    mcpButtonLayout->addStretch();
    mcpForm->addRow(mcpButtons);

    auto *mcpPathHint = new QLabel(i18n("Servers are read from <b>&lt;workspace&gt;/.kateai/mcp.json</b> and written back "
                                       "when you save here. The <b>alwaysAllow</b> list per server auto-approves "
                                       "its tools; <b>*</b> and <b>prefix*</b> patterns are supported."),
                                  mcpWidget);
    mcpPathHint->setWordWrap(true);
    mcpPathHint->setStyleSheet(u"color: #888888;"_s);
    mcpForm->addRow(mcpPathHint);

    mcpScroll->setWidget(mcpWidget);
    tabs->addTab(mcpScroll, i18n("MCP Servers"));

    // --- Web search -----------------------------------------------------------
    auto *webScroll = new QScrollArea(tabs);
    webScroll->setWidgetResizable(true);
    auto *webWidget = new QWidget(webScroll);
    auto *webForm = new QFormLayout(webWidget);
    webForm->setContentsMargins(12, 12, 12, 12);
    webForm->setSpacing(8);

    m_webProvider = new QComboBox(webWidget);
    m_webProvider->addItem(i18n("DuckDuckGo (no key required)"), QStringLiteral("duckduckgo"));
    m_webProvider->addItem(i18n("Tavily"), QStringLiteral("tavily"));
    m_webProvider->addItem(i18n("Brave Search"), QStringLiteral("brave"));
    m_webProvider->addItem(i18n("SearXNG (self-hosted)"), QStringLiteral("searxng"));
    m_webProvider->addItem(i18n("Disabled"), QStringLiteral("disabled"));
    webForm->addRow(i18n("Provider:"), m_webProvider);

    m_webApiKey = new QLineEdit(webWidget);
    m_webApiKey->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    m_webApiKey->setPlaceholderText(i18n("Required by Tavily and Brave"));
    webForm->addRow(i18n("API key:"), m_webApiKey);

    m_webEndpoint = new QLineEdit(webWidget);
    m_webEndpoint->setPlaceholderText(QStringLiteral("http://localhost:8888"));
    webForm->addRow(i18n("SearXNG URL:"), m_webEndpoint);

    m_webMaxResults = new QSpinBox(webWidget);
    m_webMaxResults->setRange(1, 20);
    webForm->addRow(i18n("Results per search:"), m_webMaxResults);

    m_webTimeout = new QSpinBox(webWidget);
    m_webTimeout->setRange(1, 120);
    m_webTimeout->setSuffix(i18n(" s"));
    webForm->addRow(i18n("Timeout:"), m_webTimeout);

    auto *webHint = new QLabel(i18n("web_search finds pages; web_fetch reads one and returns its text. "
                                    "Both are read-only: they cannot modify your workspace. "
                                    "Only the query text is sent to the provider, never file contents."),
                               webWidget);
    webHint->setWordWrap(true);
    webHint->setStyleSheet(u"color: #888888;"_s);
    webForm->addRow(webHint);

    // The API key and SearXNG URL only matter for some providers; hiding the
    // irrelevant rows keeps the tab to the choices that actually apply.
    auto syncWebRows = [this, webForm] {
        const QString provider = m_webProvider->currentData().toString();
        const bool needsKey = provider == QStringLiteral("tavily") || provider == QStringLiteral("brave");
        const bool needsEndpoint = provider == QStringLiteral("searxng");
        for (QWidget *widget : {static_cast<QWidget *>(m_webApiKey), static_cast<QWidget *>(m_webEndpoint)}) {
            if (widget->parentWidget()) {
                const int index = webForm->indexOf(widget);
                if (index >= 0) {
                    webForm->setRowVisible(index, (widget == m_webApiKey) ? needsKey : needsEndpoint);
                }
            }
        }
    };
    connect(m_webProvider, &QComboBox::currentIndexChanged, this, syncWebRows);
    syncWebRows();

    webScroll->setWidget(webWidget);
    tabs->addTab(webScroll, i18n("Web Search"));

    // --- Checkpoints and subtasks ---------------------------------------------
    auto *safetyScroll = new QScrollArea(tabs);
    safetyScroll->setWidgetResizable(true);
    auto *safetyWidget = new QWidget(safetyScroll);
    auto *safetyForm = new QFormLayout(safetyWidget);
    safetyForm->setContentsMargins(12, 12, 12, 12);
    safetyForm->setSpacing(8);

    auto *cpSeparator = new QLabel(i18n("--- Checkpoints ---"), safetyWidget);
    cpSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    safetyForm->addRow(cpSeparator);

    m_checkpointsEnabled = new QCheckBox(i18n("Snapshot the workspace before the agent changes files"), safetyWidget);
    safetyForm->addRow(i18n("Checkpoints:"), m_checkpointsEnabled);

    m_checkpointRetention = new QSpinBox(safetyWidget);
    m_checkpointRetention->setRange(2, 200);
    safetyForm->addRow(i18n("Checkpoints to keep:"), m_checkpointRetention);

    auto *cpHelp = new QLabel(i18n("Snapshots are stored in a private git repository in the cache directory, so your own "
                                   "history is never touched. Restore or diff any snapshot from the toolbar menu."),
                              safetyWidget);
    cpHelp->setWordWrap(true);
    cpHelp->setStyleSheet(u"color: #888888;"_s);
    safetyForm->addRow(cpHelp);

    auto *subSeparator = new QLabel(i18n("--- Sub-agents ---"), safetyWidget);
    subSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    safetyForm->addRow(subSeparator);

    m_maxSubtaskDepth = new QSpinBox(safetyWidget);
    m_maxSubtaskDepth->setRange(0, 5);
    safetyForm->addRow(i18n("Max subtask depth:"), m_maxSubtaskDepth);

    m_maxParallelSubtasks = new QSpinBox(safetyWidget);
    m_maxParallelSubtasks->setRange(1, 12);
    safetyForm->addRow(i18n("Parallel sub-agents:"), m_maxParallelSubtasks);

    m_subtaskTimeout = new QSpinBox(safetyWidget);
    m_subtaskTimeout->setRange(10000, 1800000);
    m_subtaskTimeout->setSingleStep(10000);
    m_subtaskTimeout->setSuffix(i18n(" ms"));
    safetyForm->addRow(i18n("Subtask timeout:"), m_subtaskTimeout);

    safetyScroll->setWidget(safetyWidget);
    tabs->addTab(safetyScroll, i18n("Checkpoints & Subtasks"));

    // --- Agent team --------------------------------------------------------------
    auto *teamScroll = new QScrollArea(tabs);
    teamScroll->setWidgetResizable(true);
    auto *teamWidget = new QWidget(teamScroll);
    auto *teamForm = new QFormLayout(teamWidget);
    teamForm->setContentsMargins(12, 12, 12, 12);
    teamForm->setSpacing(8);

    auto *rosterSeparator = new QLabel(i18n("--- Built-in agents ---"), teamWidget);
    rosterSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    teamForm->addRow(rosterSeparator);

    m_builtinRoster = new QTreeWidget(teamWidget);
    m_builtinRoster->setColumnCount(3);
    m_builtinRoster->setHeaderLabels({i18n("Agent"), i18n("Mode"), i18n("Use it for")});
    m_builtinRoster->setRootIsDecorated(false);
    m_builtinRoster->header()->setStretchLastSection(true);
    for (const AgentProfile &profile : AgentTeam::builtinAgents()) {
        auto *item = new QTreeWidgetItem(m_builtinRoster);
        item->setText(0, profile.name);
        item->setText(1, profile.modeId);
        item->setText(2, profile.description);
        item->setToolTip(2, profile.description);
    }
    m_builtinRoster->setMinimumHeight(140);
    teamForm->addRow(m_builtinRoster);

    auto *customSeparator = new QLabel(i18n("--- Custom agents ---"), teamWidget);
    customSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    teamForm->addRow(customSeparator);

    m_customAgents = new QTableWidget(teamWidget);
    m_customAgents->setColumnCount(4);
    m_customAgents->setHorizontalHeaderLabels({i18n("Id"), i18n("Name"), i18n("Mode"), i18n("Description")});
    m_customAgents->horizontalHeader()->setStretchLastSection(true);
    m_customAgents->verticalHeader()->setVisible(false);
    m_customAgents->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_customAgents->setMinimumHeight(120);
    teamForm->addRow(m_customAgents);

    auto *agentButtons = new QWidget(teamWidget);
    auto *agentButtonLayout = new QHBoxLayout(agentButtons);
    agentButtonLayout->setContentsMargins(0, 0, 0, 0);
    auto *agentAdd = new QPushButton(i18n("Add…"), agentButtons);
    auto *agentEdit = new QPushButton(i18n("Edit…"), agentButtons);
    auto *agentRemove = new QPushButton(i18n("Remove"), agentButtons);
    agentButtonLayout->addWidget(agentAdd);
    agentButtonLayout->addWidget(agentEdit);
    agentButtonLayout->addWidget(agentRemove);
    agentButtonLayout->addStretch();
    teamForm->addRow(agentButtons);

    auto *agentHint = new QLabel(i18n("Custom agents can also be shared with the project as Markdown files in "
                                      "<b>&lt;workspace&gt;/.kateai/agents/</b> with <b>id</b>, <b>name</b>, "
                                      "<b>mode</b>, and <b>description</b> frontmatter."),
                                   teamWidget);
    agentHint->setWordWrap(true);
    agentHint->setStyleSheet(u"color: #888888;"_s);
    teamForm->addRow(agentHint);

    teamScroll->setWidget(teamWidget);
    tabs->addTab(teamScroll, i18n("Agent Team"));

    // Initialize model fetcher
    m_modelFetcher = new LlmClient(this);
    m_modelFetcher->setSettings(m_plugin->settings());
    connect(m_modelFetcher, &LlmClient::modelsReceived, this, [this](Provider provider, const QStringList &models) {
        m_modelCatalog[provider] = models;
        updateModelCombo(provider);
    });
    connect(m_modelFetcher, &LlmClient::modelsFailed, this, [this](Provider provider, const QString &error) {
        // Silently ignore model fetch failures - user can still type manually
        Q_UNUSED(provider);
        Q_UNUSED(error);
    });

    // Connect markChanged
    const auto markChanged = [this]() {
        Q_EMIT changed();
    };

    connect(m_provider, &QComboBox::currentIndexChanged, this, [this, markChanged]() {
        markChanged();
        // Refresh models for the newly selected provider
        const Provider provider = providerFromId(m_provider->currentData().toString());
        if (m_modelFetcher) {
            Settings s = m_plugin->settings();
            s.provider = provider;
            m_modelFetcher->setSettings(s);
            m_modelFetcher->fetchModels(provider);
        }
    });
    connect(m_grokKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_openrouterKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_deepseekKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiCompatibleKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_claudeCompatibleKey, &QLineEdit::textChanged, this, markChanged);
        connect(m_opencodeKey, &QLineEdit::textChanged, this, markChanged);
        connect(m_opencodeUrl, &QLineEdit::textChanged, this, markChanged);
        connect(m_opencodeModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, markChanged);
    connect(m_grokModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, markChanged);
    connect(m_openaiModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, markChanged);
    connect(m_openrouterModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, markChanged);
    connect(m_deepseekModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, markChanged);
    connect(m_openaiCompatibleModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, markChanged);
    connect(m_claudeCompatibleModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, markChanged);
    connect(m_acpModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, markChanged);
    connect(m_deepseekUrl, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiCompatibleUrl, &QLineEdit::textChanged, this, markChanged);
    connect(m_claudeCompatibleUrl, &QLineEdit::textChanged, this, markChanged);
    connect(m_acpUrl, &QLineEdit::textChanged, this, markChanged);
    connect(m_apiFormat, &QComboBox::currentIndexChanged, this, markChanged);

    // Connect API key changes to fetch models
    auto fetchModelsForProvider = [this](Provider provider, QLineEdit *keyEdit, QComboBox *modelCombo) {
        connect(keyEdit, &QLineEdit::textChanged, this, [this, provider, keyEdit, modelCombo]() {
            QString key = keyEdit->text().trimmed();
            if (!key.isEmpty()) {
                Settings s = m_plugin->settings();
                // Update the correct API key field for the provider
                switch (provider) {
                    case Provider::Grok:
                        s.grokApiKey = key;
                        break;
                    case Provider::OpenAI:
                        s.openaiApiKey = key;
                        break;
                    case Provider::OpenRouter:
                        s.openrouterApiKey = key;
                        break;
                    case Provider::DeepSeek:
                        s.deepseekApiKey = key;
                        break;
                    case Provider::OpenAICompatible:
                        s.openaiCompatibleApiKey = key;
                        break;
                    case Provider::ClaudeCompatible:
                        s.claudeCompatibleApiKey = key;
                        break;
                    case Provider::Acp:
                        s.acpApiKey = key;
                        break;
                    default:
                        break;
                }
                s.provider = provider;
                m_modelFetcher->setSettings(s);
                m_modelFetcher->fetchModels(provider);
            } else {
                modelCombo->clear();
                m_modelCatalog.remove(provider);
            }
        });
    };
    fetchModelsForProvider(Provider::Grok, m_grokKey, m_grokModel);
    fetchModelsForProvider(Provider::OpenAI, m_openaiKey, m_openaiModel);
    fetchModelsForProvider(Provider::OpenRouter, m_openrouterKey, m_openrouterModel);
    fetchModelsForProvider(Provider::DeepSeek, m_deepseekKey, m_deepseekModel);
    fetchModelsForProvider(Provider::OpenAICompatible, m_openaiCompatibleKey, m_openaiCompatibleModel);
    fetchModelsForProvider(Provider::ClaudeCompatible, m_claudeCompatibleKey, m_claudeCompatibleModel);
        fetchModelsForProvider(Provider::OpenCode, m_opencodeKey, m_opencodeModel);
    fetchModelsForProvider(Provider::Acp, m_acpKey, m_acpModel);

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

    // Modes, auto-approve and rules
    connect(m_agentMode, &QComboBox::currentIndexChanged, this, markChanged);
    for (auto it = m_autoApproveBoxes.constBegin(); it != m_autoApproveBoxes.constEnd(); ++it) {
        connect(it.value(), &QCheckBox::toggled, this, markChanged);
    }
    connect(m_loadAgentRules, &QCheckBox::toggled, this, markChanged);
    connect(m_globalRules, &QPlainTextEdit::textChanged, this, markChanged);

    // MCP
    connect(m_mcpEnabled, &QCheckBox::toggled, this, markChanged);
    connect(m_mcpAutoConnect, &QCheckBox::toggled, this, markChanged);
    connect(m_mcpTimeout, &QSpinBox::valueChanged, this, markChanged);

    // Web search
    connect(m_webProvider, &QComboBox::currentIndexChanged, this, markChanged);
    connect(m_webApiKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_webEndpoint, &QLineEdit::textChanged, this, markChanged);
    connect(m_webMaxResults, &QSpinBox::valueChanged, this, markChanged);
    connect(m_webTimeout, &QSpinBox::valueChanged, this, markChanged);

    // Checkpoints and subtasks
    connect(m_checkpointsEnabled, &QCheckBox::toggled, this, markChanged);
    connect(m_checkpointRetention, &QSpinBox::valueChanged, this, markChanged);
    connect(m_maxSubtaskDepth, &QSpinBox::valueChanged, this, markChanged);
    connect(m_maxParallelSubtasks, &QSpinBox::valueChanged, this, markChanged);
    connect(m_subtaskTimeout, &QSpinBox::valueChanged, this, markChanged);

    connect(agentAdd, &QPushButton::clicked, this, &KateAiConfigPage::addCustomAgent);
    connect(agentEdit, &QPushButton::clicked, this, &KateAiConfigPage::editCustomAgent);
    connect(agentRemove, &QPushButton::clicked, this, &KateAiConfigPage::removeCustomAgent);
    connect(m_customAgents, &QTableWidget::itemDoubleClicked, this, [this] {
        editCustomAgent();
    });

    connect(mcpAdd, &QPushButton::clicked, this, &KateAiConfigPage::addMcpServer);
    connect(mcpEdit, &QPushButton::clicked, this, &KateAiConfigPage::editMcpServer);
    connect(mcpRemove, &QPushButton::clicked, this, &KateAiConfigPage::removeMcpServer);
    connect(m_mcpServers, &QTableWidget::itemDoubleClicked, this, [this] {
        editMcpServer();
    });

    reset();
}

KateAiConfigPage::~KateAiConfigPage() = default;

void KateAiConfigPage::refreshCustomAgentTable()
{
    m_customAgents->setRowCount(0);
    for (const AgentProfile &profile : m_customAgentList) {
        const int row = m_customAgents->rowCount();
        m_customAgents->insertRow(row);
        m_customAgents->setItem(row, 0, new QTableWidgetItem(profile.id));
        m_customAgents->setItem(row, 1, new QTableWidgetItem(profile.name));
        m_customAgents->setItem(row, 2, new QTableWidgetItem(profile.modeId));
        m_customAgents->setItem(row, 3, new QTableWidgetItem(profile.description));
    }
}

void KateAiConfigPage::addCustomAgent()
{
    AgentProfile profile;
    profile.id = i18n("my-agent");
    profile.name = profile.id;
    profile.modeId = u"code"_s;
    m_customAgentList.append(profile);
    refreshCustomAgentTable();
    m_customAgents->selectRow(m_customAgents->rowCount() - 1);
    editCustomAgent();
}

void KateAiConfigPage::removeCustomAgent()
{
    const int row = m_customAgents->currentRow();
    if (row < 0 || row >= m_customAgentList.size()) {
        return;
    }
    m_customAgentList.removeAt(row);
    refreshCustomAgentTable();
    Q_EMIT changed();
}

void KateAiConfigPage::editCustomAgent()
{
    const int row = m_customAgents->currentRow();
    if (row < 0 || row >= m_customAgentList.size()) {
        return;
    }
    const AgentProfile existing = m_customAgentList.at(row);

    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Custom Agent"));
    dialog.setMinimumWidth(560);
    auto *form = new QFormLayout(&dialog);

    auto *idEdit = new QLineEdit(existing.id, &dialog);
    form->addRow(i18n("Id:"), idEdit);

    auto *nameEdit = new QLineEdit(existing.name, &dialog);
    form->addRow(i18n("Name:"), nameEdit);

    auto *modeCombo = new QComboBox(&dialog);
    for (const ModeDefinition &mode : ModeRegistry::builtInModes()) {
        modeCombo->addItem(QStringLiteral("%1 (%2)").arg(mode.name, mode.id), mode.id);
    }
    const int existingIndex = modeCombo->findData(existing.modeId);
    modeCombo->setCurrentIndex(existingIndex >= 0 ? existingIndex : 0);
    form->addRow(i18n("Mode:"), modeCombo);

    auto *descriptionEdit = new QPlainTextEdit(existing.description, &dialog);
    descriptionEdit->setMaximumHeight(90);
    descriptionEdit->setPlaceholderText(i18n("Shown to the orchestrator when it decides who to delegate to."));
    form->addRow(i18n("Description:"), descriptionEdit);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    AgentProfile profile;
    profile.id = ModeRegistry::slugify(idEdit->text().trimmed());
    profile.name = nameEdit->text().trimmed().isEmpty() ? profile.id : nameEdit->text().trimmed();
    profile.modeId = modeCombo->currentData().toString();
    profile.description = descriptionEdit->toPlainText().trimmed();
    profile.enabled = true;

    if (profile.id.isEmpty()) {
        QMessageBox::warning(this, i18n("Custom Agent"), i18n("The agent needs an id made of letters, digits, or dashes."));
        return;
    }
    // A custom agent must not shadow a built-in one.
    for (const AgentProfile &builtin : AgentTeam::builtinAgents()) {
        if (builtin.id == profile.id) {
            QMessageBox::warning(this, i18n("Custom Agent"), i18n("'%1' is a built-in agent. Choose another id.", profile.id));
            return;
        }
    }

    m_customAgentList.removeAt(row);
    m_customAgentList.append(profile);
    refreshCustomAgentTable();
    m_customAgents->selectRow(m_customAgents->rowCount() - 1);
    Q_EMIT changed();
}

void KateAiConfigPage::refreshMcpServerTable()
{
    m_mcpServers->setRowCount(0);
    for (const McpServerConfig &config : m_mcpServerConfigs) {
        const int row = m_mcpServers->rowCount();
        m_mcpServers->insertRow(row);
        QString endpoint = config.url;
        if (config.transport != McpTransport::Http) {
            endpoint = config.command;
            for (const QString &arg : config.args) {
                endpoint += QLatin1Char(' ') + arg;
            }
        }
        m_mcpServers->setItem(row, 0, new QTableWidgetItem(config.enabled ? config.name : config.name + i18n(" (disabled)")));
        m_mcpServers->setItem(row, 1, new QTableWidgetItem(config.transportId()));
        m_mcpServers->setItem(row, 2, new QTableWidgetItem(endpoint));
    }
}

void KateAiConfigPage::addMcpServer()
{
    McpServerConfig config;
    config.name = i18n("new-server");
    m_mcpServerConfigs.append(config);
    refreshMcpServerTable();
    m_mcpServers->selectRow(m_mcpServers->rowCount() - 1);
    editMcpServer();
}

void KateAiConfigPage::removeMcpServer()
{
    const int row = m_mcpServers->currentRow();
    if (row < 0 || row >= m_mcpServerConfigs.size()) {
        return;
    }
    m_mcpServerConfigs.removeAt(row);
    refreshMcpServerTable();
    Q_EMIT changed();
}

void KateAiConfigPage::editMcpServer()
{
    const int row = m_mcpServers->currentRow();
    if (row < 0 || row >= m_mcpServerConfigs.size()) {
        return;
    }
    McpServerConfig config = m_mcpServerConfigs.at(row);

    QDialog dialog(this);
    dialog.setWindowTitle(i18n("MCP Server"));
    dialog.setMinimumWidth(560);
    auto *form = new QFormLayout(&dialog);

    auto *nameEdit = new QLineEdit(config.name, &dialog);
    form->addRow(i18n("Name:"), nameEdit);

    auto *transportCombo = new QComboBox(&dialog);
    transportCombo->addItem(i18n("stdio (local process)"), QStringLiteral("stdio"));
    transportCombo->addItem(i18n("HTTP (remote server)"), QStringLiteral("http"));
    transportCombo->setCurrentIndex(config.transport == McpTransport::Http ? 1 : 0);
    form->addRow(i18n("Transport:"), transportCombo);

    auto *commandEdit = new QLineEdit(config.command, &dialog);
    commandEdit->setPlaceholderText(i18n("npx"));
    form->addRow(i18n("Command:"), commandEdit);

    auto *argsEdit = new QLineEdit(config.args.join(u' '), &dialog);
    argsEdit->setPlaceholderText(i18n("-y @modelcontextprotocol/server-filesystem /path"));
    form->addRow(i18n("Arguments:"), argsEdit);

    auto *urlEdit = new QLineEdit(config.url, &dialog);
    urlEdit->setPlaceholderText(QStringLiteral("https://example.com/mcp"));
    form->addRow(i18n("URL:"), urlEdit);

    auto *cwdEdit = new QLineEdit(config.cwd, &dialog);
    form->addRow(i18n("Working directory:"), cwdEdit);

    auto *allowEdit = new QLineEdit(config.alwaysAllow.join(u", "_s), &dialog);
    allowEdit->setPlaceholderText(i18n("tool_name, prefix*, *"));
    form->addRow(i18n("Always allow:"), allowEdit);

    auto *timeoutSpin = new QSpinBox(&dialog);
    timeoutSpin->setRange(1000, 1800000);
    timeoutSpin->setSingleStep(1000);
    timeoutSpin->setSuffix(i18n(" ms"));
    timeoutSpin->setValue(config.timeoutMs);
    form->addRow(i18n("Timeout:"), timeoutSpin);

    auto *enabledBox = new QCheckBox(i18n("Enabled"), &dialog);
    enabledBox->setChecked(config.enabled);
    form->addRow(enabledBox);

    auto syncVisibility = [&dialog, transportCombo, commandEdit, argsEdit, urlEdit, cwdEdit]() {
        const bool http = transportCombo->currentData().toString() == QStringLiteral("http");
        commandEdit->setEnabled(!http);
        argsEdit->setEnabled(!http);
        cwdEdit->setEnabled(!http);
        urlEdit->setEnabled(http);
    };
    connect(transportCombo, &QComboBox::currentIndexChanged, &dialog, syncVisibility);
    syncVisibility();

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString name = nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, i18n("MCP Server"), i18n("The server needs a name."));
        return;
    }
    QStringList args;
    const QStringList rawArgs = argsEdit->text().split(u' ', Qt::SkipEmptyParts);
    for (const QString &arg : rawArgs) {
        args.append(arg);
    }
    QStringList alwaysAllow;
    const QStringList rawAllow = allowEdit->text().split(u',', Qt::SkipEmptyParts);
    for (const QString &entry : rawAllow) {
        const QString trimmed = entry.trimmed();
        if (!trimmed.isEmpty()) {
            alwaysAllow.append(trimmed);
        }
    }

    McpServerConfig updated;
    updated.name = name;
    updated.transport = transportCombo->currentData().toString() == QStringLiteral("http") ? McpTransport::Http : McpTransport::Stdio;
    updated.command = commandEdit->text().trimmed();
    updated.args = args;
    updated.cwd = cwdEdit->text().trimmed();
    updated.url = urlEdit->text().trimmed();
    updated.alwaysAllow = alwaysAllow;
    updated.timeoutMs = timeoutSpin->value();
    updated.enabled = enabledBox->isChecked();

    QString error;
    if (!updated.isValid(&error)) {
        QMessageBox::warning(this, i18n("MCP Server"), error);
        return;
    }
    // Renaming a server must not leave the old entry behind.
    m_mcpServerConfigs.removeAt(row);
    m_mcpServerConfigs.append(updated);
    refreshMcpServerTable();
    m_mcpServers->selectRow(m_mcpServers->rowCount() - 1);
    Q_EMIT changed();
}

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
    s.deepseekApiKey = m_deepseekKey->text();
    s.openaiCompatibleApiKey = m_openaiCompatibleKey->text();
    s.claudeCompatibleApiKey = m_claudeCompatibleKey->text();
        s.opencodeApiKey = m_opencodeKey->text();
    s.acpApiKey = m_acpKey->text();
    s.grokModel = m_grokModel->currentText().trimmed();
    s.openaiModel = m_openaiModel->currentText().trimmed();
    s.openrouterModel = m_openrouterModel->currentText().trimmed();
    s.deepseekModel = m_deepseekModel->currentText().trimmed();
    s.openaiCompatibleModel = m_openaiCompatibleModel->currentText().trimmed();
    s.claudeCompatibleModel = m_claudeCompatibleModel->currentText().trimmed();
        s.opencodeModel = m_opencodeModel->currentText().trimmed();
        s.opencodeUrl = m_opencodeUrl->text().trimmed();
    s.acpModel = m_acpModel->currentText().trimmed();
    s.deepseekUrl = m_deepseekUrl->text().trimmed();
    s.openaiCompatibleUrl = m_openaiCompatibleUrl->text().trimmed();
    s.claudeCompatibleUrl = m_claudeCompatibleUrl->text().trimmed();
    s.acpUrl = m_acpUrl->text().trimmed();
    s.apiFormat = apiFormatFromId(m_apiFormat->currentData().toString());

    s.permissionMode = permissionModeFromId(m_permission->currentData().toString());
    s.sandbox = sandboxProfileFromId(m_sandbox->currentData().toString());
    s.maxToolCalls = m_maxIter->value();
    s.maxIterations = s.maxToolCalls;
    s.maxModelRequests = m_maxModelRequests->value();
    s.requestsPerMinute = m_requestsPerMinute->value();
    s.maxSavedConversations = m_maxSavedConversations->value();
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

    // Modes, auto-approve and rules
    s.agentMode = m_agentMode->currentData().toString();
    s.autoApproveTools.clear();
    for (auto it = m_autoApproveBoxes.constBegin(); it != m_autoApproveBoxes.constEnd(); ++it) {
        if (it.value()->isChecked()) {
            s.autoApproveTools.append(it.key());
        }
    }
    s.loadAgentRules = m_loadAgentRules->isChecked();
    s.globalRules = m_globalRules->toPlainText();

    // MCP
    s.mcpEnabled = m_mcpEnabled->isChecked();
    s.mcpAutoConnect = m_mcpAutoConnect->isChecked();
    s.mcpTimeoutMs = m_mcpTimeout->value();
        // Web search
        s.webSearchProvider = m_webProvider->currentData().toString();
        s.webSearchApiKey = m_webApiKey->text().trimmed();
        s.webSearchEndpoint = m_webEndpoint->text().trimmed();
        s.webSearchMaxResults = m_webMaxResults->value();
        s.webSearchTimeoutMs = m_webTimeout->value() * 1000;

    // Checkpoints and subtasks
    s.checkpointsEnabled = m_checkpointsEnabled->isChecked();
    s.checkpointRetention = m_checkpointRetention->value();
    s.maxSubtaskDepth = m_maxSubtaskDepth->value();
    s.maxParallelSubtasks = m_maxParallelSubtasks->value();
    s.subtaskTimeoutMs = m_subtaskTimeout->value();

    // The roster is stored as a JSON array so it stays readable in the config
    // file and can be copied between machines.
    QJsonArray rosterArray;
    for (const AgentProfile &profile : m_customAgentList) {
        rosterArray.append(profile.toJson());
    }
    s.agentRoster = rosterArray.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(rosterArray).toJson(QJsonDocument::Compact));

    if (!m_workspace.isEmpty()) {
        QString error;
        if (!McpConfigStore::save(m_workspace, m_mcpServerConfigs, &error)) {
            qWarning().noquote() << u"Kate AI could not save MCP servers:"_s << error;
        }
    }

    m_plugin->setSettings(s);
}

void KateAiConfigPage::reset()
{
    const Settings s = m_plugin->settings();
    m_provider->setCurrentIndex(std::max(0, m_provider->findData(providerId(s.provider))));
    m_grokKey->setText(s.grokApiKey);
    m_openaiKey->setText(s.openaiApiKey);
    m_openrouterKey->setText(s.openrouterApiKey);
    m_deepseekKey->setText(s.deepseekApiKey);
    m_openaiCompatibleKey->setText(s.openaiCompatibleApiKey);
    m_claudeCompatibleKey->setText(s.claudeCompatibleApiKey);
        m_opencodeKey->setText(s.opencodeApiKey);
        m_opencodeUrl->setText(s.opencodeUrl);
    m_acpKey->setText(s.acpApiKey);

    // Update model combos with catalog and set current model
    updateModelCombo(Provider::Grok);
    updateModelCombo(Provider::OpenAI);
    updateModelCombo(Provider::OpenRouter);
    updateModelCombo(Provider::DeepSeek);
    updateModelCombo(Provider::OpenAICompatible);
    updateModelCombo(Provider::ClaudeCompatible);
    updateModelCombo(Provider::Acp);

    m_grokModel->setCurrentText(s.grokModel);
    m_openaiModel->setCurrentText(s.openaiModel);
    m_openrouterModel->setCurrentText(s.openrouterModel);
    m_deepseekModel->setCurrentText(s.deepseekModel);
    m_openaiCompatibleModel->setCurrentText(s.openaiCompatibleModel);
    m_claudeCompatibleModel->setCurrentText(s.claudeCompatibleModel);
        m_opencodeModel->setCurrentText(s.opencodeModel);
    m_acpModel->setCurrentText(s.acpModel);

    m_deepseekUrl->setText(s.deepseekUrl);
    m_openaiCompatibleUrl->setText(s.openaiCompatibleUrl);
    m_claudeCompatibleUrl->setText(s.claudeCompatibleUrl);
    m_acpUrl->setText(s.acpUrl);
    m_apiFormat->setCurrentIndex(std::max(0, m_apiFormat->findData(apiFormatId(s.apiFormat))));

    // Fetch models for providers that have API keys configured
    if (m_modelFetcher) {
        auto fetchIfKey = [this, &s](Provider provider, const QString &key) {
            if (!key.trimmed().isEmpty()) {
                Settings providerSettings = s;
                providerSettings.provider = provider;
                m_modelFetcher->setSettings(providerSettings);
                m_modelFetcher->fetchModels(provider);
            }
        };
        fetchIfKey(Provider::Grok, s.grokApiKey);
        fetchIfKey(Provider::OpenAI, s.openaiApiKey);
        fetchIfKey(Provider::OpenRouter, s.openrouterApiKey);
        fetchIfKey(Provider::DeepSeek, s.deepseekApiKey);
        fetchIfKey(Provider::OpenAICompatible, s.openaiCompatibleApiKey);
        fetchIfKey(Provider::ClaudeCompatible, s.claudeCompatibleApiKey);
        fetchIfKey(Provider::Acp, s.acpApiKey);
    }

    m_permission->setCurrentIndex(std::max(0, m_permission->findData(permissionModeId(s.permissionMode))));
    m_sandbox->setCurrentIndex(std::max(0, m_sandbox->findData(sandboxProfileId(s.sandbox))));
    m_timeout->setValue(std::max(1, s.bashTimeoutMs / 1000));
    m_deny->setPlainText(s.extraDenyGlobs.join(u'\n'));
    m_maxExpandedToolCards->setValue(s.maxExpandedToolCards);

    m_maxIter->setValue(s.maxToolCalls);
    m_maxModelRequests->setValue(s.maxModelRequests);
    m_requestsPerMinute->setValue(s.requestsPerMinute);
    m_maxSavedConversations->setValue(s.maxSavedConversations);
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

    // Modes, auto-approve and rules
    int modeIndex = m_agentMode->findData(s.agentMode);
    if (modeIndex < 0) {
        modeIndex = m_agentMode->findData(QStringLiteral("code"));
    }
    m_agentMode->setCurrentIndex(std::max(0, modeIndex));
    for (auto it = m_autoApproveBoxes.constBegin(); it != m_autoApproveBoxes.constEnd(); ++it) {
        it.value()->setChecked(s.autoApproveTools.contains(it.key()));
    }
    m_loadAgentRules->setChecked(s.loadAgentRules);
    m_globalRules->setPlainText(s.globalRules);

    // MCP
    m_mcpEnabled->setChecked(s.mcpEnabled);
    m_mcpAutoConnect->setChecked(s.mcpAutoConnect);
    m_mcpTimeout->setValue(s.mcpTimeoutMs);
        // Web search
        {
            const int index = m_webProvider->findData(s.webSearchProvider);
            m_webProvider->setCurrentIndex(index >= 0 ? index : 0);
            m_webApiKey->setText(s.webSearchApiKey);
            m_webEndpoint->setText(s.webSearchEndpoint);
            m_webMaxResults->setValue(qBound(1, s.webSearchMaxResults, 20));
            m_webTimeout->setValue(qBound(1, s.webSearchTimeoutMs / 1000, 120));
        }
    if (!m_mcpServerConfigs.isEmpty()) {
        refreshMcpServerTable();
    }

    // Checkpoints and subtasks
    m_checkpointsEnabled->setChecked(s.checkpointsEnabled);
    m_checkpointRetention->setValue(s.checkpointRetention);

    m_customAgentList.clear();
    const QString roster = s.agentRoster.trimmed();
    if (!roster.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(roster.toUtf8());
        if (document.isArray()) {
            const QJsonArray array = document.array();
            for (const QJsonValue &value : array) {
                const AgentProfile profile = AgentProfile::fromJson(value.toObject());
                if (profile.isValid()) {
                    m_customAgentList.append(profile);
                }
            }
        }
    }
    refreshCustomAgentTable();
    m_maxSubtaskDepth->setValue(s.maxSubtaskDepth);
    m_maxParallelSubtasks->setValue(s.maxParallelSubtasks);
    m_subtaskTimeout->setValue(s.subtaskTimeoutMs);
}

void KateAiConfigPage::setWorkspace(const QString &workspace)
{
    if (m_workspace == workspace) {
        return;
    }
    m_workspace = workspace;
    m_mcpServerConfigs = McpConfigStore::load(workspace);
    refreshMcpServerTable();
}

void KateAiConfigPage::updateModelCombo(Provider provider)
{
    QComboBox *combo = nullptr;
    switch (provider) {
        case Provider::Grok:
            combo = m_grokModel;
            break;
        case Provider::OpenAI:
            combo = m_openaiModel;
            break;
        case Provider::OpenRouter:
            combo = m_openrouterModel;
            break;
        case Provider::DeepSeek:
            combo = m_deepseekModel;
            break;
        case Provider::OpenAICompatible:
            combo = m_openaiCompatibleModel;
            break;
        case Provider::ClaudeCompatible:
            combo = m_claudeCompatibleModel;
            break;
        case Provider::Acp:
            combo = m_acpModel;
            break;
        default:
            return;
    }
    
    if (!combo) return;
    
    const QString currentText = combo->currentText();
    combo->clear();
    
    // Add models from catalog
    const QStringList models = m_modelCatalog.value(provider);
    if (!models.isEmpty()) {
        combo->addItems(models);
    } else {
        // Fallback to default models if catalog is empty
        combo->addItems(defaultModels(provider));
    }
    
    // Restore current text if it was set
    if (!currentText.isEmpty()) {
        combo->setCurrentText(currentText);
    }
}

void KateAiConfigPage::defaults()
{
    m_plugin->setSettings(Settings{});
    reset();
}

} // namespace KateAi
