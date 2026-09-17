#include "agentloop.h"
#include "graph.h"
#include "sessionstore.h"
#include "types.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QDateTime>
#include <QProcess>
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

    m_nextModelTimer.setSingleShot(true);
    connect(&m_nextModelTimer, &QTimer::timeout, this, &AgentLoop::sendToModel);

    connect(&m_client, &LlmClient::textDelta, this, [this](const QString &delta) {
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
}

void AgentLoop::setSettings(const Settings &settings)
{
    // Update internal settings and propagate to dependent components
    m_settings = settings;
    m_client.setSettings(settings);
    m_policy.setMode(settings.permissionMode);

    if (!m_workspace.isEmpty()) {
        m_sandbox = std::make_unique<Sandbox>(m_workspace, m_settings.sandbox, m_settings.extraDenyGlobs);
        m_tools = std::make_unique<ToolRunner>(*m_sandbox, m_bridge, this);
        m_tools->setTimeoutMs(m_settings.bashTimeoutMs);
        m_tools->setProjectGraph(m_projectGraph.get());
    }

    // Persist graph to JSON after generation/update
    m_projectGraph->saveToFile(m_workspace + u"/.kateai/project_graph.json"_s);

    // Regenerate project graph with new settings
    if (!m_workspace.isEmpty()) {
        m_projectGraph->generateGraph(m_workspace);
    }
}

void AgentLoop::setWorkspace(const QString &workspace)
{
    m_workspace = workspace;

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
    QString prompt = defaultSystemPrompt(m_workspace);
    prompt += u"\n\nAgent execution protocol:\n"_s
              u"1. Understand the requested outcome and inspect the relevant project before editing.\n"_s
              u"2. Work incrementally: make the smallest coherent change, then observe the result before choosing another action.\n"_s
              u"3. Never assume an edit worked merely because the tool returned; use the tool output as evidence.\n"_s
              u"4. After any mutation, verify the affected file or behavior with a focused read, test, build, lint, or equivalent check.\n"_s
              u"5. When verification fails, diagnose the actual failure and make a targeted repair; do not repeat the same failing action unchanged.\n"_s
              u"6. Do not repeat an identical tool action while the project state is unchanged. If an action has already produced the needed observation, use that observation.\n"_s
              u"7. Prefer one purposeful tool step over speculative exploration. Avoid reading the same large file repeatedly when a focused range or search is sufficient.\n"_s
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
    m_messages.clear();
    m_policy.revokeSession();
    m_modelRequests = 0;
    m_toolCalls = 0;
    m_stateEpoch = 0;
    m_actionSignatures.clear();
    m_actionRepeatCounts.clear();
    m_actionsThisModelTurn.clear();
    m_recoveryPromptCount = 0;
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
    const bool hadActiveTurn = m_busy || m_client.isBusy() || !m_queue.isEmpty() || !m_pendingResults.isEmpty()
        || !m_waitingCall.name.isEmpty() || m_nextModelTimer.isActive();

    m_nextModelTimer.stop();
    m_client.abort();
    m_queue.clear();
    m_pendingResults.clear();
    m_waitingCall = {};
    m_waitingRequest = {};
    m_busy = false;
    m_state = State::Idle;

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
    if (m_modelRequests >= qMax(1, m_settings.maxModelRequests)) {
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

    QString error;
    if (!canStartModelRequest(&error)) {
        finishWithFailure(error);
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 windowMs = 60'000;
    const int rpm = qBound(1, m_settings.requestsPerMinute, 60);
    while (!m_modelRequestTimes.isEmpty() && now - m_modelRequestTimes.head() >= windowMs) {
        m_modelRequestTimes.dequeue();
    }

    qint64 delay = 0;
    if (m_modelRequestTimes.size() >= rpm) {
        delay = qMax<qint64>(1, windowMs - (now - m_modelRequestTimes.head()) + 25);
    }

    m_state = State::WaitingForNextModel;
    if (delay == 0) {
        QMetaObject::invokeMethod(this, &AgentLoop::sendToModel, Qt::QueuedConnection);
        return;
    }

    Q_EMIT statusChanged(u"I’m pacing the next step to stay within the provider limit…"_s);
    m_nextModelTimer.stop();
    m_nextModelTimer.start(static_cast<int>(qMin<qint64>(delay, std::numeric_limits<int>::max())));
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
    const qint64 windowMs = 60'000;
    const int rpm = qBound(1, m_settings.requestsPerMinute, 60);
    while (!m_modelRequestTimes.isEmpty() && now - m_modelRequestTimes.head() >= windowMs) {
        m_modelRequestTimes.dequeue();
    }
    if (m_modelRequestTimes.size() >= rpm) {
        scheduleNextModelStep();
        return;
    }

    ++m_modelRequests;
    m_modelRequestTimes.enqueue(now);
    m_currentAssistant.clear();
    m_currentThinking.clear();
    m_currentPlan = QJsonArray();
    m_planShown = false;
    m_state = State::WaitingForModel;

    if (m_settings.structuredThinking) {
        Q_EMIT statusChanged(u"Thinking…"_s);
    } else if (m_settings.thinkingMode) {
        Q_EMIT statusChanged(u"Working on it…"_s);
    }

    m_client.complete(m_messages);
}

QList<ToolCall> AgentLoop::bundleSimilarTools(const QList<ToolCall> &calls)
{
    // Keep provider order. Tool calls are executed as one model step so that
    // all observations are returned together in the next model request.
    return calls;
}

QString AgentLoop::actionSignature(const ToolCall &call) const
{
    const QByteArray args = call.argumentsJson.isEmpty()
        ? QJsonDocument(call.arguments).toJson(QJsonDocument::Compact)
        : call.argumentsJson.toUtf8();
    // Keep this independent of stateEpoch so duplicate calls emitted in the
    // same model response remain duplicates even if the first call mutates the
    // project. The epoch is added separately when checking repetition across
    // model turns.
    return call.name + u"|"_s + QString::fromUtf8(args);
}

bool AgentLoop::isMutationTool(const QString &toolName) const
{
    return toolName == u"write_file"_s || toolName == u"edit_file"_s;
}

bool AgentLoop::isRepeatSensitiveTool(const QString &toolName) const
{
    // Reads are intentionally not hard-blocked: agents may legitimately re-read
    // a file or directory after an observation. Mutations and shell actions are
    // the operations where repeating the exact same call can become a hot loop.
    return isMutationTool(toolName) || toolName == u"bash"_s;
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
    if (call.name == u"read_file"_s || call.name == u"write_file"_s || call.name == u"edit_file"_s) {
        const QString path = call.arguments.value(u"path"_s).toString();
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
        } else if (call.name == u"write_file"_s || call.name == u"edit_file"_s) {
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
        } else if (result.name == u"write_file"_s || result.name == u"edit_file"_s) {
            ++mutations;
        } else if (result.name == u"bash"_s) {
            ++commands;
        } else if (result.name == u"grep"_s || result.name == u"glob"_s) {
            ++searches;
        }
    }

    for (const ToolResult &result : m_pendingResults) {
        if (!result.ok || (result.name != u"write_file"_s && result.name != u"edit_file"_s)) {
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
    result.output = formatToolResult(call, result);
    m_pendingResults.append(result);
    Q_EMIT toolFinished(result);

    if (result.ok && isMutationTool(call.name)) {
        ++m_stateEpoch;
        m_actionRepeatCounts.clear();
        const QString path = call.arguments.value(u"path"_s).toString();
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
    Q_EMIT statusChanged(QString());
    Q_EMIT turnFinished();
}

void AgentLoop::processQueue()
{
    if (!m_busy) {
        return;
    }

    if (m_queue.isEmpty()) {
        if (!m_pendingResults.isEmpty()) {
            Q_EMIT activityUpdated(summarizeCompletedWork());
        }
        appendToolResultsToConversation();
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

    if (m_toolCalls >= qMax(1, m_settings.maxToolCalls)) {
        ToolResult result;
        result.ok = false;
        result.output = u"Tool-call budget exhausted (%1 calls). No further tool execution is permitted in this turn."_s.arg(m_settings.maxToolCalls);
        appendToolResult(call, result);
        while (!m_queue.isEmpty()) {
            const ToolCall skipped = m_queue.takeFirst();
            ToolResult skippedResult;
            skippedResult.ok = false;
            skippedResult.output = u"Skipped because the tool-call budget was exhausted."_s;
            appendToolResult(skipped, skippedResult);
        }
        appendToolResultsToConversation();
        finishWithFailure(u"Stopped after reaching the tool-call budget (%1)."_s.arg(m_settings.maxToolCalls));
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

    if (m_settings.planMode && !m_policy.isReadTool(call.name)) {
        ToolResult result;
        result.ok = false;
        result.output = u"Tool rejected: plan mode only permits read-only tools. Choose a read-only tool."_s;
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
        const int priorCount = m_actionRepeatCounts.value(stateSignature, 0);
        if (priorCount >= 1) {
            ToolResult result;
            result.ok = false;
            result.output = u"REPEATED ACTION BLOCKED: this exact %1 action was already executed without a project-state change. "
                            u"Do not issue it again. Inspect the previous observation, choose a different action, or verify a different aspect of the task."_s.arg(call.name);
            appendToolResult(call, result);
            const int repeats = ++m_actionRepeatCounts[stateSignature];
            if (repeats >= 2) {
                if (m_recoveryPromptCount == 0) {
                    ++m_recoveryPromptCount;
                    appendControllerMessage(QString(u"The model has repeated the same mutating/execute action after it was already blocked. "
                                            u"Stop repeating it. Use the previous tool result and take a materially different action. "
                                            u"Do not call the same tool with the same arguments again unless the project state changes first."));
                } else {
                    appendToolResultsToConversation();
                    finishWithFailure(QString(u"Stopped because the agent repeatedly issued the same action without making progress."));
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

    const PermissionRequest request = m_tools->describe(call);
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
    Q_EMIT statusChanged(u"Working on the next step…"_s);
    ToolResult result = m_tools->run(call);
    appendToolResult(call, result);
    processQueue();
}

void AgentLoop::resolvePermission(PermissionDecision decision)
{
    if (!m_busy || m_state != State::WaitingForPermission || m_waitingCall.name.isEmpty()) {
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

    if (m_toolCalls >= qMax(1, m_settings.maxToolCalls)) {
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
    ToolResult result = m_tools->run(call);
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
    m_toolCalls = data.toolCalls;
}

void AgentLoop::clearSession()
{
    resetConversation();
    SessionStore::clear();
}

} // namespace KateAi
