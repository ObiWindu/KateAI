/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "agentloop.h"
#include "contextmanager.h"
#include "graph/projectgraph.h"
#include "rules.h"

#include <KLocalizedString>
#include "sessionstore.h"
#include "types.h"
#include "websearch.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <utility>
#include <algorithm>
#include <QMap>
#include <QQueue>
#include <limits>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

AgentLoop::AgentLoop(QObject *parent)
    : QObject(parent)
{
    // Initialize the project graph for understanding and tracking the project
    m_projectGraph = std::make_unique<ProjectGraph>();

    m_mcp = new McpManager(this);
        m_locks = new WorkspaceLocks(this);

    m_nextModelTimer.setSingleShot(true);
    connect(&m_nextModelTimer, &QTimer::timeout, this, &AgentLoop::sendToModel);

    connect(&m_client, &LlmClient::textDelta, this, [this](const QString &delta) {
        if (!m_thinkingFinishedEmitted && !m_currentThinking.isEmpty()) {
            m_thinkingFinishedEmitted = true;
            Q_EMIT thinkingFinished(m_currentThinking);
        }
        m_currentAssistant += delta;
        Q_EMIT assistantDelta(delta);
    });
    connect(&m_client, &LlmClient::thinkingDelta, this, [this](const QString &delta) {
        m_currentThinking += delta;
        Q_EMIT thinkingDelta(delta);
    });
    connect(&m_client, &LlmClient::finished, this, &AgentLoop::onFinished);
    connect(&m_client, &LlmClient::failed, this, &AgentLoop::onFailed);
    connect(&m_client, &LlmClient::modelsReceived, this, &AgentLoop::modelsReceived);
    connect(&m_client, &LlmClient::modelsFailed, this, &AgentLoop::modelsFailed);

    connect(&m_acp, &AcpClient::textDelta, this, [this](const QString &delta) {
        if (!m_thinkingFinishedEmitted && !m_currentThinking.isEmpty()) {
            m_thinkingFinishedEmitted = true;
            Q_EMIT thinkingFinished(m_currentThinking);
        }
        m_currentAssistant += delta;
        Q_EMIT assistantDelta(delta);
    });
    connect(&m_acp, &AcpClient::thinkingDelta, this, [this](const QString &delta) {
        m_currentThinking += delta;
        Q_EMIT thinkingDelta(delta);
    });
    connect(&m_acp, &AcpClient::planUpdated, this, [this](const QJsonArray &plan) {
        m_currentPlan = plan;
        m_planShown = true;
        Q_EMIT planUpdated(plan);
    });
    connect(&m_acp, &AcpClient::toolStarted, this, [this](const PermissionRequest &request) {
        ++m_toolCalls;
        Q_EMIT toolStarted(request);
        Q_EMIT statusChanged(u"Working on the next step…"_s);
    });
    connect(&m_acp, &AcpClient::toolFinished, this, [this](const ToolResult &result) {
        ChatMessage toolMsg;
        toolMsg.role = ChatMessage::Role::Tool;
        toolMsg.toolCallId = result.toolCallId;
        toolMsg.name = result.name;
        toolMsg.content = result.output;
        m_messages.append(toolMsg);
        Q_EMIT toolFinished(result);
    });
    connect(&m_acp, &AcpClient::permissionNeeded, this, [this](const PermissionRequest &request) {
        m_waitingCall.id = request.toolCallId;
        m_waitingCall.name = request.toolName;
        m_waitingRequest = request;
        m_state = State::WaitingForPermission;
        Q_EMIT permissionNeeded(request);
    });
    connect(&m_acp, &AcpClient::promptFinished, this, &AgentLoop::onAcpPromptFinished);
    connect(&m_acp, &AcpClient::failed, this, &AgentLoop::onFailed);
    connect(&m_acp, &AcpClient::logMessage, this, &AgentLoop::mcpLogMessage);
    connect(&m_acp, &AcpClient::sessionIdChanged, this, [this](const QString &) {
        Q_EMIT statusChanged(m_acp.statusText());
    });
    // Retry status signals
    connect(&m_client, &LlmClient::retryStatus, this, [this](const QString &message, int attempt, int maxAttempts, int delaySeconds) {
        Q_EMIT statusChanged(message);
        Q_EMIT activityUpdated(u"Retrying in %1s (attempt %2/%3)..."_s.arg(delaySeconds).arg(attempt).arg(maxAttempts));
    });
    connect(&m_client, &LlmClient::retryScheduled, this, [this](int attempt, int maxAttempts, int delaySeconds) {
        Q_UNUSED(attempt);
        Q_UNUSED(maxAttempts);
        Q_UNUSED(delaySeconds);
        // Could add additional UI feedback here if needed
    });

    // --- Modes ----------------------------------------------------------------
    connect(&m_modes, &ModeRegistry::modesChanged, this, [this] {
        syncToolAccess();
        Q_EMIT modesChanged();
    });

    // --- MCP ------------------------------------------------------------------
    connect(m_mcp, &McpManager::toolsChanged, this, [this] {
        // Newly discovered MCP tools change what may be auto-approved.
        syncPermissionPolicy();
        syncToolAccess();
        Q_EMIT mcpToolsChanged();
        Q_EMIT mcpStatusChanged(m_mcp->statusSummary());
    });
    connect(m_mcp, &McpManager::serversChanged, this, [this] {
        syncToolAccess();
        Q_EMIT mcpStatusChanged(m_mcp->statusSummary());
    });
    connect(m_mcp, &McpManager::serverStatusChanged, this, [this] {
        Q_EMIT mcpStatusChanged(m_mcp->statusSummary());
    });
    connect(m_mcp, &McpManager::logMessage, this, &AgentLoop::mcpLogMessage);

    // --- Checkpoints -----------------------------------------------------------
    connect(&m_checkpoints, &CheckpointManager::checkpointCreated, this, [this](const QString &id, const QString &label) {
        Q_EMIT checkpointCreated(id, label);
    });
    connect(&m_checkpoints, &CheckpointManager::failed, this, [this](const QString &error) {
        Q_EMIT checkpointFailed(error);
    });
}

AgentLoop::~AgentLoop()
{
    // A running sub-agent owns its own network client and is not parented to
    // this object, so it has to be stopped explicitly or it outlives us.
    cancelSubtask();
    if (m_locks) {
        m_locks->releaseAll(m_selfId);
    }
}

void AgentLoop::setSettings(const Settings &settings)
{
    const bool sandboxChanged = m_settings.sandbox != settings.sandbox
        || m_settings.extraDenyGlobs != settings.extraDenyGlobs;
    const bool timeoutChanged = m_settings.bashTimeoutMs != settings.bashTimeoutMs;
    const bool checkpointsChanged = m_settings.checkpointsEnabled != settings.checkpointsEnabled
        || m_settings.checkpointRetention != settings.checkpointRetention;

    m_settings = settings;
    m_client.setSettings(settings);
    syncAcpClient();
    // Keep the shared web client in step with the settings.
    m_web.setProvider(WebSearch::providerFromId(settings.webSearchProvider));
    m_web.setApiKey(settings.webSearchApiKey);
    m_web.setEndpoint(settings.webSearchEndpoint);
    m_web.setMaxResults(settings.webSearchMaxResults);
    m_web.setTimeoutMs(settings.webSearchTimeoutMs);
    syncPermissionPolicy();
    // The user roster is stored as JSON so it can be edited in the config page.
    QList<AgentProfile> customAgents;
    const QString roster = settings.agentRoster.trimmed();
    if (!roster.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(roster.toUtf8());
        if (document.isArray()) {
            const QJsonArray array = document.array();
            for (const QJsonValue &value : array) {
                const AgentProfile profile = AgentProfile::fromJson(value.toObject());
                if (profile.isValid()) {
                    customAgents.append(profile);
                }
            }
        }
    }
    m_team.setCustomAgents(customAgents);

    m_checkpoints.setEnabled(settings.checkpointsEnabled);
    if (checkpointsChanged) {
        m_checkpoints.setRetention(settings.checkpointRetention);
    }

    if (m_mcp) {
        m_mcp->setEnabled(settings.mcpEnabled);
        m_mcp->setAutoConnect(settings.mcpAutoConnect);
        m_mcp->setTimeoutMs(settings.mcpTimeoutMs);
    }

    syncToolAccess();

    if (m_workspace.isEmpty()) {
        return;
    }

    if (m_tools && timeoutChanged) {
        m_tools->setTimeoutMs(m_settings.bashTimeoutMs);
    }

    // Recreating ToolRunner destroys any in-flight bash process. Only rebuild
    // the sandbox/tools when the sandbox config actually changed, or when they
    // have not been created yet.
    if (!m_tools || sandboxChanged) {
        if (m_busy && m_tools) {
            return;
        }
        m_sandbox = std::make_unique<Sandbox>(m_workspace, m_settings.sandbox, m_settings.extraDenyGlobs);
        m_tools = std::make_unique<ToolRunner>(*m_sandbox, m_bridge, this);
        m_tools->setTimeoutMs(m_settings.bashTimeoutMs);
        m_tools->setProjectGraph(m_projectGraph.get());
    }
}

void AgentLoop::setWorkspace(const QString &workspace)
{
    m_workspace = workspace;
    m_acp.setWorkspace(workspace);

    m_modes.setWorkspace(m_workspace);
    m_team.setWorkspace(m_workspace);
    m_checkpoints.setWorkspace(m_workspace);
    m_checkpoints.setRetention(m_settings.checkpointRetention);
    if (m_mcp) {
        m_mcp->setWorkspace(m_workspace);
        m_mcp->setEnabled(m_settings.mcpEnabled);
        m_mcp->setAutoConnect(m_settings.mcpAutoConnect);
        m_mcp->setTimeoutMs(m_settings.mcpTimeoutMs);
    }
    syncPermissionPolicy();
    syncToolAccess();

    if (!m_workspace.isEmpty()) {
        m_sandbox = std::make_unique<Sandbox>(m_workspace, m_settings.sandbox, m_settings.extraDenyGlobs);
        m_tools = std::make_unique<ToolRunner>(*m_sandbox, m_bridge, this);
        m_tools->setTimeoutMs(m_settings.bashTimeoutMs);
        m_tools->setProjectGraph(m_projectGraph.get());
    }

    // Auto-generate or load project graph
    if (m_projectGraph) {
        QString graphFilePath = m_workspace + u"/.kateai/project_graph.json"_s;
        if (QFile::exists(graphFilePath)) {
            // Load existing graph
            if (!m_projectGraph->loadFromFile(graphFilePath)) {
                qWarning() << "Failed to load existing graph from" << graphFilePath;
                // Fall back to generating a new graph
                m_projectGraph->generateGraph(m_workspace);
            }
        } else {
            // Generate new graph
            m_projectGraph->generateGraph(m_workspace);
        }
    }
}

