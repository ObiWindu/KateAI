#include "types.h"

#include <KLocalizedString>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QString providerId(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
        return u"openai"_s;
    case Provider::OpenRouter:
        return u"openrouter"_s;
    case Provider::Grok:
    default:
        return u"grok"_s;
    }
}

QString providerLabel(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
        return i18n("OpenAI");
    case Provider::OpenRouter:
        return i18n("OpenRouter");
    case Provider::Grok:
    default:
        return i18n("Grok (xAI)");
    }
}

Provider providerFromId(const QString &id)
{
    if (id == u"openai"_s) {
        return Provider::OpenAI;
    }
    if (id == u"openrouter"_s) {
        return Provider::OpenRouter;
    }
    return Provider::Grok;
}

QString providerBaseUrl(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
        return u"https://api.openai.com/v1"_s;
    case Provider::OpenRouter:
        return u"https://openrouter.ai/api/v1"_s;
    case Provider::Grok:
    default:
        return u"https://api.x.ai/v1"_s;
    }
}

QStringList defaultModels(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
        return {u"gpt-4.1"_s, u"gpt-4o"_s, u"o3"_s, u"o4-mini"_s, u"gpt-4.1-mini"_s};
    case Provider::OpenRouter:
        return {u"x-ai/grok-4"_s,
                u"x-ai/grok-4.5"_s,
                u"openai/gpt-4.1"_s,
                u"openai/gpt-4o"_s,
                u"anthropic/claude-sonnet-4"_s,
                u"google/gemini-2.5-pro"_s};
    case Provider::Grok:
    default:
        return {u"grok-4.5"_s, u"grok-4.6"_s, u"grok-4"_s, u"grok-3"_s};
    }
}

QString permissionModeId(PermissionMode mode)
{
    switch (mode) {
    case PermissionMode::AcceptEdits:
        return u"acceptEdits"_s;
    case PermissionMode::AlwaysApprove:
        return u"alwaysApprove"_s;
    case PermissionMode::Ask:
    default:
        return u"ask"_s;
    }
}

QString permissionModeLabel(PermissionMode mode)
{
    switch (mode) {
    case PermissionMode::AcceptEdits:
        return i18n("Accept edits");
    case PermissionMode::AlwaysApprove:
        return i18n("Always approve");
    case PermissionMode::Ask:
    default:
        return i18n("Ask");
    }
}

PermissionMode permissionModeFromId(const QString &id)
{
    if (id == u"acceptEdits"_s) {
        return PermissionMode::AcceptEdits;
    }
    if (id == u"alwaysApprove"_s || id == u"bypassPermissions"_s) {
        return PermissionMode::AlwaysApprove;
    }
    return PermissionMode::Ask;
}

QString sandboxProfileId(SandboxProfile profile)
{
    switch (profile) {
    case SandboxProfile::Off:
        return u"off"_s;
    case SandboxProfile::ReadOnly:
        return u"read-only"_s;
    case SandboxProfile::Strict:
        return u"strict"_s;
    case SandboxProfile::Workspace:
    default:
        return u"workspace"_s;
    }
}

QString sandboxProfileLabel(SandboxProfile profile)
{
    switch (profile) {
    case SandboxProfile::Off:
        return i18n("Off");
    case SandboxProfile::ReadOnly:
        return i18n("Read-only");
    case SandboxProfile::Strict:
        return i18n("Strict");
    case SandboxProfile::Workspace:
    default:
        return i18n("Workspace");
    }
}

SandboxProfile sandboxProfileFromId(const QString &id)
{
    if (id == u"off"_s) {
        return SandboxProfile::Off;
    }
    if (id == u"read-only"_s || id == u"readonly"_s) {
        return SandboxProfile::ReadOnly;
    }
    if (id == u"strict"_s) {
        return SandboxProfile::Strict;
    }
    return SandboxProfile::Workspace;
}

QString apiKeyFor(const Settings &settings)
{
    switch (settings.provider) {
    case Provider::OpenAI:
        return settings.openaiApiKey;
    case Provider::OpenRouter:
        return settings.openrouterApiKey;
    case Provider::Grok:
    default:
        return settings.grokApiKey;
    }
}

