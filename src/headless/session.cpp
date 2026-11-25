/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "session.h"

#include "agentloop.h"
#include "documentbridge.h"
#include "sessionstore.h"
#include "settings.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>
#include <QUuid>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

HeadlessSession::HeadlessSession(const HeadlessOptions &options, bool externalPermissions,
                                 QObject *parent)
    : QObject(parent)
    , m_options(options)
    , m_externalPermissions(externalPermissions)
    , m_bridge(std::make_unique<DiskDocumentBridge>())
    , m_loop(std::make_unique<AgentLoop>())
{
    // A headless host has no editor documents. Reading and writing disk
    // directly also keeps worker-thread paths away from KTextEditor.
    m_bridge->setUseOpenDocuments(false);
    m_loop->setDocumentBridge(m_bridge.get());
}

HeadlessSession::~HeadlessSession() = default;

void HeadlessSession::applyProviderOverrides(Settings *settings) const
{
    if (!m_options.provider.isEmpty()) {
        settings->provider = providerFromId(m_options.provider);
    } else if (!m_options.baseUrl.isEmpty()) {
        // A custom base URL without an explicit provider is an
        // OpenAI-compatible endpoint; this is the common local-server case.
        settings->provider = Provider::OpenAICompatible;
    }
    const QString model = m_options.model;
    const QString key = m_options.apiKey;
    const QString url = m_options.baseUrl;
    switch (settings->provider) {
    case Provider::Grok:
        if (!model.isEmpty()) settings->grokModel = model;
        if (!key.isEmpty()) settings->grokApiKey = key;
        break;
    case Provider::OpenAI:
        if (!model.isEmpty()) settings->openaiModel = model;
        if (!key.isEmpty()) settings->openaiApiKey = key;
        break;
    case Provider::OpenRouter:
        if (!model.isEmpty()) settings->openrouterModel = model;
        if (!key.isEmpty()) settings->openrouterApiKey = key;
        break;
    case Provider::DeepSeek:
        if (!model.isEmpty()) settings->deepseekModel = model;
        if (!key.isEmpty()) settings->deepseekApiKey = key;
        if (!url.isEmpty()) settings->deepseekUrl = url;
        break;
    case Provider::ClaudeCompatible:
        if (!model.isEmpty()) settings->claudeCompatibleModel = model;
        if (!key.isEmpty()) settings->claudeCompatibleApiKey = key;
        if (!url.isEmpty()) settings->claudeCompatibleUrl = url;
        settings->apiFormat = ApiFormat::AnthropicCompatible;
        break;
    case Provider::Kilo:
        if (!model.isEmpty()) settings->kiloModel = model;
        if (!key.isEmpty()) settings->kiloApiKey = key;
        break;
    case Provider::OpenCode:
        if (!model.isEmpty()) settings->opencodeModel = model;
        if (!key.isEmpty()) settings->opencodeApiKey = key;
        if (!url.isEmpty()) settings->opencodeUrl = url;
        break;
    case Provider::OpenAICompatible:
        if (!model.isEmpty()) settings->openaiCompatibleModel = model;
        if (!key.isEmpty()) settings->openaiCompatibleApiKey = key;
        if (!url.isEmpty()) settings->openaiCompatibleUrl = url;
        settings->apiFormat = ApiFormat::OpenAICompatible;
        break;
    case Provider::Acp:
        if (!model.isEmpty()) settings->acpModel = model;
        if (!url.isEmpty()) settings->acpUrl = url;
        break;
    case Provider::Telegram:
        break;
    }
    if (settings->provider == Provider::OpenAI || settings->provider == Provider::OpenRouter
        || settings->provider == Provider::Grok || settings->provider == Provider::Kilo
        || settings->provider == Provider::DeepSeek) {
        settings->apiFormat = ApiFormat::OpenAICompatible;
    }
}