void AgentLoop::setDocumentBridge(DocumentBridge *bridge)
{
    // Set the document bridge for file operations and reinitialize tools if sandbox exists
    m_bridge = bridge;
    m_acp.setDocumentBridge(bridge);
    if (m_sandbox) {
        m_tools = std::make_unique<ToolRunner>(*m_sandbox, m_bridge, this);
        m_tools->setTimeoutMs(m_settings.bashTimeoutMs);
        m_tools->setProjectGraph(m_projectGraph.get());
    }
}

void AgentLoop::setEditorContext(const QString &context)
{
    // Update the editor context with information about the current document and cursor position
    m_editorContext = context;
    m_acp.setEditorContext(context);
}

ToolAccess AgentLoop::activeToolAccess() const
{
    return ModeRegistry::toolAccessFor(activeMode(), m_settings.planMode);
}

void AgentLoop::setMode(const QString &modeId)
{
    if (m_settings.agentMode == modeId) {
        return;
    }
    m_settings.agentMode = modeId;
    syncToolAccess();
    Q_EMIT modesChanged();
}

void AgentLoop::setLocks(WorkspaceLocks *locks)
{
    if (m_locks == locks) {
        return;
    }
    if (m_locks) {
        m_locks->releaseAll(m_selfId);
    }
    m_locks = locks;
}

QStringList AgentLoop::runningSubtaskIds() const
{
    return m_activeSubtasks.keys();
}

void AgentLoop::setMcpManager(McpManager *manager)
{
    if (m_ownsMcp && m_mcp) {
        m_mcp->deleteLater();
    }
    m_mcp = manager;
    m_ownsMcp = false;
    syncPermissionPolicy();
    syncToolAccess();
}

void AgentLoop::syncPermissionPolicy()
{
    m_policy.setMode(m_settings.permissionMode);

    QSet<QString> autoApproved(m_settings.autoApproveTools.cbegin(), m_settings.autoApproveTools.cend());
    QSet<QString> readOnly;
    if (m_mcp) {
        const QStringList approved = m_mcp->autoApprovedTools();
        autoApproved.unite(QSet<QString>(approved.cbegin(), approved.cend()));
        const QStringList readOnlyTools = m_mcp->readOnlyTools();
        readOnly = QSet<QString>(readOnlyTools.cbegin(), readOnlyTools.cend());
    }
    m_policy.setAutoApproveTools(autoApproved);
    m_policy.setReadOnlyTools(readOnly);
}

void AgentLoop::syncToolAccess()
{
    m_client.setToolAccess(activeToolAccess());
    m_client.setExtraToolDefinitions(m_mcp ? m_mcp->toolDefinitions() : QJsonArray());
}

QList<CheckpointInfo> AgentLoop::checkpoints() const
{
    return m_checkpoints.checkpoints();
}

QString AgentLoop::createCheckpoint(const QString &label)
{
    if (m_subtaskDepth > 0) {
        return {};
    }
    QString error;
    const QString id = m_checkpoints.createCheckpoint(label, &error);
    if (id.isEmpty() && !error.isEmpty()) {
        Q_EMIT checkpointFailed(error);
    }
    return id;
}

bool AgentLoop::restoreCheckpoint(const QString &id, QString *error)
{
    if (m_busy) {
        if (error) {
            *error = u"Stop the current turn before restoring a checkpoint."_s;
        }
        return false;
    }
    if (!m_checkpoints.restoreCheckpoint(id, error)) {
        return false;
    }
    // The workspace moved under the model, so the plan and the change set are
    // no longer trustworthy.
    m_changedPaths.clear();
    m_changesNeedVerification = false;
    Q_EMIT checkpointRestored(id);
    return true;
}

QString AgentLoop::diffAgainstCheckpoint(const QString &id) const
{
    return m_checkpoints.diffAgainstCheckpoint(id);
}

void AgentLoop::maybeCheckpoint(const QString &label)
{
    if (m_subtaskDepth > 0 || m_checkpointTaken) {
        return;
    }
    if (!m_settings.checkpointsEnabled) {
        return;
    }
    if (!m_checkpoints.isAvailable()) {
        return;
    }
    // Name the snapshot after the request so the history reads sensibly.
    QString fullLabel = label;
    if (!m_taskSummary.isEmpty()) {
        fullLabel += QStringLiteral(" during: %1").arg(m_taskSummary.left(80));
    }
    const QString id = createCheckpoint(fullLabel);
    if (!id.isEmpty()) {
        m_checkpointTaken = true;
    }
}

void AgentLoop::updateProjectGraph(const QString &filePath, const QString &content)
{
    // Update the project graph with changes to a file
    if (m_projectGraph) {
        m_projectGraph->updateGraph(filePath, content);
        m_projectGraph->saveToFile(m_workspace + u"/.kateai/project_graph.json"_s);
    }
}

QList<GraphNode*> AgentLoop::getProjectNodes() const
{
    if (m_projectGraph) {
        return m_projectGraph->getAllNodes();
    }
    return QList<GraphNode*>();
}

QList<GraphEdge*> AgentLoop::getProjectEdges() const
{
    if (m_projectGraph) {
        // Convert from internal edge representation to public interface
        QList<GraphEdge*> edges;
        for (auto it = m_projectGraph->getEdges().begin(); it != m_projectGraph->getEdges().end(); ++it) {
            edges.append(*it);
        }
        return edges;
    }
    return QList<GraphEdge*>();
}