QString modelFor(const Settings &settings)
{
    switch (settings.provider) {
    case Provider::OpenAI:
        return settings.openaiModel;
    case Provider::OpenRouter:
        return settings.openrouterModel;
    case Provider::Grok:
    default:
        return settings.grokModel;
    }
}

static QJsonObject toolDef(const QString &name, const QString &description, const QJsonObject &properties, const QStringList &required)
{
    QJsonObject fn;
    fn.insert(u"name"_s, name);
    fn.insert(u"description"_s, description);
    QJsonObject params;
    params.insert(u"type"_s, u"object"_s);
    params.insert(u"properties"_s, properties);
    QJsonArray req;
    for (const QString &item : required) {
        req.append(item);
    }
    params.insert(u"required"_s, req);
    fn.insert(u"parameters"_s, params);
    QJsonObject tool;
    tool.insert(u"type"_s, u"function"_s);
    tool.insert(u"function"_s, fn);
    return tool;
}

QJsonArray toolDefinitions(bool readOnlyOnly)
{
    QJsonArray tools;

    tools.append(toolDef(u"read_file"_s,
                         u"Read a UTF-8 text file. Use offset/limit (1-based lines) for large files."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"offset"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"First line to return (1-based)."_s}}},
                             {u"limit"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Maximum number of lines to return."_s}}},
                         },
                         {u"path"_s}));

    tools.append(toolDef(u"write_file"_s,
                         u"Create or overwrite a UTF-8 text file with the given contents."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"content"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Full file contents to write."_s}}},
                         },
                         {u"path"_s, u"content"_s}));

    tools.append(toolDef(u"edit_file"_s,
                         u"Replace one exact occurrence of old_string with new_string in a file."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"old_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Exact text to find. Must be unique in the file."_s}}},
                             {u"new_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Replacement text."_s}}},
                         },
                         {u"path"_s, u"old_string"_s, u"new_string"_s}));

    tools.append(toolDef(u"list_dir"_s,
                         u"List files and directories in a folder."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Directory to list. Defaults to the workspace root."_s}}},
                         },
                         {}));

    tools.append(toolDef(u"grep"_s,
                         u"Search file contents with a regular expression."_s,
                         QJsonObject{
                             {u"pattern"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Regular expression to search for."_s}}},
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File or directory to search. Defaults to the workspace."_s}}},
                             {u"glob"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional filename glob such as *.cpp."_s}}},
                         },
                         {u"pattern"_s}));

    tools.append(toolDef(u"glob"_s,
                         u"Find files whose paths match a glob pattern."_s,
                         QJsonObject{
                             {u"pattern"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Glob such as **/*.h or src/**/*.cpp."_s}}},
                         },
                         {u"pattern"_s}));

    tools.append(toolDef(u"bash"_s,
                         u"Run a shell command in the workspace. Commands are sandboxed according to the active profile."_s,
                         QJsonObject{
                             {u"command"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Shell command to execute."_s}}},
                         },
                         {u"command"_s}));

    tools.append(toolDef(u"query_project_graph"_s,
                         u"Query the project graph for nodes, edges, dependencies, and relationships."_s,
                         QJsonObject{
                             {u"query_type"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Type of query: summary, nodes, edges, dependencies, dependents, find_related, find_path, dependency_chain"_s}}},
                             {u"node_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Node ID for queries that require a specific node"_s}}},
                             {u"relationship"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Relationship type for filtering edges"_s}}},
                             {u"source_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Source node ID for path finding"_s}}},
                             {u"target_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Target node ID for path finding"_s}}},
                         },
                         {}));

    if (!readOnlyOnly) {
        return tools;
    }

    QJsonArray readOnlyTools;
    for (const QJsonValue &tool : tools) {
        const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
        if (name == u"read_file"_s || name == u"list_dir"_s || name == u"grep"_s || name == u"glob"_s) {
            readOnlyTools.append(tool);
        }
    }
    return readOnlyTools;
}

QString defaultSystemPrompt(const QString &workspace)
{
    return u"You are Kate AI, a coding assistant inside the Kate text editor.\n"
           "Use tools to inspect and change the user's project. Prefer edit_file for small changes "
           "and write_file only for new files or full rewrites.\n"
           "Match existing code style. Do not invent files outside the workspace.\n"
           "When you run bash, prefer non-interactive commands.\n"
           "Workspace root: "_s
        + workspace;
}

} // namespace KateAi
