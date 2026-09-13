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
    securityForm->addRow(i18n("Extra deny globs:"), m_deny);

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
    s.grokModel = m_grokModel->text().trimmed();
    s.openaiModel = m_openaiModel->text().trimmed();
    s.openrouterModel = m_openrouterModel->text().trimmed();
    s.openaiCompatibleModel = m_openaiCompatibleModel->text().trimmed();
    s.claudeCompatibleModel = m_claudeCompatibleModel->text().trimmed();
    s.openaiCompatibleUrl = m_openaiCompatibleUrl->text().trimmed();
    s.claudeCompatibleUrl = m_claudeCompatibleUrl->text().trimmed();

    s.permissionMode = permissionModeFromId(m_permission->currentData().toString());
    s.sandbox = sandboxProfileFromId(m_sandbox->currentData().toString());
    s.maxToolCalls = m_maxIter->value();
    s.maxIterations = s.maxToolCalls;
    s.maxModelRequests = m_maxModelRequests->value();
    s.requestsPerMinute = m_requestsPerMinute->value();
    s.bashTimeoutMs = m_timeout->value() * 1000;
    s.planMode = m_planMode->isChecked();
    s.loadProjectInstructions = m_projectInstructions->isChecked();
    s.extraSystemPrompt = m_system->toPlainText();
    s.extraDenyGlobs = m_deny->toPlainText().split(u'\n', Qt::SkipEmptyParts);

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
    m_grokModel->setText(s.grokModel);
    m_openaiModel->setText(s.openaiModel);
    m_openrouterModel->setText(s.openrouterModel);
    m_openaiCompatibleModel->setText(s.openaiCompatibleModel);
    m_claudeCompatibleModel->setText(s.claudeCompatibleModel);
    m_openaiCompatibleUrl->setText(s.openaiCompatibleUrl);
    m_claudeCompatibleUrl->setText(s.claudeCompatibleUrl);

    m_permission->setCurrentIndex(std::max(0, m_permission->findData(permissionModeId(s.permissionMode))));
    m_sandbox->setCurrentIndex(std::max(0, m_sandbox->findData(sandboxProfileId(s.sandbox))));
    m_timeout->setValue(std::max(1, s.bashTimeoutMs / 1000));
    m_deny->setPlainText(s.extraDenyGlobs.join(u'\n'));

    m_maxIter->setValue(s.maxToolCalls);
    m_maxModelRequests->setValue(s.maxModelRequests);
    m_requestsPerMinute->setValue(s.requestsPerMinute);
    m_planMode->setChecked(s.planMode);
    m_projectInstructions->setChecked(s.loadProjectInstructions);
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
}

void KateAiConfigPage::defaults()
{
    m_plugin->setSettings(Settings{});
    reset();
}

} // namespace KateAi