QString AgentLoop::systemPrompt() const
{
    const ModeDefinition mode = m_modes.modeOrDefault(m_settings.agentMode);
    QString prompt = defaultSystemPrompt(m_workspace);

    // The active mode replaces the persona; the generic coding protocol below
    // still applies on top of it.
    prompt += u"\n\nACTIVE MODE: "_s + mode.name.toUpper() + u" ("_s + mode.id + u")\n"_s;
    if (!mode.roleDefinition.trimmed().isEmpty()) {
        prompt += mode.roleDefinition.trimmed() + u"\n"_s;
    }
    for (const QString &instruction : mode.customInstructions) {
        if (!instruction.trimmed().isEmpty()) {
            prompt += instruction.trimmed() + u"\n"_s;
        }
    }
    if (mode.readOnly && !m_settings.planMode) {
        prompt += u"This mode is read-only: you cannot modify files or run commands, and you must not claim to have done so.\n"_s;
    }
    prompt += u"Your available tools: "_s + accessList() + u"\n"_s;

        // In a mode that can spawn sub-agents, tell the orchestrator who it can
        // hire and what the concurrency limit actually is.
        if (activeToolAccess().allows(subtaskToolName())) {
            prompt += u"\nSUB-AGENTS:\n"_s;
            prompt += u"You can delegate with new_task. Available agents:\n"_s + m_team.describeRoster() + u"\n"_s;
            prompt += u"At most %1 sub-agents may run at once, so ask for independent subtasks in the SAME response and they "
                      u"will run in parallel. Do not start a dependent task before its input is ready.\n"_s
                          .arg(qMax(1, m_settings.maxParallelSubtasks));
            prompt += u"A sub-agent cannot see this conversation: restate everything it needs. Read its returned answer "
                      u"before you rely on it, and pass include_transcript when you need to verify how it worked.\n"_s;
            prompt += u"Delegate only when it genuinely helps: a self-contained piece of work, or something needing a "
                      u"specialist role. Small edits and work tightly coupled to what you already know are faster and more "
                      u"reliable done by you. You remain responsible for whatever a sub-agent returns.\n"_s;
            prompt += u"Two agents editing the same file at once is refused. Give each agent its own files.\n"_s;
        }
    prompt += u"\n\nAgent execution protocol:\n"_s
              u"1. Understand the requested outcome and inspect the relevant project before editing.\n"_s
              u"2. Work incrementally: make the smallest coherent change, then observe the result before choosing another action.\n"_s
              u"3. Never assume an edit worked merely because the tool returned; use the tool output as evidence.\n"_s
              u"4. After any mutation, verify the affected file or behavior with a focused read, test, build, lint, or equivalent check.\n"_s
              u"5. When verification fails, diagnose the actual failure and make a targeted repair; do not repeat the same failing action unchanged.\n"_s
              u"6. Every successful tool result is an observation. Use it before choosing the next action; do not rerun a command just to obtain the same output.\n"_s
              u"6a. If output is truncated, do not rerun the same unbounded command or switch through equivalent commands. Request a specific file range, offset, search pattern, or output filter, then continue from the returned excerpt.\n"_s
              u"7. Do not repeat an identical tool action while the project state is unchanged. Prefer one purposeful tool step over speculative exploration.\n"_s
              u"8. Treat permission denials, sandbox failures, and tool errors as real constraints. Choose a safe alternative rather than looping.\n"_s
              u"9. Before each tool batch, briefly tell the user in natural language what you are about to inspect, change, or verify (one short sentence; do not mention tool names, APIs, or internal controller mechanics).\n"_s
              u"10. After each tool batch, briefly explain what you learned or changed and what you will do next. Keep it conversational and useful; do not narrate every individual file operation.\n"_s
              u"10a. Do not silently jump from the user's request into tool calls. A useful progress turn sounds like: 'I’ll inspect the relevant code first, then I’ll make the smallest fix and run a focused check.'\n"_s
              u"11. The user sees your streamed text while you work. Use that text for progress narration, not hidden internal reasoning. Never expose chain-of-thought, hidden reasoning, controller messages, or raw tool protocol.\n"_s
              u"12. When no further action is needed, give the user a concise final summary of what you changed and how you verified it.\n"_s
              u"13. Do not output raw tool names, tool-call JSON, controller messages, or operation logs as user-facing prose.\n"_s;

    // Add thinking and planning instructions based on settings
    if (m_settings.structuredThinking) {
        prompt += u"\n\nTHINKING PROTOCOL (MANDATORY):\n"_s
                  u"- You MUST output a <thinking>...</thinking> block BEFORE your visible response.\n"_s
                  u"- This block contains your private analysis, reasoning, and step-by-step planning.\n"_s
                  u"- The user will NOT see this block (it is collapsed by default). Be thorough and honest.\n"_s
                  u"- Include: problem analysis, root cause hypotheses, alternative approaches considered, risk assessment, file/dependency mapping, and detailed step plan.\n"_s
                  u"- Max thinking tokens: "_s + QString::number(m_settings.maxThinkingTokens) + u"\n"_s;
    }
    if (m_settings.structuredPlanning) {
        prompt += u"\nPLAN FORMAT (MANDATORY):\n"_s
                  u"- After </thinking>, output a structured plan under '## Plan' or '## Implementation Plan' heading.\n"_s
                  u"- Use numbered steps (1., 2., 3.) with concrete, verifiable actions.\n"_s
                  u"- Each step = ONE tool call or a small batch of related calls.\n"_s
                  u"- Good: 'Read auth/login.cpp lines 40-80 to understand token handling'\n"_s
                  u"- Good: 'Edit auth/login.cpp to fix token refresh logic'\n"_s
                  u"- Good: 'Run tests for auth module to verify fix'\n"_s
                  u"- Bad: 'Fix the login bug' (too vague)\n"_s
                  u"- Bad: 'Explore the codebase' (not actionable)\n"_s
                  u"- Max plan steps: "_s + QString::number(m_settings.maxPlanSteps) + u"\n"_s
                  u"- This plan is rendered as a user-visible checklist that gets checked off as you complete steps.\n"_s
                  u"- Update the plan by marking completed steps when you finish them.\n"_s;
    }
    if (m_settings.autoCollapseThinking) {
        prompt += u"\nAUTO-COLLAPSE: The thinking block will be auto-collapsed once your visible answer starts streaming.\n"_s;
    }
    if (m_settings.requireVerification) {
        prompt += u"\nVERIFICATION REQUIREMENT:\n"_s
                  u"- After ANY file mutation, you MUST verify with a focused read, test, build, or lint.\n"_s
                  u"- Verification is not optional - it's part of the step.\n"_s
                  u"- If verification fails, thinking must analyze the failure and plan a targeted fix.\n"_s
                  u"- Max verification attempts: "_s + QString::number(m_settings.maxVerificationAttempts) + u"\n"_s;
    }
    if (m_settings.selfCritique) {
        prompt += u"\nSELF-CRITIQUE:\n"_s
                  u"- Before finishing, review your work for correctness, completeness, and potential issues.\n"_s
                  u"- If you find problems, fix them before responding to the user.\n"_s;
    }
    if (m_settings.verbosity == 0) {
        prompt += u"\nVERBOSITY: Terse. Give minimal, concise responses.\n"_s;
    } else if (m_settings.verbosity == 2) {
        prompt += u"\nVERBOSITY: Detailed. Provide thorough explanations and context.\n"_s;
    }
    if (!m_settings.extraSystemPrompt.trimmed().isEmpty() && m_settings.compressSystemPrompt) {
        // Apply compression to extra system prompt if enabled
        prompt += u"\n\n"_s + compressText(m_settings.extraSystemPrompt.trimmed(), m_settings.maxSystemPromptLength, true);
    } else if (!m_settings.extraSystemPrompt.trimmed().isEmpty()) {
        prompt += u"\n\n"_s + m_settings.extraSystemPrompt.trimmed();
    }
    if (!m_editorContext.isEmpty() && m_settings.compressEditorContext) {
        // Apply compression to editor context if enabled
        QString editorContext = m_editorContext;
        if (editorContext.length() > m_settings.maxEditorContextLength) {
            editorContext = editorContext.left(m_settings.maxEditorContextLength) + u"... (truncated)"_s;
        }
        prompt += u"\n\n<editor_context>\n"_s + editorContext + u"\n</editor_context>\n"_s;
    } else if (!m_editorContext.isEmpty()) {
        prompt += u"\n\n<editor_context>\n"_s + m_editorContext + u"\n</editor_context>\n"_s;
    }
    prompt += u"\nSandbox profile: "_s + sandboxProfileId(m_settings.sandbox);
    prompt += u"\nPermission mode: "_s + permissionModeId(m_settings.permissionMode);
    prompt += u"\nAgent mode: "_s + mode.id;

    if (!m_workspace.isEmpty()) {
        QDir dir(m_workspace);
        if (dir.exists()) {
            const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
            QStringList listing;
            int shown = 0;
            for (const QFileInfo &info : entries) {
                if (info.fileName() == u".git"_s || info.fileName() == u".kateai"_s) {
                    continue;
                }
                listing.append((info.isDir() ? u"dir  "_s : u"file "_s) + info.fileName());
                if (++shown >= 40) {
                    listing.append(u"..."_s);
                    break;
                }
            }
            if (!listing.isEmpty()) {
                prompt += u"\n\n<workspace_root>\n"_s + listing.join(u'\n') + u"\n</workspace_root>"_s;
            }
        }
        if (QDir(m_workspace + u"/.git"_s).exists()) {
            QProcess git;
            git.setWorkingDirectory(m_workspace);
            git.start(u"git"_s, QStringList{u"status"_s, u"--short"_s, u"-uno"_s});
            if (git.waitForFinished(1500)) {
                QString status = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
                if (status.size() > 1200) {
                    status = status.left(1200) + u"\n..."_s;
                }
                if (!status.isEmpty()) {
                    prompt += u"\n\n<git_status>\n"_s + status + u"\n</git_status>"_s;
                }
            }
        }
    }
    if (m_settings.planMode) {
        prompt += u"\n\nPlan mode is active. Inspect the project and return a concise, ordered implementation plan. "
                  "Only read-only tools are available; do not claim to have changed files or run commands."_s;
    }
    if (m_settings.loadProjectInstructions) {
        const QString instructionPath = m_workspace + u"/KATEAI.md"_s;
        QFile instructions(instructionPath);
        if (instructions.exists() && instructions.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QByteArray contents = instructions.read(32768);
            if (!contents.isEmpty()) {
                // Apply compression to project instructions if enabled
                if (m_settings.compressProjectInstructions && contents.size() > m_settings.maxProjectInstructionsLength) {
                    contents = contents.left(m_settings.maxProjectInstructionsLength);
                    // Add truncation indicator
                    contents += "\n... (project instructions truncated)";
                }
                prompt += u"\n\n<project_instructions path=\"KATEAI.md\">\n"_s
                    + QString::fromUtf8(contents) + u"\n</project_instructions>"_s;
            }
        }
    }

    // Layered rules: user-wide rules, then .kateai/rules (and the Kilo/Cline
    // equivalents), then AGENTS.md.
    if (m_settings.loadAgentRules) {
        const QList<RulesLoader::Block> ruleBlocks = RulesLoader::load(m_workspace, mode.id);
        const QString rendered = RulesLoader::render(ruleBlocks, m_settings.globalRules);
        if (!rendered.trimmed().isEmpty()) {
            prompt += u"\n\nProject rules to follow:\n"_s + rendered;
        }
    }
    
    // Add project graph information to help the agent understand the project structure
    if (m_projectGraph && m_projectGraph->getNodeCount() > 0 && m_settings.compressProjectGraph) {
        prompt += u"\n\n<project_graph>\n"_s;
        
        // Apply context compression based on settings
        int maxNodes = m_settings.maxGraphNodes;
        int maxEdges = m_settings.maxGraphEdges;
        
        if (m_settings.contextCompressionLevel == 0) {
            // Full context - include all nodes and edges
            prompt += u"Project contains "_s + QString::number(m_projectGraph->getNodeCount()) + u" nodes and "_s + 
                      QString::number(m_projectGraph->getEdgeCount()) + u" edges.\n"_s;
            
            // Include node types for full context
            QMap<QString, int> typeCount;
            for (auto it = m_projectGraph->getNodes().begin(); it != m_projectGraph->getNodes().end(); ++it) {
                const GraphNode *node = it.value();
                typeCount[node->type]++;
            }
            
            prompt += u"Node types:\n"_s;
            for (auto it = typeCount.begin(); it != typeCount.end(); ++it) {
                prompt += u"  - "_s + it.key() + u": "_s + QString::number(it.value()) + u"\n"_s;
            }
            
            // Include key dependencies for full context
            prompt += u"\nKey dependencies:\n"_s;
            for (auto it = m_projectGraph->getEdges().begin(); it != m_projectGraph->getEdges().end(); ++it) {
                const GraphEdge *edge = it.value();
                if (edge->relationship == u"imports"_s || edge->relationship == u"calls"_s || edge->relationship == u"extends"_s) {
                    prompt += u"  - "_s + edge->sourceId + u" -> "_s + edge->targetId +
                              u" ("_s + edge->relationship + u")\n"_s;
                }
            }
        } else {
            // Compressed context - include limited information
            prompt += u"Project contains "_s + QString::number(qMin(m_projectGraph->getNodeCount(), maxNodes)) + u" nodes and "_s + 
                      QString::number(qMin(m_projectGraph->getEdgeCount(), maxEdges)) + u" edges.\n"_s;
            
            if (m_settings.contextCompressionLevel == 1) {
                // Summary level - include node types and key dependencies
                QMap<QString, int> typeCount;
                for (auto it = m_projectGraph->getNodes().begin(); it != m_projectGraph->getNodes().end(); ++it) {
                    const GraphNode *node = it.value();
                    typeCount[node->type]++;
                }
                
                prompt += u"Node types:\n"_s;
                for (auto it = typeCount.begin(); it != typeCount.end(); ++it) {
                    prompt += u"  - "_s + it.key() + u": "_s + QString::number(it.value()) + u"\n"_s;
                }
                
                // Add key dependencies (limited to most important relationships)
                prompt += u"\nKey dependencies:\n"_s;
                int edgeCount = 0;
                for (auto it = m_projectGraph->getEdges().begin(); it != m_projectGraph->getEdges().end() && edgeCount < maxEdges; ++it) {
                    const GraphEdge *edge = it.value();
                    if (edge->relationship == u"imports"_s || edge->relationship == u"calls"_s || edge->relationship == u"extends"_s) {
                        prompt += u"  - "_s + edge->sourceId + u" -> "_s + edge->targetId +
                                  u" ("_s + edge->relationship + u")\n"_s;
                        edgeCount++;
                    }
                }
            } else if (m_settings.contextCompressionLevel == 2) {
                // Minimal level - just basic structure
                prompt += u"Project structure overview.\n"_s;
                if (m_settings.includeFileContents) {
                    prompt += u"Key files and directories are available for inspection.\n"_s;
                }
            } else if (m_settings.contextCompressionLevel == 3) {
                // Ultra-minimal level - only essential info
                prompt += u"Project overview available. Use tools to explore specific files as needed.\n"_s;
            }
        }
        
        prompt += u"</project_graph>\n"_s;
    }
    
    return prompt;
}

void AgentLoop::resetConversation()
{
    abort();
    cancelSubtask();
    m_acp.resetSession();
    m_messages.clear();
    m_policy.revokeSession();
    m_modelRequests = 0;
    m_toolCalls = 0;
    m_stateEpoch = 0;
    m_actionSignatures.clear();
    m_actionRepeatCounts.clear();
    m_actionsThisModelTurn.clear();
    m_recoveryPromptCount = 0;
    m_repeatedTruncatedObservationCount = 0;
    m_changedPaths.clear();
    m_changesNeedVerification = false;
    m_verificationAttempted = false;
    m_verificationPromptCount = 0;
    m_currentThinking.clear();
    m_currentPlan = QJsonArray();
    m_planShown = false;
    m_currentAssistant.clear();
}

