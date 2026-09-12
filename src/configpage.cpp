#include "configpage.h"
#include "plugin.h"
#include "settings.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

KateAiConfigPage::KateAiConfigPage(QWidget *parent, KateAiPlugin *plugin)
    : KTextEditor::ConfigPage(parent)
    , m_plugin(plugin)
{
    // Create a form layout for organizing configuration options
    auto *form = new QFormLayout(this);

    // Create the provider selection dropdown with available AI providers
    m_provider = new QComboBox(this);
    m_provider->addItem(providerLabel(Provider::Grok), providerId(Provider::Grok));
    m_provider->addItem(providerLabel(Provider::OpenAI), providerId(Provider::OpenAI));
    m_provider->addItem(providerLabel(Provider::OpenRouter), providerId(Provider::OpenRouter));
    form->addRow(i18n("Default provider:"), m_provider);

    // Helper lambda to create password input fields for API keys
    auto makeKey = [this]() {
        auto *edit = new QLineEdit(this);
        edit->setEchoMode(QLineEdit::Password);
        edit->setClearButtonEnabled(true);
        return edit;
    };

    // Create password fields for each provider's API key
    m_grokKey = makeKey();
    m_openaiKey = makeKey();
    m_openrouterKey = makeKey();
    form->addRow(i18n("Grok (xAI) API key:"), m_grokKey);
    form->addRow(i18n("OpenAI API key:"), m_openaiKey);
    form->addRow(i18n("OpenRouter API key:"), m_openrouterKey);

    // Create input fields for model selection for each provider
    m_grokModel = new QLineEdit(this);
    m_openaiModel = new QLineEdit(this);
    m_openrouterModel = new QLineEdit(this);
    form->addRow(i18n("Grok model:"), m_grokModel);
    form->addRow(i18n("OpenAI model:"), m_openaiModel);
    form->addRow(i18n("OpenRouter model:"), m_openrouterModel);

    // Create permission mode dropdown with different security levels
    m_permission = new QComboBox(this);
    m_permission->addItem(permissionModeLabel(PermissionMode::Ask), permissionModeId(PermissionMode::Ask));
    m_permission->addItem(permissionModeLabel(PermissionMode::AcceptEdits), permissionModeId(PermissionMode::AcceptEdits));
    m_permission->addItem(permissionModeLabel(PermissionMode::AlwaysApprove), permissionModeId(PermissionMode::AlwaysApprove));
    form->addRow(i18n("Permission mode:"), m_permission);

    // Create sandbox profile dropdown with different security levels
    m_sandbox = new QComboBox(this);
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Workspace), sandboxProfileId(SandboxProfile::Workspace));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::ReadOnly), sandboxProfileId(SandboxProfile::ReadOnly));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Strict), sandboxProfileId(SandboxProfile::Strict));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Off), sandboxProfileId(SandboxProfile::Off));
    form->addRow(i18n("Sandbox:"), m_sandbox);

    // Agent budgets are separate: model/API requests are not the same thing as tool work.
    m_maxIter = new QSpinBox(this);
    m_maxIter->setRange(1, 500);
    form->addRow(i18n("Max tool calls:"), m_maxIter);

    m_maxModelRequests = new QSpinBox(this);
    m_maxModelRequests->setRange(1, 500);
    form->addRow(i18n("Max model requests per task:"), m_maxModelRequests);

    m_requestsPerMinute = new QSpinBox(this);
    m_requestsPerMinute->setRange(1, 60);
    form->addRow(i18n("Model requests per minute:"), m_requestsPerMinute);

    // Create spin box for command timeout in seconds
    m_timeout = new QSpinBox(this);
    m_timeout->setRange(1, 600);
    m_timeout->setSuffix(i18n(" s"));
    form->addRow(i18n("Command timeout:"), m_timeout);

    // Create checkbox for plan mode (read-only operations only)
    m_planMode = new QCheckBox(i18n("Only allow read-only tools and ask for an implementation plan"), this);
    form->addRow(i18n("Plan mode:"), m_planMode);

    // Create checkbox for loading project instructions from KATEAI.md
    m_projectInstructions = new QCheckBox(i18n("Load KATEAI.md from the workspace root"), this);
    form->addRow(i18n("Project instructions:"), m_projectInstructions);

    // Create text area for extra system prompt instructions
    m_system = new QPlainTextEdit(this);
    m_system->setPlaceholderText(i18n("Extra system prompt (optional)"));
    m_system->setMaximumHeight(100);
    form->addRow(i18n("Extra instructions:"), m_system);

    // Create text area for additional deny patterns
    m_deny = new QPlainTextEdit(this);
    m_deny->setPlaceholderText(i18n("One glob per line, e.g. **/secrets/**"));
    m_deny->setMaximumHeight(80);
    form->addRow(i18n("Extra deny globs:"), m_deny);

    // Message writing speed
    m_speed = new QComboBox(this);
    m_speed->addItem(i18n("Slow"), 0);
    m_speed->addItem(i18n("Medium"), 1);
    m_speed->addItem(i18n("Fast"), 2);
    form->addRow(i18n("Message speed:"), m_speed);

    // Create separator line for context compression settings
    auto *compressionSeparator = new QLabel(i18n("--- Context Compression ---"), this);
    compressionSeparator->setStyleSheet(u"font-weight: bold; margin-top: 10px;"_s);
    form->addRow(compressionSeparator);

    // Create spin box for context compression level
    m_compressionLevel = new QSpinBox(this);
    m_compressionLevel->setRange(0, 3);
    m_compressionLevel->setSuffix(i18n(" (0=full, 1=summary, 2=minimal, 3=ultra-minimal)"));
    form->addRow(i18n("Context compression level:"), m_compressionLevel);

    // Create spin box for maximum graph nodes
    m_maxGraphNodes = new QSpinBox(this);
    m_maxGraphNodes->setRange(10, 200);
    form->addRow(i18n("Max project graph nodes:"), m_maxGraphNodes);

    // Create spin box for maximum graph edges
    m_maxGraphEdges = new QSpinBox(this);
    m_maxGraphEdges->setRange(10, 500);
    form->addRow(i18n("Max project graph edges:"), m_maxGraphEdges);

    // Create checkbox for compressing project graph
    m_compressGraph = new QCheckBox(i18n("Compress project graph information"), this);
    form->addRow(m_compressGraph);

    // Create checkbox for including file contents
    m_includeFileContents = new QCheckBox(i18n("Include file contents in project graph"), this);
    form->addRow(m_includeFileContents);

    // Create spin box for maximum file content length
    m_maxFileContentLength = new QSpinBox(this);
    m_maxFileContentLength->setRange(100, 5000);
    m_maxFileContentLength->setSuffix(i18n(" chars"));
    form->addRow(i18n("Max file content length:"), m_maxFileContentLength);

    // Create checkbox for compressing editor context
    m_compressEditorContext = new QCheckBox(i18n("Compress editor context"), this);
    form->addRow(m_compressEditorContext);

    // Create spin box for maximum editor context length
    m_maxEditorContextLength = new QSpinBox(this);
    m_maxEditorContextLength->setRange(50, 500);
    m_maxEditorContextLength->setSuffix(i18n(" chars"));
    form->addRow(i18n("Max editor context length:"), m_maxEditorContextLength);

    // Create checkbox for compressing project instructions
    m_compressProjectInstructions = new QCheckBox(i18n("Compress project instructions (KATEAI.md)"), this);
    form->addRow(m_compressProjectInstructions);

    // Create spin box for maximum project instructions length
    m_maxProjectInstructionsLength = new QSpinBox(this);
    m_maxProjectInstructionsLength->setRange(500, 10000);
    m_maxProjectInstructionsLength->setSuffix(i18n(" chars"));
    form->addRow(i18n("Max project instructions length:"), m_maxProjectInstructionsLength);

    // Create checkbox for compressing system prompt
    m_compressSystemPrompt = new QCheckBox(i18n("Compress system prompt"), this);
    form->addRow(m_compressSystemPrompt);

    // Create spin box for maximum system prompt length
    m_maxSystemPromptLength = new QSpinBox(this);
    m_maxSystemPromptLength->setRange(256, 2048);
    m_maxSystemPromptLength->setSuffix(i18n(" chars"));
    form->addRow(i18n("Max system prompt length:"), m_maxSystemPromptLength);

    // Create hint label with information about context compression
    auto *compressionHint = new QLabel(
        i18n("Context compression reduces token usage by limiting the amount of project information sent to the AI. "
             "Higher compression levels provide less context but use fewer tokens. "
             "Enable compression to improve performance with large projects."),
        this);
    compressionHint->setWordWrap(true);
    compressionHint->setStyleSheet(u"font-size: 9pt; color: gray;"_s);
    form->addRow(compressionHint);

    // Create hint label with information about API keys and security
    auto *hint = new QLabel(
        i18n("Keys are stored in your Kate config. Grok uses https://api.x.ai/v1, OpenAI https://api.openai.com/v1, "
             "OpenRouter https://openrouter.ai/api/v1. File tools stay inside the workspace sandbox; writes and shell "
             "commands ask for permission unless you change the mode."),
        this);
    hint->setWordWrap(true);
    form->addRow(hint);

    // Lambda function to emit changed signal when any configuration option is modified
    const auto markChanged = [this]() {
        Q_EMIT changed();
    };
    
    // Connect all configuration controls to the markChanged lambda
    connect(m_provider, &QComboBox::currentIndexChanged, this, markChanged);
    connect(m_grokKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_openrouterKey, &QLineEdit::textChanged, this, markChanged);
    connect(m_grokModel, &QLineEdit::textChanged, this, markChanged);
    connect(m_openaiModel, &QLineEdit::textChanged, this, markChanged);
    connect(m_openrouterModel, &QLineEdit::textChanged, this, markChanged);
    connect(m_permission, &QComboBox::currentIndexChanged, this, markChanged);
    connect(m_sandbox, &QComboBox::currentIndexChanged, this, markChanged);
    connect(m_maxIter, &QSpinBox::valueChanged, this, markChanged);
    connect(m_maxModelRequests, &QSpinBox::valueChanged, this, markChanged);
    connect(m_requestsPerMinute, &QSpinBox::valueChanged, this, markChanged);
    connect(m_timeout, &QSpinBox::valueChanged, this, markChanged);
    connect(m_planMode, &QCheckBox::toggled, this, markChanged);
    connect(m_projectInstructions, &QCheckBox::toggled, this, markChanged);
    connect(m_system, &QPlainTextEdit::textChanged, this, markChanged);
    connect(m_deny, &QPlainTextEdit::textChanged, this, markChanged);
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
    connect(m_speed, &QComboBox::currentIndexChanged, this, markChanged);

    // Load current settings into the configuration form
    reset();
}

