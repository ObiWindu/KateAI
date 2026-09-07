#pragma once

#include "documentbridge.h"
#include "llmclient.h"
#include "permissions.h"
#include "sandbox.h"
#include "tools.h"
#include "types.h"

#include <QObject>
#include <memory>

namespace KateAi
{

class AgentLoop : public QObject
{
    Q_OBJECT

public:
    explicit AgentLoop(QObject *parent = nullptr);

    void setSettings(const Settings &settings);
    void setWorkspace(const QString &workspace);
    void setDocumentBridge(DocumentBridge *bridge);
    void setEditorContext(const QString &context);

    bool isBusy() const
    {
        return m_busy;
    }

    void start(const QString &userText);
    void abort();
    void resetConversation();
    void resolvePermission(PermissionDecision decision);
    void fetchModels(Provider provider);

    const QList<ChatMessage> &messages() const
    {
        return m_messages;
    }

    PermissionPolicy &policy()
    {
        return m_policy;
    }

Q_SIGNALS:
    void userMessage(const QString &text);
    void assistantDelta(const QString &delta);
    void assistantFinished(const QString &text);
    void toolStarted(const PermissionRequest &request);
    void toolFinished(const ToolResult &result);
    void permissionNeeded(const PermissionRequest &request);
    void statusChanged(const QString &status);
    void failed(const QString &error);
    void turnFinished();
    void modelsReceived(Provider provider, const QStringList &models);
    void modelsFailed(Provider provider, const QString &error);

private:
    void sendToModel();
    void onFinished(const QString &text, const QList<ToolCall> &toolCalls);
    void onFailed(const QString &error);
    void processQueue();
    void executeOne(const ToolCall &call);
    QString systemPrompt() const;

    Settings m_settings;
    QString m_workspace;
    QString m_editorContext;
    DocumentBridge *m_bridge = nullptr;
    LlmClient m_client;
    PermissionPolicy m_policy;
    std::unique_ptr<Sandbox> m_sandbox;
    std::unique_ptr<ToolRunner> m_tools;
    QList<ChatMessage> m_messages;
    QList<ToolCall> m_queue;
    QList<ToolResult> m_pendingResults;
    ToolCall m_waitingCall;
    PermissionRequest m_waitingRequest;
    bool m_busy = false;
    int m_iterations = 0;
    QString m_currentAssistant;
};

} // namespace KateAi