void AgentLoop::abort()
{
    const bool hadActiveTurn = m_busy || m_client.isBusy() || m_acp.isBusy() || !m_queue.isEmpty() || !m_pendingResults.isEmpty()
        || !m_waitingCall.name.isEmpty() || m_nextModelTimer.isActive() || !m_activeSubtasks.isEmpty();

    m_nextModelTimer.stop();
    m_client.abort();
    m_acp.cancel();
    m_web.abort();
    cancelSubtask();
    m_inFlight = 0;
    m_queue.clear();
    m_pendingResults.clear();
    m_waitingCall = {};
    m_waitingRequest = {};
    m_busy = false;
    m_state = State::Idle;
    if (m_locks) {
        m_locks->releaseAll(m_selfId);
    }
    m_owedPaths.clear();

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

    if (usesAcpNative(m_settings)) {
        startAcpTurn(userText);
        return;
    }

    m_busy = true;
    m_state = State::WaitingForNextModel;
    m_modelRequests = 0;
    m_toolCalls = 0;
    m_stateEpoch = 0;
    m_queue.clear();
    m_pendingResults.clear();
    m_waitingCall = {};
    m_waitingRequest = {};
    m_currentAssistant.clear();
    m_actionSignatures.clear();
    m_actionRepeatCounts.clear();
    m_actionsThisModelTurn.clear();
    m_recoveryPromptCount = 0;
    m_repeatedTruncatedObservationCount = 0;
    m_checkpointTaken = false;
    m_compactionLevel = 0;
    m_contextOverflowRetries = 0;
    m_taskSummary = userText.simplified().left(80);

    if (m_messages.isEmpty()) {
        ChatMessage system;
        system.role = ChatMessage::Role::System;
        system.content = systemPrompt();
        m_messages.append(system);
    } else if (m_messages.first().role == ChatMessage::Role::System) {
        m_messages.first().content = systemPrompt();
    }

    ChatMessage user;
    user.role = ChatMessage::Role::User;
    user.content = userText;
    m_messages.append(user);
    Q_EMIT userMessage(userText);
    Q_EMIT activityUpdated(u"I’ll inspect the relevant parts of the project and work through the task step by step."_s);

    scheduleNextModelStep();
}

QList<ChatMessage> AgentLoop::m_messagesForRequest() const
{
    ContextManager::Options options;
    options.contextWindow = m_settings.contextWindow > 0
        ? m_settings.contextWindow
        : ContextManager::contextWindowFor(modelFor(m_settings));
    options.reserveForResponse = m_settings.contextWindowReserve > 0
        ? m_settings.contextWindowReserve
        : 8192;
    options.autoCompact = m_settings.compressOldMessages;
    options.compactionLevel = m_compactionLevel;
    if (m_settings.keepRecentTokens > 0) {
        options.keepRecentTokens = m_settings.keepRecentTokens;
    } else if (m_settings.smartContextTruncation) {
        options.keepRecentTokens = qMax(2000, options.reserveForResponse * 2);
    } else {
        options.keepRecentTokens = qMax(800, m_settings.compressionThreshold / 4);
    }
    return ContextManager::build(m_messages, options);
}

bool AgentLoop::canStartModelRequest(QString *error) const
{
    if (!m_busy) {
        if (error) *error = u"The agent is not running."_s;
        return false;
    }
    if (m_state != State::WaitingForNextModel) {
        if (error) *error = u"The agent is not ready for another model request."_s;
        return false;
    }
    if (m_client.isBusy()) {
        if (error) *error = u"A model request is already in progress."_s;
        return false;
    }
    if (m_settings.maxModelRequests > 0 && m_modelRequests >= m_settings.maxModelRequests) {
        if (error) {
            *error = u"Model-request budget exhausted (%1 requests)."_s.arg(m_settings.maxModelRequests);
        }
        return false;
    }
    return true;
}

void AgentLoop::scheduleNextModelStep()
{
    if (!m_busy) {
        return;
    }

    // Exhausting the request budget is not a failure. Ending the turn as an
    // error threw away the work already done and left the user with a red
    // banner instead of the answer they had already paid for.
    if (m_settings.maxModelRequests > 0 && m_modelRequests >= m_settings.maxModelRequests) {
        Q_EMIT statusChanged(i18n("Finishing: the model-request budget for this turn is spent."));
        finishTurn();
        return;
    }

    QString error;
    if (!canStartModelRequest(&error)) {
        finishWithFailure(error);
        return;
    }

    const qint64 delay = rateLimitDelayMs(QDateTime::currentMSecsSinceEpoch());
    m_state = State::WaitingForNextModel;
    if (delay == 0) {
        QMetaObject::invokeMethod(this, &AgentLoop::sendToModel, Qt::QueuedConnection);
        return;
    }

    Q_EMIT statusChanged(i18n("Pacing the next step to stay within the provider limit…"));
    m_nextModelTimer.stop();
    m_nextModelTimer.start(static_cast<int>(qMin<qint64>(delay, std::numeric_limits<int>::max())));
}

qint64 AgentLoop::rateLimitDelayMs(qint64 now) const
{
    // A fixed one-minute window rather than a true sliding log: the window only
    // needs to be good enough to keep a burst from tripping the provider.
    constexpr qint64 kWindowMs = 60'000;
    const int rpm = qMax(1, m_settings.requestsPerMinute);
    qint64 oldest = 0;
    int used = 0;
    for (const qint64 stamp : m_modelRequestTimes) {
        if (now - stamp >= kWindowMs) {
            continue;
        }
        if (used == 0) {
            oldest = stamp;
        }
        ++used;
    }
    if (used < rpm) {
        return 0;
    }
    return qMax<qint64>(1, kWindowMs - (now - oldest) + 25);
}

