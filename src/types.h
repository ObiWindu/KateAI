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
    int maxIterations = 20;
    int bashTimeoutMs = 60000;
    bool planMode = false;
    bool loadProjectInstructions = true;
    QString extraSystemPrompt;
    QStringList extraDenyGlobs;
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

} // namespace KateAi