bool HeadlessSession::initialize(QString *error)
{
    if (m_ready) {
        return true;
    }
    const QString workspace = m_options.workspace.isEmpty()
        ? QDir::currentPath()
        : QDir(m_options.workspace).absolutePath();
    if (!QFileInfo(workspace).isDir()) {
        if (error) {
            *error = u"Workspace does not exist: %1"_s.arg(workspace);
        }
        return false;
    }

    Settings settings = SettingsStore::load();
    applyProviderOverrides(&settings);
    settings.requireVerification = m_options.verify;
    settings.checkpointsEnabled = false;
    // The deterministic gate replaces the prose-only self-critique nudge in
    // non-interactive runs; a second "check your work" turn just doubles cost.
    settings.selfCritique = false;
    if (m_options.maxTurns > 0) {
        settings.maxModelRequests = m_options.maxTurns;
    }
    if (m_options.maxCostUsd > 0.0) {
        settings.costBudgetUsd = m_options.maxCostUsd;
    }
    if (!m_options.modeId.isEmpty()) {
        settings.agentMode = m_options.modeId;
    }
    if (m_options.permission == u"ask"_s) {
        settings.permissionMode = PermissionMode::Ask;
    } else if (m_options.permission == u"accept-edits"_s) {
        settings.permissionMode = PermissionMode::AcceptEdits;
    } else {
        settings.permissionMode = PermissionMode::AlwaysApprove;
    }
    if (m_options.sandbox == u"off"_s) {
        settings.sandbox = SandboxProfile::Off;
    } else if (m_options.sandbox == u"read-only"_s) {
        settings.sandbox = SandboxProfile::ReadOnly;
    } else if (m_options.sandbox == u"strict"_s) {
        settings.sandbox = SandboxProfile::Strict;
    } else {
        settings.sandbox = SandboxProfile::Workspace;
    }

    m_loop->setSettings(settings);
    m_loop->setWorkspace(workspace);

    QString conversation = m_options.conversationId;
    if (conversation.isEmpty() && m_options.persist) {
        conversation = SessionStore::createNewConversation();
    }
    if (conversation.isEmpty()) {
        conversation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!m_options.conversationId.isEmpty()) {
        const SessionStore::SessionData existing =
            SessionStore::loadConversation(m_options.conversationId);
        if (!existing.loadFailed && !existing.messages.isEmpty()) {
            m_loop->restoreSession(existing);
        }
    }
    m_loop->setConversationId(conversation);

    AgentLoop *loop = m_loop.get();
    connect(loop, &AgentLoop::assistantDelta, this, &HeadlessSession::assistantDelta);
    connect(loop, &AgentLoop::thinkingDelta, this, &HeadlessSession::thinkingDelta);
    connect(loop, &AgentLoop::planUpdated, this, &HeadlessSession::planUpdated);
    connect(loop, &AgentLoop::toolStarted, this, &HeadlessSession::toolStarted);
    connect(loop, &AgentLoop::toolFinished, this, &HeadlessSession::toolFinished);
    connect(loop, &AgentLoop::noticeRaised, this, &HeadlessSession::notice);
    connect(loop, &AgentLoop::harnessEvent, this, &HeadlessSession::harnessEvent);
    if (m_options.maxCostUsd > 0.0) {
        connect(loop, &AgentLoop::harnessEvent, this,
                [this](const QString &type, const QJsonObject &payload) {
                    if (type != EventTypes::Notice
                        || !payload.contains(u"inputTokens"_s)) {
                        return;
                    }
                    if (m_error.isEmpty()
                        && m_loop->sessionCostUsd() > m_options.maxCostUsd) {
                        m_error = u"The per-run cost budget of $%1 was exhausted."_s.arg(
                            QString::number(m_options.maxCostUsd, 'f', 2));
                        m_stopReason = u"budget"_s;
                        m_loop->abort();
                    }
                });
    }

    connect(loop, &AgentLoop::permissionNeeded, this, [this](const PermissionRequest &request) {
        if (m_externalPermissions) {
            Q_EMIT permissionRequested(request);
            return;
        }
        if (m_options.permission == u"ask"_s) {
            m_error = u"A permission decision is required for %1, but the run is non-interactive."_s
                          .arg(request.toolName);
            m_stopReason = u"permission"_s;
            m_loop->abort();
            return;
        }
        if (m_options.permission == u"accept-edits"_s && request.risk == ToolRisk::Execute) {
            // accept-edits covers file mutations, not arbitrary commands. A
            // command that needs approval fails closed in a non-interactive
            // run instead of being silently allowed.
            m_error = u"The command %1 needs an explicit approval, which accept-edits does not grant."_s
                          .arg(request.toolName);
            m_stopReason = u"permission"_s;
            m_loop->abort();
            return;
        }
        // accept-edits, auto and bypass have already passed the hardline and
        // policy layers; a policy ask is the user's chosen non-interactive
        // mode, so grant it for the turn.
        m_loop->resolvePermission(PermissionDecision::AllowSession, request.toolCallId);
    });
    connect(loop, &AgentLoop::questionAsked, this, [this](const PermissionRequest &request) {
        if (m_externalPermissions) {
            Q_EMIT questionRequested(request);
            return;
        }
        if (!m_options.answer.isEmpty() && !m_questionAnswered) {
            m_questionAnswered = true;
            m_loop->answerQuestion(m_options.answer);
            return;
        }
        m_error = u"The agent asked a question, but the run is non-interactive."_s;
        m_stopReason = u"question"_s;
        m_loop->abort();
    });
    connect(loop, &AgentLoop::failed, this, [this](const QString &message) {
        if (m_error.isEmpty()) {
            m_error = message;
        }
        if (m_stopReason == u"end_turn"_s) {
            m_stopReason = u"error"_s;
        }
    });
    connect(loop, &AgentLoop::turnFinished, this, [this] {
        const bool ok = m_error.isEmpty();
        finishOnce(ok, ok ? u"end_turn"_s : m_stopReason,
                   m_error);
    });

    m_ready = true;
    return true;
}

