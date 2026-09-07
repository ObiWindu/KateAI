#include "chatwidget.h"

#include "permissionbar.h"
#include "promptedit.h"
#include "settings.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

ChatWidget::ChatWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto *toolbar = new QHBoxLayout;
    m_provider = new QComboBox(this);
    m_model = new QComboBox(this);
    m_model->setEditable(false);
    m_permission = new QComboBox(this);
    m_sandbox = new QComboBox(this);
    m_mode = new QComboBox(this);
    m_newChat = new QPushButton(i18n("New chat"), this);
    m_configure = new QPushButton(QIcon::fromTheme(u"settings-configure"_s), QString(), this);
    m_configure->setToolTip(i18n("Configure Kate AI"));
    m_configure->setAccessibleName(i18n("Configure Kate AI"));
    m_send = new QPushButton(u"➤"_s, this);
    m_send->setToolTip(i18n("Send message"));
    m_send->setAccessibleName(i18n("Send message"));
    m_send->setFixedSize(38, 38);
    m_send->setStyleSheet(u"QPushButton { color: #1d99f3; font-size: 22px; font-weight: bold; }"_s);
    m_stop = new QPushButton(u"■"_s, this);
    m_stop->setToolTip(i18n("Stop response"));
    m_stop->setAccessibleName(i18n("Stop response"));
    m_stop->setFixedSize(38, 38);
    m_stop->setStyleSheet(u"QPushButton { color: #e74c3c; font-size: 17px; font-weight: bold; }"_s);
    m_stop->setEnabled(false);

    m_permission->addItem(permissionModeLabel(PermissionMode::Ask), permissionModeId(PermissionMode::Ask));
    m_permission->addItem(permissionModeLabel(PermissionMode::AcceptEdits), permissionModeId(PermissionMode::AcceptEdits));
    m_permission->addItem(permissionModeLabel(PermissionMode::AlwaysApprove), permissionModeId(PermissionMode::AlwaysApprove));

    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Workspace), sandboxProfileId(SandboxProfile::Workspace));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::ReadOnly), sandboxProfileId(SandboxProfile::ReadOnly));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Strict), sandboxProfileId(SandboxProfile::Strict));
    m_sandbox->addItem(sandboxProfileLabel(SandboxProfile::Off), sandboxProfileId(SandboxProfile::Off));
    m_mode->addItem(i18n("Agent"), false);
    m_mode->addItem(i18n("Plan"), true);
    m_mode->setToolTip(i18n("Plan mode only gives the AI read-only project tools."));

    toolbar->addWidget(m_provider, 1);
    toolbar->addWidget(m_model, 2);
    toolbar->addWidget(m_permission, 1);
    toolbar->addWidget(m_sandbox, 1);
    toolbar->addWidget(m_mode);
    toolbar->addWidget(m_newChat);
    toolbar->addWidget(m_configure);
    root->addLayout(toolbar);

    m_transcript = new QTextBrowser(this);
    m_transcript->setOpenExternalLinks(true);
    m_transcript->setPlaceholderText(i18n("Kate AI — send a prompt to read and write project files with permission asks."));
    root->addWidget(m_transcript, 1);

    m_permissionBar = new PermissionBar(this);
    root->addWidget(m_permissionBar);

    m_prompt = new PromptEdit(this);
    auto *composer = new QHBoxLayout;
    composer->setContentsMargins(0, 0, 0, 0);
    composer->setSpacing(4);
    composer->addWidget(m_prompt, 1);
    auto *composerActions = new QVBoxLayout;
    composerActions->setContentsMargins(0, 0, 0, 0);
    composerActions->setSpacing(4);
    composerActions->addWidget(m_send);
    composerActions->addWidget(m_stop);
    composerActions->addStretch();
    composer->addLayout(composerActions);
    root->addLayout(composer);

    m_status = new QLabel(i18n("Enter to send · Shift+Enter for a new line"), this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    connect(m_prompt, &PromptEdit::submitRequested, this, &ChatWidget::submit);
    connect(m_send, &QPushButton::clicked, this, &ChatWidget::submit);
    connect(m_newChat, &QPushButton::clicked, this, &ChatWidget::newChat);
    connect(m_configure, &QPushButton::clicked, this, &ChatWidget::configureRequested);
    connect(m_stop, &QPushButton::clicked, this, [this]() {
        m_permissionBar->hideBar();
        m_agent.abort();
    });
    connect(m_permissionBar, &PermissionBar::decided, &m_agent, &AgentLoop::resolvePermission);

    connect(m_provider, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingCombos || m_provider->currentData().isNull()) {
            return;
        }
        m_settings.provider = providerFromId(m_provider->currentData().toString());
        m_preferredProvider = m_settings.provider;
        refreshModels();
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
        case Provider::Grok:
        default:
            m_settings.grokModel = text.trimmed();
            break;
        }
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
    });
    connect(m_permission, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingCombos) {
            return;
        }
        m_settings.permissionMode = permissionModeFromId(m_permission->currentData().toString());
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
    });
    connect(m_sandbox, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingCombos) {
            return;
        }
        m_settings.sandbox = sandboxProfileFromId(m_sandbox->currentData().toString());
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
    });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingCombos) {
            return;
        }
        m_settings.planMode = m_mode->currentData().toBool();
        m_agent.setSettings(m_settings);
        Q_EMIT settingsChanged(m_settings);
    });

    connect(&m_agent, &AgentLoop::userMessage, this, [this](const QString &text) {
        freezeStreaming();
        m_streamText.clear();
        appendHtml(u"<p><b>%1</b></p><pre>%2</pre>"_s.arg(i18n("You"), escape(text)));
    });
    connect(&m_agent, &AgentLoop::assistantDelta, this, [this](const QString &delta) {
        setStreaming(m_streamText + delta);
    });
    connect(&m_agent, &AgentLoop::assistantFinished, this, [this](const QString &text) {
        Q_UNUSED(text);
        freezeStreaming();
    });
    connect(&m_agent, &AgentLoop::toolStarted, this, [this](const PermissionRequest &request) {
        appendHtml(u"<p><i>%1</i></p>"_s.arg(escape(u"→ %1"_s.arg(request.summary))));
    });
    connect(&m_agent, &AgentLoop::toolFinished, this, [this](const ToolResult &result) {
        const QString mark = result.ok ? u"ok"_s : u"failed"_s;
        appendHtml(u"<p><code>%1</code> — %2</p><pre>%3</pre>"_s.arg(escape(result.name), mark, escape(result.output.left(2000))));
    });
    connect(&m_agent, &AgentLoop::permissionNeeded, this, [this](const PermissionRequest &request) {
        m_permissionBar->showRequest(request);
        m_prompt->setEnabled(false);
        m_send->setEnabled(false);
    });
    connect(&m_agent, &AgentLoop::statusChanged, this, [this](const QString &status) {
        m_status->setText(status.isEmpty() ? i18n("Enter to send · Shift+Enter for a new line") : status);
        m_stop->setEnabled(m_agent.isBusy());
        m_prompt->setEnabled(!m_permissionBar->isVisible());
        m_send->setEnabled(!m_agent.isBusy() && !m_permissionBar->isVisible());
    });
    connect(&m_agent, &AgentLoop::failed, this, [this](const QString &error) {
        appendHtml(u"<p style='color:#c0392b'><b>%1</b> %2</p>"_s.arg(i18n("Error:"), escape(error)));
        m_stop->setEnabled(false);
        m_send->setEnabled(true);
        freezeStreaming();
    });
    connect(&m_agent, &AgentLoop::turnFinished, this, [this]() {
        m_stop->setEnabled(false);
        m_prompt->setEnabled(true);
        m_send->setEnabled(true);
        m_prompt->setFocus();
    });
    connect(&m_agent, &AgentLoop::modelsReceived, this, [this](Provider provider, const QStringList &models) {
        m_modelCatalog.insert(provider, models);
        refreshProviders();
    });
    connect(&m_agent, &AgentLoop::modelsFailed, this, [this](Provider provider, const QString &error) {
        m_modelCatalog.remove(provider);
        refreshProviders();
        if (provider == m_settings.provider) {
            m_status->setText(i18n("Model list unavailable: %1", error));
        }
    });
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
    m_updatingCombos = false;
    refreshProviders();
    for (Provider provider : {Provider::Grok, Provider::OpenAI, Provider::OpenRouter}) {
        Settings providerSettings = settings;
        providerSettings.provider = provider;
        if (!apiKeyFor(providerSettings).trimmed().isEmpty()) {
            // LlmClient owns networking; it validates the key by loading its model catalog.
            m_agent.fetchModels(provider);
        }
    }
}