void AgentLoop::sendToModel()
{
    if (!m_busy) {
        return;
    }

    QString error;
    if (!canStartModelRequest(&error)) {
        finishWithFailure(error);
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (rateLimitDelayMs(now) > 0) {
        // Reschedule rather than send; the queued call has not consumed a slot.
        scheduleNextModelStep();
        return;
    }
    m_modelRequestTimes.enqueue(now);

    const QList<ChatMessage> requestMessages = m_messagesForRequest();

    ++m_modelRequests;
    m_currentAssistant.clear();
    m_currentThinking.clear();
    m_thinkingFinishedEmitted = false;
    m_currentPlan = QJsonArray();
    m_planShown = false;
    m_state = State::WaitingForModel;

    if (m_settings.structuredThinking) {
        Q_EMIT statusChanged(u"Thinking…"_s);
    } else if (m_settings.thinkingMode) {
        Q_EMIT statusChanged(u"Working on it…"_s);
    }

    m_client.complete(requestMessages);
}

void AgentLoop::syncAcpClient()
{
    m_acp.setSettings(m_settings);
    m_acp.setWorkspace(m_workspace);
    m_acp.setDocumentBridge(m_bridge);
    m_acp.setEditorContext(m_editorContext);
    if (m_mcp) {
        m_acp.setMcpServers(m_mcp->servers());
    }
    if (!usesAcpNative(m_settings) && !m_acp.isBusy()) {
        m_acp.stop();
    }
}

void AgentLoop::startAcpTurn(const QString &userText)
{
    m_busy = true;
    m_state = State::WaitingForModel;
    m_modelRequests = 0;
    m_toolCalls = 0;
    m_queue.clear();
    m_pendingResults.clear();
    m_waitingCall = {};
    m_waitingRequest = {};
    m_currentAssistant.clear();
    m_currentThinking.clear();
    m_thinkingFinishedEmitted = false;
    m_currentPlan = QJsonArray();
    m_planShown = false;
    m_checkpointTaken = false;
    m_taskSummary = userText.simplified().left(80);

    ChatMessage user;
    user.role = ChatMessage::Role::User;
    user.content = userText;
    m_messages.append(user);
    Q_EMIT userMessage(userText);
    Q_EMIT activityUpdated(i18n("Talking to %1 over ACP…", acpAgentDisplayName(m_settings)));
    Q_EMIT statusChanged(u"Working on it…"_s);

    syncAcpClient();
    ++m_modelRequests;
    m_acp.prompt(userText);
}

void AgentLoop::onAcpPromptFinished(const QString &stopReason, const QString &text, const QJsonArray &toolCalls)
{
    if (!m_busy) {
        return;
    }

    ChatMessage assistant;
    assistant.role = ChatMessage::Role::Assistant;
    assistant.content = text;
    assistant.thinking = m_currentThinking;
    assistant.plan = m_currentPlan;
    assistant.toolCalls = toolCalls;
    m_messages.append(assistant);

    if (!m_thinkingFinishedEmitted && !m_currentThinking.isEmpty()) {
        m_thinkingFinishedEmitted = true;
        Q_EMIT thinkingFinished(m_currentThinking);
    }
    Q_EMIT assistantFinished(text);

    if (stopReason == u"refusal"_s) {
        finishWithFailure(u"The ACP agent refused to continue."_s);
        return;
    }
    if (stopReason == u"max_tokens"_s) {
        Q_EMIT statusChanged(i18n("The agent stopped because it hit the token limit."));
    }
    finishTurn();
}

QList<ToolCall> AgentLoop::bundleSimilarTools(const QList<ToolCall> &calls)
{
    // Keep provider order. Tool calls are executed as one model step so that
    // all observations are returned together in the next model request.
    return calls;
}

QString AgentLoop::actionSignature(const ToolCall &call) const
{
    QJsonObject arguments = call.arguments;
    if (!call.argumentsJson.isEmpty()) {
        const QJsonDocument parsed = QJsonDocument::fromJson(call.argumentsJson.toUtf8());
        if (parsed.isObject()) {
            arguments = parsed.object();
        }
    }
    // Serialize the parsed object rather than preserving the model's original
    // key order, so reordering JSON fields cannot bypass repeat detection.
    const QByteArray args = QJsonDocument(arguments).toJson(QJsonDocument::Compact);
    // Keep this independent of stateEpoch so duplicate calls emitted in the
    // same model response remain duplicates even if the first call mutates the
    // project. The epoch is added separately when checking repetition across
    // model turns.
    return call.name + u"|"_s + QString::fromUtf8(args);
}

bool AgentLoop::isMutationTool(const QString &toolName) const
{
    return toolName == u"write_file"_s || toolName == u"edit_file"_s
        || toolName == u"multi_edit_file"_s || toolName == u"multi_replace_file_content"_s;
}

bool AgentLoop::isRepeatSensitiveTool(const QString &toolName) const
{
    // Any identical call can become a hot loop if the model ignores its prior
    // observation. The state epoch lets a read/search be repeated after a
    // successful mutation changes the project.
    return !toolName.isEmpty();
}

void AgentLoop::appendControllerMessage(const QString &content)
{
    ChatMessage controller;
    controller.role = ChatMessage::Role::User;
    controller.content = u"[KateAI agent controller] "_s + content;
    m_messages.append(controller);
}

bool AgentLoop::isVerificationTool(const QString &toolName) const
{
    return toolName == u"read_file"_s || toolName == u"grep"_s || toolName == u"bash"_s;
}

bool AgentLoop::isVerificationForChangedFiles(const ToolCall &call) const
{
    if (!m_changesNeedVerification) {
        return false;
    }
    if (call.name == u"read_file"_s || isMutationTool(call.name)) {
        QString path = call.arguments.value(u"path"_s).toString();
        if (path.isEmpty()) {
            path = call.arguments.value(u"TargetFile"_s).toString();
        }
        return !path.isEmpty() && m_changedPaths.contains(path);
    }
    // grep/bash can be a real verification when the command/search is not
    // tied to a specific path; the model is explicitly instructed to use them
    // for tests/builds/checks after changes.
    return call.name == u"grep"_s || call.name == u"bash"_s;
}

QString AgentLoop::describePlannedWork(const QList<ToolCall> &calls) const
{
    int reads = 0;
    int mutations = 0;
    int commands = 0;
    int searches = 0;
    for (const ToolCall &call : calls) {
        if (call.name == u"read_file"_s || call.name == u"list_dir"_s || call.name == u"query_project_graph"_s) {
            ++reads;
        } else if (isMutationTool(call.name)) {
            ++mutations;
        } else if (call.name == u"bash"_s) {
            ++commands;
        } else if (call.name == u"grep"_s || call.name == u"glob"_s) {
            ++searches;
        }
    }

    QStringList parts;
    if (reads) {
        parts << (reads == 1 ? u"inspect the relevant project files"_s
                              : u"inspect %1 relevant project items"_s.arg(reads));
    }
    if (searches) {
        parts << (searches == 1 ? u"search for the relevant code"_s
                                : u"search for %1 relevant code patterns"_s.arg(searches));
    }
    if (mutations) {
        parts << (mutations == 1 ? u"make the necessary code change"_s
                                  : u"make %1 focused code changes"_s.arg(mutations));
    }
    if (commands) {
        parts << (commands == 1 ? u"run a command to check the result"_s
                                : u"run %1 checks or commands"_s.arg(commands));
    }

    if (parts.isEmpty()) {
        return u"I’m working through the next step of the task."_s;
    }

    QString sentence;
    if (parts.size() == 1) {
        sentence = parts.first();
    } else if (parts.size() == 2) {
        sentence = parts.at(0) + u" and "_s + parts.at(1);
    } else {
        sentence = parts.mid(0, parts.size() - 1).join(u", "_s) + u", and "_s + parts.last();
    }
    return u"I’m going to %1."_s.arg(sentence);
}

QString AgentLoop::summarizeCompletedWork() const
{
    int succeeded = 0;
    int failed = 0;
    int reads = 0;
    int mutations = 0;
    int commands = 0;
    int searches = 0;
    QStringList changedFiles;

    for (const ToolResult &result : m_pendingResults) {
        if (result.ok) {
            ++succeeded;
        } else {
            ++failed;
        }

        if (result.name == u"read_file"_s || result.name == u"list_dir"_s || result.name == u"query_project_graph"_s) {
            ++reads;
        } else if (isMutationTool(result.name)) {
            ++mutations;
        } else if (result.name == u"bash"_s) {
            ++commands;
        } else if (result.name == u"grep"_s || result.name == u"glob"_s) {
            ++searches;
        }
    }

    for (const ToolResult &result : m_pendingResults) {
        if (!result.ok || !isMutationTool(result.name)) {
            continue;
        }
        // Keep this intentionally compact; detailed diffs remain in the model context,
        // while the transcript only tells the user what was accomplished.
        const QString pathLine = result.output.section(u"TARGET: "_s, 1, 1).section(u'\n', 0, 0).trimmed();
        if (!pathLine.isEmpty() && !changedFiles.contains(pathLine)) {
            changedFiles.append(pathLine);
        }
    }

    if (failed > 0 && succeeded == 0) {
        return u"I ran into an issue while doing that, so I’m adjusting the approach."_s;
    }
    if (mutations > 0) {
        if (!changedFiles.isEmpty()) {
            return u"I’ve made the requested change in %1. I’m checking the result now."_s.arg(changedFiles.join(u", "_s));
        }
        return u"I’ve made the requested change. I’m checking the result now."_s;
    }
    if (commands > 0 && failed == 0) {
        return u"I’ve completed the checks from this step and am using the results to decide what to do next."_s;
    }
    if (reads > 0 && searches > 0 && failed == 0) {
        return u"I’ve inspected the relevant code and narrowed down the next step."_s;
    }
    if (reads > 0 && failed == 0) {
        return u"I’ve inspected the relevant code and am working from what I found."_s;
    }
    if (failed > 0) {
        return u"Part of that step failed, so I’m using the error to adjust the approach."_s;
    }
    return u"That step is complete. I’m deciding what’s needed next."_s;
}

QString AgentLoop::formatToolResult(const ToolCall &call, const ToolResult &result) const
{
    QString out;
    out += u"TOOL: %1\n"_s.arg(call.name);
    out += u"STATUS: %1\n"_s.arg(result.ok ? u"success"_s : u"failure"_s);

    const QString path = call.arguments.value(u"path"_s).toString();
    if (!path.isEmpty()) {
        out += u"TARGET: %1\n"_s.arg(path);
    }

    out += u"OBSERVATION:\n"_s;
    out += result.output.trimmed().isEmpty() ? u"(no output)"_s : result.output.trimmed();
    out += u"\n"_s;

    if (!result.ok) {
        out += u"NEXT: Diagnose the reported failure; do not repeat the identical action blindly.\n"_s;
    } else if (isMutationTool(call.name)) {
        out += u"NEXT: The mutation succeeded. Verify the changed behavior/file before declaring the task complete.\n"_s;
    } else if (result.output.contains(u"truncated"_s, Qt::CaseInsensitive)) {
        out += u"NEXT: Do not repeat the unbounded command. Use a specific file range, offset, search pattern, or output filter, then proceed from that excerpt.\n"_s;
    } else if (isVerificationTool(call.name)) {
        out += u"NEXT: Treat this output as evidence and choose the next necessary action; stop when the task is verified complete.\n"_s;
    } else {
        out += u"NEXT: Use this observation to choose the smallest next action; do not repeat unchanged exploration.\n"_s;
    }
    return out;
}

void AgentLoop::appendToolResult(const ToolCall &call, ToolResult result)
{
    result.toolCallId = call.id;
    result.name = call.name;

    if (result.ok && (call.name == u"bash"_s || call.name == u"read_file"_s)
        && result.output.contains(u"truncated"_s, Qt::CaseInsensitive)) {
        ++m_repeatedTruncatedObservationCount;
    }

    result.output = formatToolResult(call, result);
    m_pendingResults.append(result);
    Q_EMIT toolFinished(result);

    if (result.ok && isMutationTool(call.name)) {
        // Only a project mutation is definite progress. A successful read or
        // command can still be part of an infinite retry loop, so it must not
        // reset the repeated-call recovery/termination counter.
        m_recoveryPromptCount = 0;
        m_repeatedTruncatedObservationCount = 0;
        ++m_stateEpoch;
        m_actionRepeatCounts.clear();
        QString path = call.arguments.value(u"path"_s).toString();
        if (path.isEmpty()) {
            path = call.arguments.value(u"TargetFile"_s).toString();
        }
        if (!path.isEmpty()) {
            m_changedPaths.insert(path);
        }
        m_changesNeedVerification = true;
        m_verificationAttempted = false;
    } else if (result.ok && isVerificationForChangedFiles(call)) {
        m_verificationAttempted = true;
    }
}

void AgentLoop::appendToolResultsToConversation()
{
    for (const ToolResult &result : std::as_const(m_pendingResults)) {
        ChatMessage toolMsg;
        toolMsg.role = ChatMessage::Role::Tool;
        toolMsg.toolCallId = result.toolCallId;
        toolMsg.name = result.name;
        toolMsg.content = result.output;
        m_messages.append(toolMsg);
    }
    m_pendingResults.clear();
}

void AgentLoop::onFailed(const QString &error)
{
    const QString lower = error.toLower();
    const bool overflow = lower.contains(u"context_window_exceeded"_s)
        || lower.contains(u"context_length_exceeded"_s)
        || lower.contains(u"maximum context length"_s)
        || lower.contains(u"context window exceeded"_s)
        || lower.contains(u"prompt too large"_s)
        || lower.contains(u"too many tokens"_s)
        || lower.contains(u"payload too large"_s);
    if (overflow && m_busy && m_contextOverflowRetries < 6) {
        ++m_contextOverflowRetries;
        ++m_compactionLevel;
        if (m_modelRequests > 0) {
            --m_modelRequests;
        }
        m_state = State::WaitingForNextModel;
        Q_EMIT statusChanged(i18n("Compacting conversation to fit the context window…"));
        Q_EMIT activityUpdated(i18n("Context filled up; compacting earlier turns and continuing."));
        scheduleNextModelStep();
        return;
    }
    finishWithFailure(error);
}

void AgentLoop::onFinished(const QString &text, const QList<ToolCall> &toolCalls)
{
    if (!m_busy || m_state != State::WaitingForModel) {
        return;
    }

    ChatMessage assistant;
    assistant.role = ChatMessage::Role::Assistant;
    assistant.content = text;
    assistant.thinking = m_currentThinking;

    // Parse structured plan from the response text
    if (m_settings.structuredPlanning && !text.isEmpty()) {
        QJsonArray parsedPlan = parsePlanFromText(text);
        if (!parsedPlan.isEmpty()) {
            assistant.plan = parsedPlan;
            m_currentPlan = parsedPlan;
            m_planShown = false;
            Q_EMIT planUpdated(parsedPlan);
        }
    }

    if (!toolCalls.isEmpty()) {
        QJsonArray encoded;
        for (const ToolCall &call : toolCalls) {
            QJsonObject fn;
            fn.insert(u"name"_s, call.name);
            fn.insert(u"arguments"_s, call.argumentsJson.isEmpty()
                          ? QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact))
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
    if (!m_thinkingFinishedEmitted && !m_currentThinking.isEmpty()) {
        m_thinkingFinishedEmitted = true;
        Q_EMIT thinkingFinished(m_currentThinking);
    }
    Q_EMIT assistantFinished(text);

    // Emit plan if we have one
    if (!assistant.plan.isEmpty()) {
        Q_EMIT planUpdated(assistant.plan);
    }

    // The model is responsible for natural progress narration. If it returned
    // tool calls without any user-facing text, provide a planned-work update
    // rather than exposing raw tool operations in the UI.
    if (!toolCalls.isEmpty() && text.trimmed().isEmpty()) {
        m_actionsThisModelTurn.clear();
        m_queue = bundleSimilarTools(toolCalls);
        m_pendingResults.clear();
        Q_EMIT activityUpdated(describePlannedWork(m_queue));
        m_state = State::ExecutingTools;
        processQueue();
        return;
    }

    // A coding agent should not stop immediately after a successful mutation
    // without at least one verification attempt. One controller turn is
    // allowed to force the model back into the inspect/test loop.
    if (toolCalls.isEmpty()) {
        if (m_changesNeedVerification && !m_verificationAttempted && m_verificationPromptCount < 1) {
            requestVerificationTurn();
            return;
        }
        finishTurn();
        return;
    }

    m_actionsThisModelTurn.clear();
    m_queue = bundleSimilarTools(toolCalls);
    m_pendingResults.clear();
    if (!text.trimmed().isEmpty()) {
        // The streamed assistant text already provides the user-facing update
        // for this model turn.
    }
    m_state = State::ExecutingTools;
    processQueue();
}

void AgentLoop::finishWithFailure(const QString &error)
{
    m_nextModelTimer.stop();
    m_client.abort();
    m_busy = false;
    m_state = State::Idle;
    m_queue.clear();
    m_pendingResults.clear();
    m_waitingCall = {};
    m_waitingRequest = {};
    m_currentThinking.clear();
    m_currentPlan = QJsonArray();
    m_planShown = false;
    m_currentAssistant.clear();
    Q_EMIT failed(error);
    Q_EMIT turnFinished();
}

void AgentLoop::requestVerificationTurn()
{
    ++m_verificationPromptCount;
    ChatMessage controller;
    controller.role = ChatMessage::Role::User;
    controller.content = u"[KateAI agent controller] You made project changes but have not verified them yet. "
                         u"Before giving the final answer, inspect the affected files and/or run the most relevant focused test, build, or check. "
                         u"Only finish after using the verification result as evidence."_s;
    m_messages.append(controller);
    m_state = State::WaitingForNextModel;
    scheduleNextModelStep();
}

void AgentLoop::finishTurn()
{
    m_nextModelTimer.stop();
    m_busy = false;
    m_state = State::Idle;
    m_queue.clear();
    m_pendingResults.clear();
    m_waitingCall = {};
    m_waitingRequest = {};
    m_currentThinking.clear();
    m_currentPlan = QJsonArray();
    m_planShown = false;
    m_currentAssistant.clear();
    Q_EMIT statusChanged(QString());
    Q_EMIT turnFinished();
}

void AgentLoop::processQueue()
{
    if (!m_busy) {
        return;
    }

    if (m_queue.isEmpty()) {
        // Async tools (MCP calls, sub-agents) are still running; the turn is
        // only finished once every one of them has reported back.
        if (m_inFlight > 0) {
            return;
        }
        // A permission request is holding one call out of the queue, so the
        // batch is not finished yet either.
        if (!m_waitingCall.name.isEmpty()) {
            return;
        }
        if (m_state != State::ExecutingTools) {
            // A previous entry already closed this batch out.
            return;
        }
        if (!m_pendingResults.isEmpty()) {
            Q_EMIT activityUpdated(summarizeCompletedWork());
        }
        appendToolResultsToConversation();
        if (m_repeatedTruncatedObservationCount >= 4) {
            finishWithFailure(u"Stopped after repeated successful tool calls returned truncated output. Use a bounded file range or focused search, then continue from that observation."_s);
            return;
        }
        m_state = State::WaitingForNextModel;
        scheduleNextModelStep();
        return;
    }

    const ToolCall call = m_queue.takeFirst();
    executeOne(call);
}

void AgentLoop::executeOne(const ToolCall &call)
{
    if (!m_busy) {
        return;
    }

    if (m_settings.maxToolCalls > 0 && m_toolCalls >= m_settings.maxToolCalls) {
        ToolResult result;
        result.ok = false;
        result.output = i18n("Tool-call budget exhausted (%1 calls). No further tool execution is permitted in this turn.", m_settings.maxToolCalls);
        appendToolResult(call, result);
        while (!m_queue.isEmpty()) {
            const ToolCall skipped = m_queue.takeFirst();
            ToolResult skippedResult;
            skippedResult.ok = false;
            skippedResult.output = i18n("Skipped because the tool-call budget was exhausted.");
            appendToolResult(skipped, skippedResult);
        }
        appendToolResultsToConversation();
        // Previously this ended the turn as a failure, so a run that had
        // already made most of the edits was thrown away with an error. Instead
        // the model gets one more turn to summarise what it has, which is what
        // the user actually wants at this point. The request budget still bounds
        // it, so a model that ignores the nudge cannot loop.
        Q_EMIT statusChanged(i18n("Tool budget reached — summarising."));
        appendControllerMessage(i18n("The tool-call budget for this turn is now spent, so no further tools will run. "
                                     "Summarise what you completed, what you did not, and what the user should do next. "
                                     "Do not request any more tools."));
        m_state = State::WaitingForNextModel;
        processQueue();
        return;
    }

    if (!m_sandbox || !m_tools) {
        ToolResult result;
        result.ok = false;
        result.output = u"Tool execution is unavailable because the sandbox is not initialized."_s;
        appendToolResult(call, result);
        processQueue();
        return;
    }

    // The mode (and plan mode) decide which tools exist at all. Rejecting here
    // is the runtime half of the same rule the request advertises.
    const ToolAccess access = activeToolAccess();
    if (!access.allows(call.name)) {
        ToolResult result;
        result.ok = false;
        if (m_settings.planMode && !m_policy.isReadTool(call.name)) {
            result.output = u"Tool rejected: plan mode only permits read-only tools. Choose a read-only tool."_s;
        } else if (m_mcp && isMcpToolName(call.name) && !m_mcp->hasTool(call.name)) {
            result.output = u"Tool rejected: no MCP server named '%1' is currently connected, so '%2' is unavailable."_s
                                .arg(call.name.mid(mcpToolPrefix().size()).section(u"__"_s, 0, 0), call.name);
        } else {
            result.output = u"Tool rejected: the %1 mode does not provide the '%2' tool. Available tools: %3. "_s
                                u"Use one of those, or switch modes."_s
                                    .arg(activeMode().name, call.name, accessList());
        }
        appendToolResult(call, result);
        processQueue();
        return;
    }

    const QString signature = actionSignature(call);
    const QString stateSignature = QString::number(m_stateEpoch) + u"|"_s + signature;
    const bool repeatSensitive = isRepeatSensitiveTool(call.name);

    // Duplicate tool calls inside one model response are different from a
    // deliberate re-check. The first copy executes; later identical copies get
    // a deterministic observation without consuming another tool execution.
    if (repeatSensitive && m_actionsThisModelTurn.contains(signature)) {
        ToolResult result;
        result.ok = false;
        result.output = u"DUPLICATE TOOL CALL: this identical action was already requested earlier in the same model turn. "
                        u"It was not executed again. Reuse the earlier result and choose the next necessary action."_s;
        appendToolResult(call, result);
        ++m_actionRepeatCounts[stateSignature];
        processQueue();
        return;
    }

    if (repeatSensitive) {
        // A repeated call is only blocked while the project remains in the same
        // state. Successful mutations advance m_stateEpoch, allowing fresh
        // observations of files changed by the agent.
        const int priorCount = m_actionRepeatCounts.value(stateSignature, 0);
        if (priorCount >= 1) {
            ToolResult result;
            result.ok = false;
            result.output = u"REPEATED ACTION BLOCKED: this exact %1 action was already executed without a project-state change. "
                            u"Do not issue it again. Inspect the previous observation, choose a different action, or verify a different aspect of the task."_s.arg(call.name);
            appendToolResult(call, result);
            const int repeats = ++m_actionRepeatCounts[stateSignature];
            constexpr int MaxRecoveryPrompts = 3;
            if (repeats >= 2) {
                if (m_recoveryPromptCount < MaxRecoveryPrompts) {
                    ++m_recoveryPromptCount;
                    QString recoveryMessage;
                    switch (m_recoveryPromptCount) {
                    case 1:
                        recoveryMessage = u"The model has repeated the same tool call after it was already blocked. "
                                          u"Stop repeating it. Use the previous observation and take a materially different action. "
                                          u"Do not call the same tool with the same arguments again unless the project state changes first."_s;
                        break;
                    case 2:
                        recoveryMessage = u"This is the second warning: you have repeated the same blocked tool call again. "
                                          u"You MUST choose a different tool or different arguments. "
                                          u"Consider: reading a different file, searching for related code, running a test, or examining the error output from the previous attempt."_s;
                        break;
                    case 3:
                        recoveryMessage = u"Final warning: you have ignored previous guidance and repeated the same blocked tool call. "
                                          u"The next repetition will terminate this agent turn. "
                                          u"You must now take a fundamentally different approach - try a different tool, explore a different file, or reconsider the task strategy."_s;
                        break;
                    default:
                        recoveryMessage = u"Repeated the same blocked action. Choose a different action immediately."_s;
                        break;
                    }
                    appendControllerMessage(recoveryMessage);
                } else {
                    appendToolResultsToConversation();
                    finishWithFailure(QString(u"Stopped because the agent repeatedly issued the same action without making progress after %1 recovery prompts.").arg(MaxRecoveryPrompts));
                    return;
                }
            }
            processQueue();
            return;
        }
        m_actionRepeatCounts.insert(stateSignature, 1);
    }

    m_actionsThisModelTurn.insert(signature);
    m_actionSignatures.insert(signature);

    const PermissionRequest request = describeTool(call);
    QString reason;
    const auto verdict = m_policy.evaluate(call.name, call.arguments, *m_sandbox, &reason);

    if (verdict == PermissionPolicy::Verdict::Deny) {
        ToolResult result;
        result.ok = false;
        result.output = u"Tool denied: "_s + reason;
        appendToolResult(call, result);
        processQueue();
        return;
    }

    if (verdict == PermissionPolicy::Verdict::Ask) {
        m_waitingCall = call;
        m_waitingRequest = request;
        m_state = State::WaitingForPermission;
        Q_EMIT statusChanged(u"Waiting for permission…"_s);
        Q_EMIT permissionNeeded(request);
        return;
    }

    ++m_toolCalls;
    Q_EMIT toolStarted(request);

    // MCP calls and sub-agents answer later; the queue keeps pumping so
    // independent calls in the same model turn actually run in parallel.
    if (isAsyncTool(call.name)) {
        ++m_inFlight;
        Q_EMIT statusChanged(u"Working on the next step…"_s);
        dispatchAsyncTool(call);
        processQueue();
        return;
    }

    if (!m_policy.isReadTool(call.name)) {
        maybeCheckpoint(u"Before %1"_s.arg(call.name));
        // Two agents editing one file would silently overwrite each other,
        // because each reads it once and writes the whole result back.
        QString deniedBy;
        if (!acquireEditLock(call, &deniedBy)) {
            ToolResult result;
            result.ok = false;
            result.output = u"Tool rejected: another agent (%1) is already editing this file. Do not retry until that "
                            u"agent reports back, and do not try to work around it."_s.arg(deniedBy);
            appendToolResult(call, result);
            processQueue();
            return;
        }
    }

    Q_EMIT statusChanged(u"Working on the next step…"_s);
    ToolResult result = m_tools->run(call);
    releaseEditLocksFor(call);
    appendToolResult(call, result);
    processQueue();
}

QString AgentLoop::accessList() const
{
    QStringList names;
    const ToolAccess access = activeToolAccess();
    for (const QString &name : allBuiltInToolNames()) {
        if (access.allows(name)) {
            names.append(name);
        }
    }
    if (m_mcp) {
        for (const McpTool &tool : m_mcp->tools()) {
            if (access.allows(tool.qualifiedName())) {
                names.append(tool.qualifiedName());
            }
        }
    }
    return names.isEmpty() ? QStringLiteral("none") : names.join(u", "_s);
}

PermissionRequest AgentLoop::describeTool(const ToolCall &call) const
{
    if (m_mcp && isMcpToolName(call.name)) {
        PermissionRequest request;
        request.toolName = call.name;
        request.toolCallId = call.id;
        request.summary = u"%1 %2"_s
                              .arg(call.name, QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact)));
        request.details = QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Indented));
        request.risk = m_policy.isReadTool(call.name) ? ToolRisk::Read : ToolRisk::Execute;
        return request;
    }

    if (isSubtaskTool(call.name)) {
        PermissionRequest request;
        request.toolName = call.name;
        request.toolCallId = call.id;
        const QString description = call.arguments.value(u"description"_s).toString();
        const QString mode = call.arguments.value(u"mode"_s).toString(QStringLiteral("code"));
        request.summary = u"sub-agent (%1): %2"_s.arg(mode, description.left(200));
        request.details = QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Indented));
        request.risk = ToolRisk::Execute;
        return request;
    }

    if (isWebTool(call.name)) {
        PermissionRequest request;
        request.toolName = call.name;
        request.toolCallId = call.id;
        if (call.name == u"web_search"_s) {
            request.summary = u"Search the web: %1"_s.arg(call.arguments.value(u"query"_s).toString());
        } else {
            request.summary = u"Fetch page: %1"_s.arg(call.arguments.value(u"url"_s).toString());
        }
        request.details = QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Indented));
        // Reading the public web cannot touch the workspace.
        request.risk = ToolRisk::Read;
        return request;
    }

    return m_tools->describe(call);
}