void HeadlessSession::start(const QString &prompt)
{
    if (!m_ready || prompt.isEmpty()) {
        return;
    }
    m_finishedEmitted = false;
    m_error.clear();
    m_stopReason = u"end_turn"_s;
    m_questionAnswered = false;
    m_loop->start(prompt);
}

void HeadlessSession::abort()
{
    m_stopReason = u"cancelled"_s;
    m_loop->abort();
}

void HeadlessSession::answerQuestion(const QString &answer)
{
    m_questionAnswered = true;
    m_loop->answerQuestion(answer);
}

void HeadlessSession::resolvePermission(PermissionDecision decision, const QString &toolCallId)
{
    m_loop->resolvePermission(decision, toolCallId);
}

QString HeadlessSession::conversationId() const
{
    return m_loop->conversationId();
}

bool HeadlessSession::saveConversation(QString *error)
{
    Q_UNUSED(error)
    if (!m_options.persist || !m_ready) {
        return true;
    }
    SessionStore::saveConversation(conversationId(), m_loop->sessionData(),
                                   m_options.prompt.left(80),
                                   SettingsStore::load().maxSavedConversations);
    return true;
}

void HeadlessSession::finishOnce(bool ok, const QString &stopReason, const QString &error)
{
    if (m_finishedEmitted) {
        return;
    }
    m_finishedEmitted = true;
    QString saveError;
    if (!saveConversation(&saveError) && error.isEmpty()) {
        Q_EMIT finished(false, QStringLiteral("error"), saveError);
        return;
    }
    Q_EMIT finished(ok, stopReason, error);
}

QJsonObject HeadlessSession::summary() const
{
    QJsonObject object;
    object.insert(u"conversationId"_s, conversationId());
    object.insert(u"workspace"_s, m_loop->workspace());
    object.insert(u"modelRequests"_s, m_loop->sessionData().modelRequests);
    object.insert(u"toolCalls"_s, m_loop->sessionData().toolCalls);
    object.insert(u"costUsd"_s, m_loop->sessionCostUsd());
    object.insert(u"inputTokens"_s, m_loop->inputTokensTotal());
    object.insert(u"outputTokens"_s, m_loop->outputTokensTotal());
    object.insert(u"cachedInputTokens"_s, m_loop->cachedInputTokensTotal());
    QJsonArray changed;
    for (const QString &path : m_loop->sessionData().changedPaths) {
        changed.append(path);
    }
    object.insert(u"changedPaths"_s, changed);
    object.insert(u"trace"_s, m_loop->traceJson());
    object.insert(u"cost"_s, m_loop->costReportJson());
    return object;
}

} // namespace KateAi