QString KateAiConfigPage::name() const
{
    return i18n("Kate AI");
}

QString KateAiConfigPage::fullName() const
{
    return i18n("Kate AI Providers and Safety");
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
    s.grokModel = m_grokModel->text().trimmed();
    s.openaiModel = m_openaiModel->text().trimmed();
    s.openrouterModel = m_openrouterModel->text().trimmed();
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
    
    // Apply context compression settings
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
    m_grokModel->setText(s.grokModel);
    m_openaiModel->setText(s.openaiModel);
    m_openrouterModel->setText(s.openrouterModel);
    m_permission->setCurrentIndex(std::max(0, m_permission->findData(permissionModeId(s.permissionMode))));
    m_sandbox->setCurrentIndex(std::max(0, m_sandbox->findData(sandboxProfileId(s.sandbox))));
    m_maxIter->setValue(s.maxToolCalls);
    m_maxModelRequests->setValue(s.maxModelRequests);
    m_requestsPerMinute->setValue(s.requestsPerMinute);
    m_timeout->setValue(std::max(1, s.bashTimeoutMs / 1000));
    m_planMode->setChecked(s.planMode);
    m_projectInstructions->setChecked(s.loadProjectInstructions);
    m_system->setPlainText(s.extraSystemPrompt);
    m_deny->setPlainText(s.extraDenyGlobs.join(u'\n'));
    
    // Load context compression settings
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
    m_speed->setCurrentIndex(m_speed->findData(s.messageSpeed));
}

void KateAiConfigPage::defaults()
{
    m_plugin->setSettings(Settings{});
    reset();
}

} // namespace KateAi