bool AgentLoop::isSubtaskTool(const QString &toolName) const
{
    return toolName == subtaskToolName();
}

bool AgentLoop::isWebTool(const QString &toolName) const
{
    return toolName == u"web_search"_s || toolName == u"web_fetch"_s;
}

bool AgentLoop::isAsyncTool(const QString &toolName) const
{
    if (isSubtaskTool(toolName)) {
        return true;
    }
    // A search or a page fetch takes seconds. Running it inline would block
    // the loop and freeze the transcript until the network answers.
    if (isWebTool(toolName)) {
        return true;
    }
    return m_mcp && m_mcp->isAvailable(toolName);
}

void AgentLoop::startWebTool(const ToolCall &call)
{
    const QString query = call.arguments.value(u"query"_s).toString().trimmed();
    const QString url = call.arguments.value(u"url"_s).toString().trimmed();

    if (!m_web.isConfigured()) {
        ToolResult blocked;
        blocked.toolCallId = call.id;
        blocked.name = call.name;
        blocked.ok = false;
        blocked.output = m_web.configurationError();
        finishAsyncTool(call, blocked);
        return;
    }

    if (call.name == u"web_search"_s) {
        const int wanted = call.arguments.value(u"max_results"_s).toInt(0);
        if (wanted > 0) {
            m_web.setMaxResults(wanted);
        }
        connect(&m_web, &WebSearch::searchFinished, this,
                [this, call, query](const QString &callId, const QList<WebSearchResult> &results, const QString &error) {
                    // m_web is shared by every loop, and the agent may have
                    // several searches in flight at once. Without this guard the
                    // first reply completes every waiting call, which double
                    // decrements m_inFlight and corrupts the turn accounting.
                    if (callId != call.id) {
                        return;
                    }
                    ToolResult out;
                    out.toolCallId = callId;
                    out.name = call.name;
                    if (!error.isEmpty()) {
                        out.ok = false;
                        out.output = error;
                        finishAsyncTool(call, out);
                        return;
                    }
                    QStringList lines;
                    lines << u"Results for \"%1\":\n"_s.arg(query);
                    for (int i = 0; i < results.size(); ++i) {
                        lines << results.at(i).toMarkdown(i + 1);
                    }
                    lines << QString();
                    lines << i18n("Fetch a page with web_fetch before relying on a result.");
                    out.ok = true;
                    out.output = lines.join(u"\n"_s);
                    finishAsyncTool(call, out);
                },
                Qt::SingleShotConnection);
        m_web.search(query, call.id);
        return;
    }

    connect(&m_web, &WebSearch::fetchFinished, this,
            [this, call](const QString &callId, const QString &title, const QString &text, const QString &error) {
                if (callId != call.id) {
                    return;
                }
                ToolResult out;
                out.toolCallId = callId;
                out.name = call.name;
                if (!error.isEmpty()) {
                    out.ok = false;
                    out.output = error;
                    finishAsyncTool(call, out);
                    return;
                }
                out.ok = true;
                out.output = title.isEmpty() ? text : (u"# "_s + title + u"\n\n"_s + text);
                finishAsyncTool(call, out);
            },
            Qt::SingleShotConnection);
    m_web.fetchPage(url, call.id);
}

