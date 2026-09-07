#include "chatwidget.h"

#include "permissionbar.h"
#include "promptedit.h"
#include "settings.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QAbstractItemView>
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
    // Create the main vertical layout for the chat widget
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // Create the toolbar layout for provider and model selection
    auto *toolbar = new QHBoxLayout;
    m_provider = new QComboBox(this);
    m_model = new QComboBox(this);
    m_model->setEditable(true);
    m_model->lineEdit()->setPlaceholderText(i18n("Filter models..."));
    m_model->view()->setMinimumWidth(420);
    m_model->setInsertPolicy(QComboBox::NoInsert);
    
    // Connect model filter changes to refresh the model list
    connect(m_model->lineEdit(), &QLineEdit::textChanged, this, [this](const QString &filter) {
        if (m_updatingCombos) {
            return;
        }
        m_modelFilter = filter.trimmed();
        refreshModels();
    });
    m_permission = new QComboBox(this);
    m_sandbox = new QComboBox(this);
    m_mode = new QComboBox(this);
    m_newChat = new QPushButton(i18n("New chat"), this);
    m_configure = new QPushButton(QIcon::fromTheme(u"settings-configure"_s), QString(), this);
    m_configure->setToolTip(i18n("Configure Kate AI"));
    m_configure->setAccessibleName(i18n("Configure Kate AI"));
    m_send = new QPushButton(this);
    m_send->setToolTip(i18n("Send message"));
    m_send->setAccessibleName(i18n("Send message"));
    m_send->setFixedSize(38, 38);
    m_send->setStyleSheet(u"QPushButton { font-size: 22px; font-weight: bold; border-radius: 19px; }"_s);
    updateSendButtonState();

    m_stop = new QPushButton(this);
    m_stop->setVisible(false);

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
    m_transcript->setStyleSheet(u"QTextBrowser { background-color: #12141a; color: #e8e8e8; border: none; padding: 8px; }"_s);
    root->addWidget(m_transcript, 1);

    m_permissionBar = new PermissionBar(this);
    root->addWidget(m_permissionBar);

    m_prompt = new PromptEdit(this);
    auto *composer = new QHBoxLayout;
    composer->setContentsMargins(0, 0, 0, 0);
    composer->setSpacing(4);
    composer->addWidget(m_prompt, 1);
    auto *composerActions = new QHBoxLayout;
    composerActions->setContentsMargins(0, 0, 0, 0);
    composerActions->setSpacing(4);
    composerActions->addStretch();
    composerActions->addWidget(m_send);
    composerActions->addStretch();
    composer->addLayout(composerActions);
    root->addLayout(composer);

    m_status = new QLabel(i18n("Enter to send · Shift+Enter for a new line"), this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    connect(m_prompt, &PromptEdit::submitRequested, this, &ChatWidget::submit);
    connect(m_send, &QPushButton::clicked, this, [this]() {
        if (m_agent.isBusy()) {
            m_permissionBar->hideBar();
            m_agent.abort();
        } else {
            submit();
        }
    });
    connect(m_newChat, &QPushButton::clicked, this, &ChatWidget::newChat);
    connect(m_configure, &QPushButton::clicked, this, &ChatWidget::configureRequested);
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
        appendHtml(u"<div style=\"background-color:#2a2f3a; color:#e8e8e8; padding:12px 16px; border-radius:8px; margin:8px 0 8px 0; max-width:85%; text-align:left; font-family:sans-serif; font-size:13px; line-height:1.5; border:2px solid #3a3f4a; box-shadow:0 2px 8px rgba(0,0,0,0.3);\"><b style=\"color:#7aa2f7; font-size:11px; text-transform:uppercase; letter-spacing:0.5px;\">You</b><br>%1</div>"_s.arg(escape(text).replace(u"\n"_s, u"<br>"_s)));
    });
    connect(&m_agent, &AgentLoop::assistantDelta, this, [this](const QString &delta) {
        setStreaming(m_streamText + delta);
    });
    connect(&m_agent, &AgentLoop::assistantFinished, this, [this](const QString &text) {
        Q_UNUSED(text);
        freezeStreaming();
    });
    connect(&m_agent, &AgentLoop::toolStarted, this, [this](const PermissionRequest &request) {
        appendHtml(u"<div style=\"color:#7aa2f7; font-size:12px; padding:4px 0 4px 8px; margin:4px 0; border-left:2px solid #7aa2f7; font-family:sans-serif;\">⚙ %1</div>"_s.arg(escape(u"→ %1"_s.arg(request.summary))));
    });
    connect(&m_agent, &AgentLoop::toolFinished, this, [this](const ToolResult &result) {
        const QString mark = result.ok ? u"<span style='color:#9ece6a;'>✓</span>"_s : u"<span style='color:#f7768e;'>✗</span>"_s;
        appendHtml(u"<div style=\"color:#7a7a8a; font-size:12px; padding:4px 0 4px 8px; margin:4px 0; border-left:2px solid #3a3a4a; font-family:sans-serif;\"><code style='color:#bb9af7;'>%1</code> %2 <span style='font-size:10px;'>%3</span></div>"_s.arg(escape(result.name), mark, escape(result.output.left(300).replace(u"\n"_s, u" · "_s))));
    });
    connect(&m_agent, &AgentLoop::permissionNeeded, this, [this](const PermissionRequest &request) {
        m_permissionBar->showRequest(request);
        m_prompt->setEnabled(false);
        m_send->setEnabled(false);
    });
    connect(m_permissionBar, &PermissionBar::decided, &m_agent, &AgentLoop::resolvePermission);

    connect(&m_agent, &AgentLoop::statusChanged, this, [this](const QString &status) {
        m_status->setText(status.isEmpty() ? i18n("Enter to send · Shift+Enter for a new line") : status);
        m_stop->setEnabled(m_agent.isBusy());
        m_prompt->setEnabled(!m_permissionBar->isVisible());
        m_send->setEnabled(!m_agent.isBusy() && !m_permissionBar->isVisible());
        updateSendButtonState();
    });
    connect(&m_agent, &AgentLoop::failed, this, [this](const QString &error) {
        appendHtml(u"<p style='color:#c0392b'><b>Error:</b> %1</p>"_s.arg(escape(error)));
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
    const QStringList allModels = m_modelCatalog.value(m_settings.provider);
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
    if (m_modelFilter.isEmpty()) {
        m_model->lineEdit()->setText(QString());
    }
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

void ChatWidget::updateSendButtonState()
{
    if (m_agent.isBusy()) {
        m_send->setText(u"■"_s);
        m_send->setStyleSheet(u"QPushButton { color: #e74c3c; font-size: 17px; font-weight: bold; border-radius: 19px; }"_s);
        m_send->setToolTip(i18n("Stop response"));
    } else {
        m_send->setText(u"➤"_s);
        m_send->setStyleSheet(u"QPushButton { color: #1d99f3; font-size: 22px; font-weight: bold; border-radius: 19px; }"_s);
        m_send->setToolTip(i18n("Send message"));
    }
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
    m_transcript->setHtml(m_historyHtml + QStringLiteral("<div style=\"background-color:#1e222d; color:#e8e8e8; padding:12px 16px; border-radius:8px; margin:8px 0; border:1px solid #3a3f4a; font-family:sans-serif; font-size:13px; line-height:1.6;\"><div style=\"white-space:pre-wrap;\">") + markdownToHtml(m_streamText) + QStringLiteral("</div></div>"));
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
}

void ChatWidget::freezeStreaming()
{
    if (!m_streamText.isEmpty()) {
        m_historyHtml += QStringLiteral("<div style=\"background-color:#1e222d; color:#e8e8e8; padding:12px 16px; border-radius:8px; margin:8px 0; border:1px solid #3a3f4a; font-family:sans-serif; font-size:13px; line-height:1.6;\"><div style=\"white-space:pre-wrap;\">") + markdownToHtml(m_streamText) + QStringLiteral("</div></div>");
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
