#include "agentloop.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

AgentLoop::AgentLoop(QObject *parent)
    : QObject(parent)
{
    connect(&m_client, &LlmClient::textDelta, this, [this](const QString &delta) {
        m_currentAssistant += delta;
        Q_EMIT assistantDelta(delta);
    });
    connect(&m_client, &LlmClient::finished, this, &AgentLoop::onFinished);
    connect(&m_client, &LlmClient::failed, this, &AgentLoop::onFailed);
}

void AgentLoop::setSettings(const Settings &settings)
{
    m_settings = settings;
    m_client.setSettings(settings);
    m_policy.setMode(settings.permissionMode);
    if (!m_workspace.isEmpty()) {
        m_sandbox = std::make_unique<Sandbox>(m_workspace, settings.sandbox, settings.extraDenyGlobs);
        m_tools = std::make_unique<ToolRunner>(*m_sandbox, m_bridge, this);
        m_tools->setTimeoutMs(settings.bashTimeoutMs);
    }
}

void AgentLoop::setWorkspace(const QString &workspace)
{
    m_workspace = workspace;
    m_sandbox = std::make_unique<Sandbox>(m_workspace, m_settings.sandbox, m_settings.extraDenyGlobs);
    m_tools = std::make_unique<ToolRunner>(*m_sandbox, m_bridge, this);
    m_tools->setTimeoutMs(m_settings.bashTimeoutMs);
}

void AgentLoop::setDocumentBridge(DocumentBridge *bridge)
{
    m_bridge = bridge;
    if (m_sandbox) {
        m_tools = std::make_unique<ToolRunner>(*m_sandbox, m_bridge, this);
        m_tools->setTimeoutMs(m_settings.bashTimeoutMs);
    }
}

void AgentLoop::setEditorContext(const QString &context)
{
    m_editorContext = context;
}

QString AgentLoop::systemPrompt() const
{
    QString prompt = defaultSystemPrompt(m_workspace);
    if (!m_settings.extraSystemPrompt.trimmed().isEmpty()) {
        prompt += u"\n\n"_s + m_settings.extraSystemPrompt.trimmed();
    }
    if (!m_editorContext.isEmpty()) {
        prompt += u"\n\n<editor_context>\n"_s + m_editorContext + u"\n</editor_context>\n"_s;
    }
    prompt += u"\nSandbox profile: "_s + sandboxProfileId(m_settings.sandbox);
    prompt += u"\nPermission mode: "_s + permissionModeId(m_settings.permissionMode);
    return prompt;
}

void AgentLoop::resetConversation()
{
    abort();
    m_messages.clear();
    m_policy.revokeSession();
    m_iterations = 0;
}

void AgentLoop::abort()
{
    const bool hadActiveTurn = m_busy || m_client.isBusy() || !m_queue.isEmpty() || !m_pendingResults.isEmpty()
        || !m_waitingCall.name.isEmpty();
    m_client.abort();
    m_queue.clear();
    m_pendingResults.clear();
    m_busy = false;
    m_waitingCall = {};
    m_waitingRequest = {};
    if (hadActiveTurn) {
        Q_EMIT statusChanged(u"Stopped"_s);
        Q_EMIT turnFinished();
    }
}

void AgentLoop::start(const QString &userText)
{
    if (m_busy) {
        return;
    }
    if (m_workspace.isEmpty()) {
        Q_EMIT failed(u"No workspace is open."_s);
        return;
    }
    if (!m_tools) {
        setWorkspace(m_workspace);
    }

    m_busy = true;
    m_iterations = 0;
    m_queue.clear();
    m_pendingResults.clear();
    m_currentAssistant.clear();

    if (m_messages.isEmpty()) {
        ChatMessage system;
        system.role = ChatMessage::Role::System;
        system.content = systemPrompt();
        m_messages.append(system);
    } else if (m_messages.first().role == ChatMessage::Role::System) {
        // Workspace, selection, and safety settings can change between turns.
        // Keep the one system message current without discarding the chat history.
        m_messages.first().content = systemPrompt();
    }

    ChatMessage user;
    user.role = ChatMessage::Role::User;
    user.content = userText;
    m_messages.append(user);
    Q_EMIT userMessage(userText);
    sendToModel();
}

void AgentLoop::sendToModel()
{
    ++m_iterations;
    if (m_iterations > m_settings.maxIterations) {
        m_busy = false;
        Q_EMIT failed(u"Stopped after %1 tool iterations."_s.arg(m_settings.maxIterations));
        Q_EMIT turnFinished();
        return;
    }
    m_currentAssistant.clear();
    Q_EMIT statusChanged(u"Thinking…"_s);
    m_client.complete(m_messages);
}