void AgentLoop::finishAsyncTool(const ToolCall &call, const ToolResult &result)
{
    if (m_inFlight > 0) {
        --m_inFlight;
    }
    if (!m_busy) {
        // The turn was aborted or replaced while the tool was in flight.
        releaseEditLocksFor(call);
        return;
    }
    // Do not clobber a pending permission request: the user may still be
    // looking at it while this result lands.
    if (m_waitingCall.name.isEmpty()) {
        m_state = State::ExecutingTools;
    }
    releaseEditLocksFor(call);
    appendToolResult(call, result);
    processQueue();
}

void AgentLoop::releaseEditLocksFor(const ToolCall &call)
{
    if (!m_locks || !isMutationTool(call.name)) {
        return;
    }
    for (const QString &path : std::as_const(m_owedPaths)) {
        m_locks->release(path, m_selfId);
    }
    m_owedPaths.clear();
}

bool AgentLoop::acquireEditLock(const ToolCall &call, QString *deniedBy)
{
    if (!m_locks || !isMutationTool(call.name)) {
        return true;
    }
    QString path = call.arguments.value(u"path"_s).toString();
    if (path.isEmpty()) {
        path = call.arguments.value(u"targetFile"_s).toString();
    }
    if (path.isEmpty()) {
        return true;
    }
    if (!m_workspace.isEmpty() && QDir::isRelativePath(path)) {
        path = m_workspace + QLatin1Char('/') + path;
    }
    if (!m_locks->tryAcquire(path, m_selfId)) {
        if (deniedBy) {
            *deniedBy = m_locks->holder(path);
        }
        return false;
    }
    m_owedPaths.append(path);
    return true;
}

void AgentLoop::dispatchAsyncTool(const ToolCall &call)
{
    if (isSubtaskTool(call.name)) {
        dispatchSubtask(call);
        return;
    }
    if (isWebTool(call.name)) {
        startWebTool(call);
        return;
    }
    if (!m_mcp) {
        ToolResult result;
        result.ok = false;
        result.output = u"MCP is not available."_s;
        finishAsyncTool(call, result);
        return;
    }

    const QString token = QStringLiteral("mcp-%1").arg(++m_asyncCounter);
    connect(
        m_mcp,
        &McpManager::toolResult,
        this,
        [this, call, token](const QString &id, bool ok, const QString &output, const QString &error) {
            if (id != token) {
                return;
            }
            ToolResult result;
            result.toolCallId = call.id;
            result.name = call.name;
            result.ok = ok;
            // On failure the manager puts the message in `error`; the agent
            // reads tool output, so it has to travel in `output`.
            result.output = ok ? output : error;
            finishAsyncTool(call, result);
        },
        Qt::SingleShotConnection);
    m_mcp->callTool(token, call.name, call.arguments);
}

// Renders the sub-agent's tool log so the orchestrator can audit its answer.
static QString subtaskLog(const QStringList &log, int toolCount)
{
    if (log.isEmpty()) {
        return QString();
    }
    return QStringLiteral("\n\n<subtask_log tools=\"%1\">\n%2\n</subtask_log>").arg(toolCount).arg(log.join(u'\n'));
}

