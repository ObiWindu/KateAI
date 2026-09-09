#include "agentloop.h"
#include "graph.h"
#include "types.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <algorithm>
#include <QMap>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

AgentLoop::AgentLoop(QObject *parent)
    : QObject(parent)
{
    // Initialize the project graph for understanding and tracking the project
    m_projectGraph = std::make_unique<ProjectGraph>();

    connect(&m_client, &LlmClient::textDelta, this, [this](const QString &delta) {
        m_currentAssistant += delta;
        Q_EMIT assistantDelta(delta);
    });
    connect(&m_client, &LlmClient::finished, this, &AgentLoop::onFinished);
    connect(&m_client, &LlmClient::failed, this, &AgentLoop::onFailed);
    connect(&m_client, &LlmClient::modelsReceived, this, &AgentLoop::modelsReceived);
    connect(&m_client, &LlmClient::modelsFailed, this, &AgentLoop::modelsFailed);
}

void AgentLoop::setSettings(const Settings &settings)
{
    // Update internal settings and propagate to dependent components
    m_settings = settings;
    m_client.setSettings(settings);
    m_policy.setMode(settings.permissionMode);

    // Initialize sandbox and tool runner if workspace is set
    if (!m_workspace.isEmpty()) {
        m_sandbox = std::make_unique<Sandbox>(m_workspace, m_settings.sandbox, m_settings.extraDenyGlobs);
        m_tools = std::make_unique<ToolRunner>(*m_sandbox, m_bridge, this);
        m_tools->setTimeoutMs(m_settings.bashTimeoutMs);
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
    // Stop any ongoing AI interaction and clear all conversation state
    abort();
    
    // Clear the conversation history to start fresh
    m_messages.clear();
    
    // Revoke any active permission sessions for security
    m_policy.revokeSession();
    
    // Reset iteration counter to track tool usage
    m_iterations = 0;
}

void AgentLoop::abort()
{
    // Check if there was an active AI turn that needs to be cleaned up
    const bool hadActiveTurn = m_busy || m_client.isBusy() || !m_queue.isEmpty() || !m_pendingResults.isEmpty()
        || !m_waitingCall.name.isEmpty();
    
    // Cancel any ongoing AI operations
    m_client.abort();
    
    // Clear all pending tool calls and results
    m_queue.clear();
    m_pendingResults.clear();
    
    // Reset the agent state
    m_busy = false;
    m_waitingCall = {};
    m_waitingRequest = {};
    
    // If there was an active turn, notify the UI that it was stopped
    if (hadActiveTurn) {
        Q_EMIT statusChanged(u"Stopped"_s);
        Q_EMIT turnFinished();
    }
}

void AgentLoop::start(const QString &userText)
{
    // If already processing a request, ignore new ones
    if (m_busy) {
        return;
    }
    
    // Ensure we have an active workspace before proceeding
    if (m_workspace.isEmpty()) {
        Q_EMIT failed(u"No workspace is open."_s);
        return;
    }
    
    // Initialize tools if not already set up
    if (!m_tools) {
        setWorkspace(m_workspace);
    }

    // Reset conversation state for new interaction
    m_busy = true;
    m_iterations = 0;
    m_queue.clear();
    m_pendingResults.clear();
    m_currentAssistant.clear();

    // Initialize conversation with system prompt if this is the first message
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

    // Add user message to conversation and notify UI
    ChatMessage user;
    user.role = ChatMessage::Role::User;
    user.content = userText;
    m_messages.append(user);
    Q_EMIT userMessage(userText);
    sendToModel();
}

void AgentLoop::sendToModel()
{
    // Increment the iteration counter to track tool usage
    ++m_iterations;
    
    // Check if we've exceeded the maximum allowed iterations
    if (m_iterations > m_settings.maxIterations) {
        m_busy = false;
        Q_EMIT failed(u"Stopped after %1 tool iterations."_s.arg(m_settings.maxIterations));
        Q_EMIT turnFinished();
        return;
    }
    
    // Clear any previous assistant response
    m_currentAssistant.clear();
    
    // Notify UI that we're thinking and waiting for AI response
    if (m_settings.thinkingMode) {
        Q_EMIT statusChanged(u"Thinking…"_s);
    }
    
    // Send the conversation to the LLM client for completion
    m_client.complete(m_messages);
}

QList<ToolCall> AgentLoop::bundleSimilarTools(const QList<ToolCall> &calls)
{
    // Return empty list if no calls to process
    if (calls.isEmpty()) {
        return {};
    }

    // Group tool calls by name for potential batching optimization
    QHash<QString, QList<ToolCall>> grouped;
    for (const ToolCall &call : calls) {
        grouped[call.name].append(call);
    }

    // For now, return all calls as-is since we need to maintain order
    // In a more advanced implementation, we could batch similar calls
    // into a single request if the LLM supports it
    return calls;
}

void AgentLoop::onFailed(const QString &error)
{
    // Mark the agent as no longer busy and notify UI of the failure
    m_busy = false;
    Q_EMIT failed(error);
    Q_EMIT turnFinished();
}

void AgentLoop::onFinished(const QString &text, const QList<ToolCall> &toolCalls)
{
    // Create and populate the assistant's response message
    ChatMessage assistant;
    assistant.role = ChatMessage::Role::Assistant;
    assistant.content = text;
    
    // If the AI used any tools, encode them for the conversation history
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
    
    // Add the assistant's response to the conversation history
    m_messages.append(assistant);
    Q_EMIT assistantFinished(text);

    // If no tools were used, this turn is complete
    if (toolCalls.isEmpty()) {
        m_busy = false;
        Q_EMIT statusChanged(QString());
        Q_EMIT turnFinished();
        return;
    }

    // Bundle similar tool calls for optimization and process them
    m_queue = bundleSimilarTools(toolCalls);
    processQueue();
}

void AgentLoop::processQueue()
{
    // If there are no more tool calls to execute, process any pending results
    if (m_queue.isEmpty()) {
        // Convert all pending tool results into chat messages for the conversation history
        for (const ToolResult &result : m_pendingResults) {
            ChatMessage toolMsg;
            toolMsg.role = ChatMessage::Role::Tool;
            toolMsg.toolCallId = result.toolCallId;
            toolMsg.name = result.name;
            toolMsg.content = result.output;
            m_messages.append(toolMsg);
        }
        
        // Clear the pending results since they've been processed
        m_pendingResults.clear();
        
        // Send any new messages to the AI model for processing
        sendToModel();
        return;
    }

    // Get the next tool call from the queue and execute it
    const ToolCall call = m_queue.takeFirst();
    executeOne(call);
}

void AgentLoop::executeOne(const ToolCall &call)
{
    // Check if sandbox and tools are properly initialized before proceeding
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

    // Tool definitions are advisory to a provider. Enforce plan mode locally
    // as well, so malformed or injected tool calls cannot modify a project.
    if (m_settings.planMode && !m_policy.isReadTool(call.name)) {
        ToolResult result;
        result.toolCallId = call.id;
        result.name = call.name;
        result.ok = false;
        result.output = u"Plan mode only permits read-only project tools."_s;
        m_pendingResults.append(result);
        Q_EMIT toolFinished(result);
        processQueue();
        return;
    }

    // Prepare permission request for the tool call
    const PermissionRequest request = m_tools->describe(call);
    QString reason;
    const auto verdict = m_policy.evaluate(call.name, call.arguments, *m_sandbox, &reason);
    
    // Handle different permission verdicts
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
        // Request user permission for this tool call
        m_waitingCall = call;
        m_waitingRequest = request;
        Q_EMIT statusChanged(u"Waiting for permission…"_s);
        Q_EMIT permissionNeeded(request);
        return;
    }

    // If we get here, the tool call is approved - proceed with execution
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
    // If there's no pending permission request, just return
    if (m_waitingCall.name.isEmpty()) {
        return;
    }
    
    // Extract the pending tool call and request details
    const ToolCall call = m_waitingCall;
    m_waitingCall = {};
    const PermissionRequest request = m_waitingRequest;
    m_waitingRequest = {};

    // Handle user rejection of the tool call
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
    
    // If user allows the tool call for this session, grant permission
    if (decision == PermissionDecision::AllowSession) {
        m_policy.grantSession(call.name);
    }

    // Proceed with executing the approved tool call
    Q_EMIT toolStarted(request);
    Q_EMIT statusChanged(u"Running %1…"_s.arg(call.name));
    ToolResult result = m_tools->run(call);
    result.toolCallId = call.id;
    m_pendingResults.append(result);
    Q_EMIT toolFinished(result);
    processQueue();
}

void AgentLoop::fetchModels(Provider provider)
{
    m_client.fetchModels(provider);
}

} // namespace KateAi
