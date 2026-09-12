#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace KateAi
{

enum class Provider {
    Grok,
    OpenAI,
    OpenRouter,
};

enum class PermissionMode {
    Ask,
    AcceptEdits,
    AlwaysApprove,
};

enum class SandboxProfile {
    Off,
    Workspace,
    ReadOnly,
    Strict,
};

enum class PermissionDecision {
    AllowOnce,
    AllowSession,
    Deny,
};

enum class ToolRisk {
    Read,
    Write,
    Execute,
};

struct ChatMessage {
    enum class Role { System, User, Assistant, Tool };
    Role role = Role::User;
    QString content;
    QString toolCallId;
    QString name;
    QJsonArray toolCalls;
};

struct ToolCall {
    QString id;
    QString name;
    QString argumentsJson;
    QJsonObject arguments;
};

struct CompletionChunk {
    QString contentDelta;
    QList<ToolCall> completedTools;
    bool finished = false;
    QString finishReason;
    QString error;
};

struct PermissionRequest {
    QString toolName;
    QString toolCallId;
    QString summary;
    QString details;
    QString path;
    ToolRisk risk = ToolRisk::Write;
};

struct ToolResult {
    QString toolCallId;
    QString name;
    QString output;
    bool ok = true;
};

struct Settings {
    Provider provider = Provider::Grok;
    QString grokApiKey;
    QString openaiApiKey;
    QString openrouterApiKey;
    QString grokModel = QStringLiteral("grok-4.5");
    QString openaiModel = QStringLiteral("gpt-4.1");
    QString openrouterModel = QStringLiteral("x-ai/grok-4");
    PermissionMode permissionMode = PermissionMode::Ask;
    SandboxProfile sandbox = SandboxProfile::Workspace;
    // Agent budgets are intentionally separate: API model turns, tool calls, and provider rate.
    int maxModelRequests = 40;
    int maxToolCalls = 80;
    int requestsPerMinute = 15;
    // Legacy compatibility with older KateAI settings/UI. Internally maxToolCalls is used.
    int maxIterations = 20;
    int bashTimeoutMs = 60000;
    bool planMode = false;
    bool loadProjectInstructions = true;
    bool thinkingMode = true;
    QString extraSystemPrompt;
    QStringList extraDenyGlobs;
    int contextCompressionLevel = 1; // 0=full, 1=summary, 2=minimal, 3=ultra-minimal
    int maxGraphNodes = 50; // Maximum number of nodes to include in project graph
    int maxGraphEdges = 100; // Maximum number of edges to include in project graph
    bool compressProjectGraph = true; // Whether to compress project graph information
    bool includeFileContents = true; // Whether to include file content in project graph
    int maxFileContentLength = 500; // Maximum characters per file content preview
    bool compressEditorContext = true; // Whether to compress editor context
    int maxEditorContextLength = 200; // Maximum characters for editor context
    bool compressProjectInstructions = true; // Whether to compress project instructions
    int maxProjectInstructionsLength = 2048; // Maximum characters for project instructions
    bool compressSystemPrompt = true; // Whether to compress system prompt
    int maxSystemPromptLength = 1024; // Maximum characters for system prompt
    int messageSpeed = 2; // 0=slow, 1=medium, 2=fast (default fast)
};

QString providerId(Provider provider);
QString providerLabel(Provider provider);
Provider providerFromId(const QString &id);
QString providerBaseUrl(Provider provider);
QStringList defaultModels(Provider provider);

QString permissionModeId(PermissionMode mode);
QString permissionModeLabel(PermissionMode mode);
PermissionMode permissionModeFromId(const QString &id);

QString sandboxProfileId(SandboxProfile profile);
QString sandboxProfileLabel(SandboxProfile profile);
SandboxProfile sandboxProfileFromId(const QString &id);

QString apiKeyFor(const Settings &settings);
QString modelFor(const Settings &settings);

QJsonArray toolDefinitions(bool readOnlyOnly = false);
QString defaultSystemPrompt(const QString &workspace);
QString compressText(const QString &text, int maxLength, bool enabled);

} // namespace KateAi