void AgentLoop::onFailed(const QString &error)
{
    m_busy = false;
    Q_EMIT failed(error);
    Q_EMIT turnFinished();
}

void AgentLoop::onFinished(const QString &text, const QList<ToolCall> &toolCalls)
{
    ChatMessage assistant;
    assistant.role = ChatMessage::Role::Assistant;
    assistant.content = text;
    if (!toolCalls.isEmpty()) {
        QJsonArray encoded;
        for (const ToolCall &call : toolCalls) {
            QJsonObject fn;
            fn.insert(u"name"_s, call.name);
            fn.insert(u"arguments"_s, call.argumentsJson.isEmpty() ? QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact))
                                                                   : call.argumentsJson);
            QJsonObject obj;
            obj.insert(u"id"_s, call.id);
            obj.insert(u"type"_s, u"function"_s);
            obj.insert(u"function"_s, fn);
            encoded.append(obj);
        }
        assistant.toolCalls = encoded;
    }
    m_messages.append(assistant);
    Q_EMIT assistantFinished(text);

    if (toolCalls.isEmpty()) {
        m_busy = false;
        Q_EMIT statusChanged(QString());
        Q_EMIT turnFinished();
        return;
    }

    m_queue = toolCalls;
    processQueue();
}

void AgentLoop::processQueue()
{
    if (m_queue.isEmpty()) {
        for (const ToolResult &result : m_pendingResults) {
            ChatMessage toolMsg;
            toolMsg.role = ChatMessage::Role::Tool;
            toolMsg.toolCallId = result.toolCallId;
            toolMsg.name = result.name;
            toolMsg.content = result.output;
            m_messages.append(toolMsg);
        }
        m_pendingResults.clear();
        sendToModel();
        return;
    }

    const ToolCall call = m_queue.takeFirst();
    executeOne(call);
}

void AgentLoop::executeOne(const ToolCall &call)
{
    if (!m_sandbox || !m_tools) {
        ToolResult result;
        result.toolCallId = call.id;
        result.name = call.name;
        result.ok = false;
        result.output = u"Sandbox is not initialized."_s;
        m_pendingResults.append(result);
        Q_EMIT toolFinished(result);
        processQueue();
        return;
    }

    const PermissionRequest request = m_tools->describe(call);
    QString reason;
    const auto verdict = m_policy.evaluate(call.name, call.arguments, *m_sandbox, &reason);
    if (verdict == PermissionPolicy::Verdict::Deny) {
        ToolResult result;
        result.toolCallId = call.id;
        result.name = call.name;
        result.ok = false;
        result.output = reason;
        m_pendingResults.append(result);
        Q_EMIT toolFinished(result);
        processQueue();
        return;
    }
    if (verdict == PermissionPolicy::Verdict::Ask) {
        m_waitingCall = call;
        m_waitingRequest = request;
        Q_EMIT statusChanged(u"Waiting for permission…"_s);
        Q_EMIT permissionNeeded(request);
        return;
    }

    Q_EMIT toolStarted(request);
    Q_EMIT statusChanged(u"Running %1…"_s.arg(call.name));
    ToolResult result = m_tools->run(call);
    result.toolCallId = call.id;
    m_pendingResults.append(result);
    Q_EMIT toolFinished(result);
    processQueue();
}

void AgentLoop::resolvePermission(PermissionDecision decision)
{
    if (m_waitingCall.name.isEmpty()) {
        return;
    }
    const ToolCall call = m_waitingCall;
    m_waitingCall = {};
    const PermissionRequest request = m_waitingRequest;
    m_waitingRequest = {};

    if (decision == PermissionDecision::Deny) {
        ToolResult result;
        result.toolCallId = call.id;
        result.name = call.name;
        result.ok = false;
        result.output = u"User denied this tool call."_s;
        m_pendingResults.append(result);
        Q_EMIT toolFinished(result);
        processQueue();
        return;
    }
    if (decision == PermissionDecision::AllowSession) {
        m_policy.grantSession(call.name);
    }

    Q_EMIT toolStarted(request);
    Q_EMIT statusChanged(u"Running %1…"_s.arg(call.name));
    ToolResult result = m_tools->run(call);
    result.toolCallId = call.id;
    m_pendingResults.append(result);
    Q_EMIT toolFinished(result);
    processQueue();
}

} // namespace KateAi