void ChatWidget::applyProviderToCombos()
{
    refreshModels();
}

void ChatWidget::refreshProviders()
{
    const bool wasUpdating = m_updatingCombos;
    m_updatingCombos = true;
    m_provider->clear();
    for (Provider provider : {Provider::Grok, Provider::OpenAI, Provider::OpenRouter}) {
        if (m_modelCatalog.contains(provider)) {
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
    m_agent.setSettings(m_settings);
}

void ChatWidget::refreshModels()
{
    const bool wasUpdating = m_updatingCombos;
    m_updatingCombos = true;
    m_model->clear();
    const QStringList models = m_modelCatalog.value(m_settings.provider);
    m_model->addItems(models);
    m_model->setEnabled(!models.isEmpty());
    const int index = m_model->findText(modelFor(m_settings));
    const int selectedIndex = index >= 0 ? index : (models.isEmpty() ? -1 : 0);
    m_model->setCurrentIndex(selectedIndex);
    if (selectedIndex >= 0 && index < 0) {
        const QString selectedModel = models.at(selectedIndex);
        switch (m_settings.provider) {
        case Provider::OpenAI:
            m_settings.openaiModel = selectedModel;
            break;
        case Provider::OpenRouter:
            m_settings.openrouterModel = selectedModel;
            break;
        case Provider::Grok:
        default:
            m_settings.grokModel = selectedModel;
            break;
        }
    }
    m_updatingCombos = wasUpdating;
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

void ChatWidget::newChat()
{
    m_agent.resetConversation();
    m_historyHtml.clear();
    m_streamText.clear();
    m_transcript->clear();
    m_permissionBar->hideBar();
    m_status->setText(i18n("New chat"));
    m_prompt->setFocus();
}

void ChatWidget::submit()
{
    const QString text = m_prompt->toPlainText().trimmed();
    if (text.isEmpty() || m_agent.isBusy()) {
        return;
    }
    m_prompt->clear();
    m_send->setEnabled(false);
    m_stop->setEnabled(true);
    m_agent.start(text);
}

void ChatWidget::appendHtml(const QString &html)
{
    m_historyHtml += html;
    m_transcript->setHtml(m_historyHtml + (m_streamText.isEmpty() ? QString() : markdownToHtml(m_streamText)));
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
}

void ChatWidget::setStreaming(const QString &text)
{
    m_streamText = text;
    m_transcript->setHtml(m_historyHtml + markdownToHtml(m_streamText));
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
}

void ChatWidget::freezeStreaming()
{
    if (!m_streamText.isEmpty()) {
        m_historyHtml += markdownToHtml(m_streamText);
        m_streamText.clear();
        m_transcript->setHtml(m_historyHtml);
    }
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