void AgentLoop::dispatchSubtask(const ToolCall &call)
{
    const QString description = call.arguments.value(u"description"_s).toString().trimmed();
    const QString requestedMode = call.arguments.value(u"mode"_s).toString().trimmed();
    const QString requestedAgent = call.arguments.value(u"agent"_s).toString().trimmed();
    const bool includeTranscript = call.arguments.value(u"include_transcript"_s).toBool(false);

    if (description.isEmpty()) {
        ToolResult result;
        result.ok = false;
        result.output = u"The subtask needs a description of the work to perform."_s;
        finishAsyncTool(call, result);
        return;
    }
    if (m_subtaskDepth >= qMax(0, m_settings.maxSubtaskDepth)) {
        ToolResult result;
        result.ok = false;
        result.output = u"Subtask nesting limit reached (%1 levels). Do the remaining work yourself."_s.arg(m_settings.maxSubtaskDepth);
        finishAsyncTool(call, result);
        return;
    }

    const int limit = qMax(1, m_settings.maxParallelSubtasks);
    if (m_activeSubtasks.size() >= limit) {
        ToolResult result;
        result.ok = false;
        result.output = u"Too many sub-agents are already running (%1 of %2). Wait for one to finish before starting another; "
                        u"do not retry immediately."_s.arg(m_activeSubtasks.size()).arg(limit);
        finishAsyncTool(call, result);
        return;
    }

    // An agent name wins over a raw mode: the roster entry already knows which
    // mode it wants, and the orchestrator thinks in names.
    const QString lookupId = requestedAgent.isEmpty() ? requestedMode : requestedAgent;
    const AgentProfile profile = m_team.resolve(lookupId);
    // resolve() falls back to a generic agent for ids it does not know, so hold
    // on to whether this really was a roster entry: the fallback's placeholder
    // description must not be handed to the child as if it were a real role.
    const bool knownAgent = m_team.byId(lookupId).has_value();
    QString modeId = profile.modeId;
    if (m_modes.modeById(requestedMode)) {
        // An explicit mode id overrides the agent's default, which is what the
        // tool schema promises. It used to be gated on an empty agent argument,
        // so passing both silently dropped the mode.
        modeId = requestedMode;
    }

    auto *child = new AgentLoop();
    child->setMcpManager(m_mcp);
    child->setLocks(m_locks);
    child->setSubtaskDepth(m_subtaskDepth + 1);

    // Use the tool call id so the chat card, the activity log and the tool
    // result all key off the same string.
    const QString taskId = call.id.isEmpty() ? QStringLiteral("sub-%1").arg(++m_asyncCounter) : call.id;
    child->setSelfId(taskId);

    Settings childSettings = m_settings;
    // A subtask must never narrow itself: plan mode belongs to the parent turn.
    childSettings.planMode = false;
    childSettings.agentMode = m_modes.modeById(modeId) ? modeId : m_settings.agentMode;
    childSettings.checkpointsEnabled = false;
    childSettings.maxSubtaskDepth = 0;
    // Sub-agents keep their own model budget so a runaway child cannot starve
    // the siblings sharing this turn.
    if (m_settings.maxToolCalls > 0) {
        childSettings.maxToolCalls = qMax(4, m_settings.maxToolCalls / 2);
    }
    if (m_settings.maxModelRequests > 0) {
        childSettings.maxModelRequests = qMax(4, m_settings.maxModelRequests / 2);
    }

    child->setSettings(childSettings);
    child->setWorkspace(m_workspace);
    child->setDocumentBridge(m_bridge);
    child->setEditorContext(m_editorContext);

    struct SubtaskState {
        ToolCall call;
        QString taskId;
        QString answer;
        QString error;
        QStringList log;
        int toolCount = 0;
        bool ok = true;
        bool includeTranscript = false;
        qint64 startedAt = 0;
        QElapsedTimer elapsed;
        QTimer *timer = nullptr;
    };
    auto *state = new SubtaskState;
    state->call = call;
    state->taskId = taskId;
    state->includeTranscript = includeTranscript;
    state->startedAt = QDateTime::currentMSecsSinceEpoch();
    state->elapsed.start();

    connect(child, &AgentLoop::assistantFinished, child, [state](const QString &text) {
        state->answer = text;
    });
    connect(child, &AgentLoop::failed, child, [state](const QString &error) {
        state->ok = false;
        state->error = error;
    });

    // Stream the child's progress so the transcript card can show it live.
    connect(child, &AgentLoop::toolStarted, child, [this, state](const PermissionRequest &request) {
        ++state->toolCount;
        const QString line = request.summary.isEmpty() ? request.toolName : QStringLiteral("%1 %2").arg(request.toolName, request.summary);
        state->log.append(QStringLiteral("→ %1").arg(line.left(400)));
        Q_EMIT subtaskActivity(state->taskId, QStringLiteral("→ %1").arg(line.left(200)), false);
    });
    connect(child, &AgentLoop::toolFinished, child, [this, state](const ToolResult &result) {
        const QString marker = result.ok ? QStringLiteral("✓") : QStringLiteral("✗");
        const QString detail = result.ok ? QString() : QStringLiteral(" — %1").arg(result.output.left(160));
        state->log.append(QStringLiteral("  %1 %2%3").arg(marker, result.name, detail));
        Q_EMIT subtaskActivity(state->taskId, QStringLiteral("  %1 %2").arg(marker, result.name), !result.ok);
    });

    state->timer = new QTimer(child);
    state->timer->setSingleShot(true);
    state->timer->setInterval(qBound(10000, m_settings.subtaskTimeoutMs, 30 * 60 * 1000));
    connect(state->timer, &QTimer::timeout, this, [this, child, state] {
        child->abort();
        state->ok = false;
        state->error = u"The subtask exceeded its time budget."_s;
    });

    connect(
        child,
        &AgentLoop::turnFinished,
        this,
        [this, child, state] {
            if (state->timer) {
                state->timer->stop();
            }
            m_activeSubtasks.remove(state->taskId);
            if (m_locks) {
                m_locks->releaseAll(child->selfId());
            }

            ToolResult result;
            result.toolCallId = state->call.id;
            result.name = state->call.name;
            result.ok = state->ok && !child->cancelRequested();
                        result.cancelled = child->cancelRequested();
            if (child->cancelRequested()) {
                result.output = u"Subtask cancelled by the user before it finished."_s;
            } else if (state->ok) {
                QString answer = state->answer.isEmpty() ? u"The sub-agent finished without producing a visible answer."_s
                                                         : state->answer;
                if (state->includeTranscript) {
                    answer += subtaskLog(state->log, state->toolCount);
                }
                result.output = answer;
            } else {
                result.output = u"Sub-agent failed: %1"_s.arg(state->error);
                if (state->includeTranscript) {
                    result.output += subtaskLog(state->log, state->toolCount);
                }
            }

            const ToolCall pendingCall = state->call;
            const QString finishedId = state->taskId;
            delete state;
            child->deleteLater();
            Q_EMIT subtaskFinished(finishedId, result.ok);
            finishAsyncTool(pendingCall, result);
        },
        Qt::SingleShotConnection);

    m_activeSubtasks.insert(taskId, child);
    Q_EMIT subtaskStarted(taskId, profile.id, profile.name, childSettings.agentMode, description);
    state->timer->start();
    // Without this the child only ever sees its mode's generic persona, so a
    // Reviewer runs as a plain read-only question and has no idea it was picked
    // to check somebody else's work.
    const QString task = (knownAgent && !profile.description.isEmpty())
        ? u"You are the '%1' agent on the team. Your role: %2\n\nTask:\n%3"_s.arg(profile.name, profile.description, description)
        : description;
    child->start(task);
}

void AgentLoop::cancelSubtask(const QString &taskId)
{
    if (taskId.isEmpty()) {
        const QList<AgentLoop *> children = m_activeSubtasks.values();
        m_activeSubtasks.clear();
        for (AgentLoop *child : children) {
            if (m_locks) {
                m_locks->releaseAll(child->selfId());
            }
            child->requestCancel();
            child->abort();
            child->deleteLater();
        }
        return;
    }

    AgentLoop *child = m_activeSubtasks.value(taskId);
    if (!child) {
        return;
    }
    m_activeSubtasks.remove(taskId);
    if (m_locks) {
        m_locks->releaseAll(child->selfId());
    }
    child->requestCancel();
    child->abort();
    child->deleteLater();
}

void AgentLoop::resolvePermission(PermissionDecision decision)
{
    if (!m_busy || m_state != State::WaitingForPermission || m_waitingCall.name.isEmpty()) {
        return;
    }

    if (usesAcpNative(m_settings)) {
        if (decision == PermissionDecision::AllowSession) {
            m_policy.grantSession(m_waitingCall.name);
        }
        m_waitingCall = {};
        m_waitingRequest = {};
        m_state = State::WaitingForModel;
        m_acp.resolvePermission(decision);
        return;
    }

    const ToolCall call = m_waitingCall;
    const PermissionRequest request = m_waitingRequest;
    m_waitingCall = {};
    m_waitingRequest = {};

    if (decision == PermissionDecision::Deny) {
        ToolResult result;
        result.toolCallId = call.id;
        result.name = call.name;
        result.ok = false;
        result.output = u"User denied this tool call. Continue the task without assuming the denied action happened."_s;
        m_pendingResults.append(result);
        Q_EMIT toolFinished(result);
        m_state = State::ExecutingTools;
        processQueue();
        return;
    }

    if (decision == PermissionDecision::AllowSession) {
        m_policy.grantSession(call.name);
    }

    if (m_settings.maxToolCalls > 0 && m_toolCalls >= m_settings.maxToolCalls) {
        ToolResult result;
        result.ok = false;
        result.output = u"Tool-call budget exhausted before permission was resolved."_s;
        appendToolResult(call, result);
        m_state = State::ExecutingTools;
        processQueue();
        return;
    }

    ++m_toolCalls;
    Q_EMIT toolStarted(request);
    Q_EMIT statusChanged(u"Working on the next step…"_s);
    if (isAsyncTool(call.name)) {
        ++m_inFlight;
        dispatchAsyncTool(call);
        m_state = State::ExecutingTools;
        processQueue();
        return;
    }
    QString deniedBy;
    if (!acquireEditLock(call, &deniedBy)) {
        ToolResult blocked;
        blocked.toolCallId = call.id;
        blocked.name = call.name;
        blocked.ok = false;
        blocked.output = u"Tool rejected: another agent (%1) is already editing this file."_s.arg(deniedBy);
        m_pendingResults.append(blocked);
        Q_EMIT toolFinished(blocked);
        m_state = State::ExecutingTools;
        processQueue();
        return;
    }
    ToolResult result = m_tools->run(call);
    releaseEditLocksFor(call);
    appendToolResult(call, result);
    m_state = State::ExecutingTools;
    processQueue();
}

void AgentLoop::fetchModels(Provider provider)
{
    m_client.fetchModels(provider);
}

SessionStore::SessionData AgentLoop::sessionData() const
{
    SessionStore::SessionData data;
    data.messages = m_messages;
    data.currentThinking = m_currentThinking;
    data.currentPlan = m_currentPlan;
    data.planShown = m_planShown;
    data.currentAssistant = m_currentAssistant;
    data.stateEpoch = m_stateEpoch;
    data.actionSignatures = m_actionSignatures.values();
    data.actionRepeatCounts = m_actionRepeatCounts;
    data.changedPaths = m_changedPaths.values();
    data.changesNeedVerification = m_changesNeedVerification;
    data.verificationAttempted = m_verificationAttempted;
    data.verificationPromptCount = m_verificationPromptCount;
    data.modelRequests = m_modelRequests;
    data.toolCalls = m_toolCalls;
    data.acpSessionId = m_acp.sessionId();
    return data;
}

void AgentLoop::restoreSession(const SessionStore::SessionData &data)
{
    // Clear current state first
    resetConversation();

    // Restore messages
    m_messages = data.messages;

    // Restore turn state
    m_currentThinking = data.currentThinking;
    m_currentPlan = data.currentPlan;
    m_planShown = data.planShown;
    m_currentAssistant = data.currentAssistant;

    // Restore tracking state
    m_stateEpoch = data.stateEpoch;
    m_actionSignatures = QSet<QString>(data.actionSignatures.constBegin(), data.actionSignatures.constEnd());
    m_actionRepeatCounts = data.actionRepeatCounts;
    m_changedPaths = QSet<QString>(data.changedPaths.constBegin(), data.changedPaths.constEnd());
    m_changesNeedVerification = data.changesNeedVerification;
    m_verificationAttempted = data.verificationAttempted;
    m_verificationPromptCount = data.verificationPromptCount;
    m_modelRequests = data.modelRequests;
    if (!data.acpSessionId.isEmpty()) {
        m_acp.setResumeSessionId(data.acpSessionId);
    }
    m_toolCalls = data.toolCalls;
}

void AgentLoop::clearSession()
{
    // In-memory reset only. This must NOT touch stored history: ChatWidget's
    // newChat() calls clearSession() immediately after saving the current
    // conversation, and SessionStore::clear() deletes the active conversation
    // from disk. The net effect was that pressing "+" made the thread you had
    // just been working in vanish from the history list.
    resetConversation();
}

} // namespace KateAi
