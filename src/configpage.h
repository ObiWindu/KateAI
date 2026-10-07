/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"
#include "mcp.h"
#include "agentteam.h"

#include <QHash>
#include <QList>
#include <QWidget>
#include "llmclient.h"

#include <KTextEditor/ConfigPage>

class QComboBox;
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QTableWidget;
class QTreeWidget;

namespace KateAi
{

class KateAiPlugin;

class KateAiConfigPage : public KTextEditor::ConfigPage
{
    Q_OBJECT

public:
    KateAiConfigPage(QWidget *parent, KateAiPlugin *plugin);

    ~KateAiConfigPage() override;

    QString name() const override;
    QString fullName() const override;
    QIcon icon() const override;

    void apply() override;
    void reset() override;
    void defaults() override;

    // The MCP server list lives in the workspace, so the view tells the page
    // which workspace is open before the dialog is shown.
    void setWorkspace(const QString &workspace);

private:
    KateAiPlugin *m_plugin = nullptr;

    // Providers
    QComboBox *m_provider = nullptr;
    QLineEdit *m_grokKey = nullptr;
    QLineEdit *m_openaiKey = nullptr;
    QLineEdit *m_openrouterKey = nullptr;
    QLineEdit *m_deepseekKey = nullptr;
    QLineEdit *m_openaiCompatibleKey = nullptr;
    QLineEdit *m_claudeCompatibleKey = nullptr;
    QLineEdit *m_opencodeKey = nullptr;
    QLineEdit *m_acpKey = nullptr;
    QComboBox *m_grokModel = nullptr;
    QComboBox *m_openaiModel = nullptr;
    QComboBox *m_openrouterModel = nullptr;
    QComboBox *m_deepseekModel = nullptr;
    QComboBox *m_openaiCompatibleModel = nullptr;
    QComboBox *m_claudeCompatibleModel = nullptr;
    QComboBox *m_opencodeModel = nullptr;
    QComboBox *m_acpModel = nullptr;
    QLineEdit *m_deepseekUrl = nullptr;
    QLineEdit *m_openaiCompatibleUrl = nullptr;
    QLineEdit *m_claudeCompatibleUrl = nullptr;
    QLineEdit *m_opencodeUrl = nullptr;
    QLineEdit *m_acpUrl = nullptr;
    QLineEdit *m_acpCommand = nullptr;
    QLineEdit *m_acpArgs = nullptr;
    QComboBox *m_apiFormat = nullptr;

    // Model fetching
    LlmClient *m_modelFetcher = nullptr;
    QHash<Provider, QStringList> m_modelCatalog;

    void updateModelCombo(Provider provider);

    // Security
    QComboBox *m_permission = nullptr;
    QComboBox *m_sandbox = nullptr;
    QSpinBox *m_timeout = nullptr;
    QSpinBox *m_maxExpandedToolCards = nullptr;
    QPlainTextEdit *m_deny = nullptr;

    // Agent
    QSpinBox *m_maxIter = nullptr;
    QSpinBox *m_maxModelRequests = nullptr;
    QSpinBox *m_requestsPerMinute = nullptr;
    QSpinBox *m_maxSavedConversations = nullptr;
    QCheckBox *m_planMode = nullptr;
    QCheckBox *m_projectInstructions = nullptr;
    QPlainTextEdit *m_system = nullptr;
    QComboBox *m_speed = nullptr;
    QCheckBox *m_thinkingMode = nullptr;

    // Compression
    QSpinBox *m_compressionLevel = nullptr;
    QSpinBox *m_maxGraphNodes = nullptr;
    QSpinBox *m_maxGraphEdges = nullptr;
    QCheckBox *m_compressGraph = nullptr;
    QCheckBox *m_includeFileContents = nullptr;
    QSpinBox *m_maxFileContentLength = nullptr;
    QCheckBox *m_compressEditorContext = nullptr;
    QSpinBox *m_maxEditorContextLength = nullptr;
    QCheckBox *m_compressProjectInstructions = nullptr;
    QSpinBox *m_maxProjectInstructionsLength = nullptr;
    QCheckBox *m_compressSystemPrompt = nullptr;
    QSpinBox *m_maxSystemPromptLength = nullptr;

    // Optimal Intelligence Parameters
    QDoubleSpinBox *m_temperature = nullptr;
    QDoubleSpinBox *m_topP = nullptr;
    QSpinBox *m_maxTokens = nullptr;
    QComboBox *m_reasoningEffort = nullptr;
    QCheckBox *m_selfCritique = nullptr;
    QCheckBox *m_parallelToolCalls = nullptr;
    QComboBox *m_verbosity = nullptr;

    // Enhanced Intelligence Parameters
    QCheckBox *m_structuredThinking = nullptr;
    QCheckBox *m_structuredPlanning = nullptr;
    QCheckBox *m_autoCollapseThinking = nullptr;
    QCheckBox *m_showPlanAsChecklist = nullptr;
    QSpinBox *m_maxThinkingTokens = nullptr;
    QSpinBox *m_maxPlanSteps = nullptr;
    QCheckBox *m_requireVerification = nullptr;
    QSpinBox *m_maxVerificationAttempts = nullptr;
    QCheckBox *m_adaptiveTemperature = nullptr;
    QDoubleSpinBox *m_explorationTemperature = nullptr;
    QDoubleSpinBox *m_exploitationTemperature = nullptr;
    QCheckBox *m_enablePlanUpdates = nullptr;
    QCheckBox *m_narrativeProgress = nullptr;

    // Context Management
    QCheckBox *m_smartContextTruncation = nullptr;
    QSpinBox *m_contextWindow = nullptr;
    QSpinBox *m_keepRecentTokens = nullptr;
    QSpinBox *m_contextWindowReserve = nullptr;
    QCheckBox *m_compressOldMessages = nullptr;
    QSpinBox *m_compressionThreshold = nullptr;

    // Modes & Tools
    QComboBox *m_agentMode = nullptr;
    QWidget *m_autoApproveTools = nullptr;
    QHash<QString, QCheckBox *> m_autoApproveBoxes;
    QCheckBox *m_loadAgentRules = nullptr;
    QPlainTextEdit *m_globalRules = nullptr;

    // MCP Servers
    QCheckBox *m_mcpEnabled = nullptr;
    QCheckBox *m_mcpAutoConnect = nullptr;
    QSpinBox *m_mcpTimeout = nullptr;
    QTableWidget *m_mcpServers = nullptr;
    QList<McpServerConfig> m_mcpServerConfigs;

    // Web search
    QComboBox *m_webProvider = nullptr;
    QLineEdit *m_webApiKey = nullptr;
    QLineEdit *m_webEndpoint = nullptr;
    QSpinBox *m_webMaxResults = nullptr;
    QSpinBox *m_webTimeout = nullptr;
    QString m_workspace;

    // Checkpoints & Subtasks
    QCheckBox *m_checkpointsEnabled = nullptr;
    QSpinBox *m_checkpointRetention = nullptr;
    QSpinBox *m_maxSubtaskDepth = nullptr;
    QSpinBox *m_maxParallelSubtasks = nullptr;
    QSpinBox *m_subtaskTimeout = nullptr;

    // Agent Team
    QTreeWidget *m_builtinRoster = nullptr;
    QTableWidget *m_customAgents = nullptr;
    QList<AgentProfile> m_customAgentList;
    void refreshCustomAgentTable();
    void addCustomAgent();
    void editCustomAgent();
    void removeCustomAgent();

    void editMcpServer();
    void addMcpServer();
    void removeMcpServer();
    void refreshMcpServerTable();
};

} // namespace KateAi
