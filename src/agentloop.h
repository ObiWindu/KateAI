#pragma once

#include "documentbridge.h"
#include "llmclient.h"
#include "permissions.h"
#include "sandbox.h"
#include "tools.h"
#include "types.h"
#include "graph.h"

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
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

    void updateProjectGraph(const QString &filePath, const QString &content);
    ProjectGraph *getProjectGraph() const { return m_projectGraph.get(); }
    QList<GraphNode *> getProjectNodes() const;
    QList<GraphEdge *> getProjectEdges() const;

    bool isBusy() const { return m_busy; }

    void start(const QString &userText);
    void abort();
    void resetConversation();
    void resolvePermission(PermissionDecision decision);
    void fetchModels(Provider provider);

    const QList<ChatMessage> &messages() const { return m_messages; }

    PermissionPolicy &policy() { return m_policy; }

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
    enum class State {
        Idle,
        WaitingForNextModel,
        WaitingForModel,
        ExecutingTools,
        WaitingForPermission,
    };

    void scheduleNextModelStep();
    void sendToModel();
    void finishTurn();
    void finishWithFailure(const QString &error);
    bool canStartModelRequest(QString *error = nullptr) const;
    QString actionSignature(const ToolCall &call) const;
    QString formatToolResult(const ToolCall &call, const ToolResult &result) const;
    bool isMutationTool(const QString &toolName) const;
    bool isVerificationTool(const QString &toolName) const;
    bool isVerificationForChangedFiles(const ToolCall &call) const;
    void appendToolResult(const ToolCall &call, ToolResult result);
    void appendToolResultsToConversation();
    void addBudgetFailureResults(const QString &reason);
    void requestVerificationTurn();
    void onFinished(const QString &text, const QList<ToolCall> &toolCalls);
    void onFailed(const QString &error);
    void processQueue();
    void executeOne(const ToolCall &call);
    QString systemPrompt() const;
    QList<ToolCall> bundleSimilarTools(const QList<ToolCall> &calls);

    Settings m_settings;
    QString m_workspace;
    QString m_editorContext;
    DocumentBridge *m_bridge = nullptr;
    LlmClient m_client;
    PermissionPolicy m_policy;
    std::unique_ptr<Sandbox> m_sandbox;
    std::unique_ptr<ToolRunner> m_tools;
    std::unique_ptr<ProjectGraph> m_projectGraph;

    QList<ChatMessage> m_messages;
    QList<ToolCall> m_queue;
    QList<ToolResult> m_pendingResults;
    ToolCall m_waitingCall;
    PermissionRequest m_waitingRequest;

    bool m_busy = false;
    State m_state = State::Idle;
    int m_modelRequests = 0;
    int m_toolCalls = 0;
    QQueue<qint64> m_modelRequestTimes;
    QTimer m_nextModelTimer;

    quint64 m_stateEpoch = 0;
    QSet<QString> m_actionSignatures;
    QSet<QString> m_changedPaths;
    bool m_changesNeedVerification = false;
    bool m_verificationAttempted = false;
    int m_verificationPromptCount = 0;

    QString m_currentAssistant;
};

} // namespace KateAi
