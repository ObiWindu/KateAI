/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "modes.h"
#include "types.h"

#include <QFileInfo>
#include <KLocalizedString>

#include <QFileInfo>

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
    case Provider::DeepSeek:
        return u"deepseek"_s;
    case Provider::OpenAICompatible:
        return u"openai-compatible"_s;
    case Provider::ClaudeCompatible:
        return u"claude-compatible"_s;
    case Provider::Kilo:
        return u"kilo"_s;
    case Provider::Acp:
        return u"acp"_s;
    case Provider::OpenCode:
        return u"opencode"_s;
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
    case Provider::DeepSeek:
        return i18n("DeepSeek");
    case Provider::OpenAICompatible:
        return i18n("OpenAI Compatible");
    case Provider::ClaudeCompatible:
        return i18n("Claude Compatible");
    case Provider::Kilo:
        return i18n("Kilo.ai");
    case Provider::Acp:
        return i18n("ACP");
    case Provider::OpenCode:
        return i18n("OpenCode Zen");
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
    if (id == u"deepseek"_s) {
        return Provider::DeepSeek;
    }
    if (id == u"openai-compatible"_s) {
        return Provider::OpenAICompatible;
    }
    if (id == u"claude-compatible"_s) {
        return Provider::ClaudeCompatible;
    }
    if (id == u"kilo"_s) {
        return Provider::Kilo;
    }
    if (id == u"acp"_s) {
        return Provider::Acp;
    }
    if (id == u"opencode"_s) {
        return Provider::OpenCode;
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
    case Provider::DeepSeek:
        return u"https://api.deepseek.com"_s;
    case Provider::OpenAICompatible:
        return u"http://localhost:11434/v1"_s;
    case Provider::ClaudeCompatible:
        return u"https://api.anthropic.com/v1"_s;
    case Provider::Kilo:
        return u"https://api.kilo.ai/v1"_s;
    case Provider::Acp:
        return u"http://localhost:8080"_s;
    case Provider::OpenCode:
        return u"https://opencode.ai/zen/v1"_s;
    case Provider::Grok:
    default:
        return u"https://api.x.ai/v1"_s;
    }
}

QString providerBaseUrl(const Settings &settings)
{
    switch (settings.provider) {
    case Provider::OpenAI:
        return u"https://api.openai.com/v1"_s;
    case Provider::OpenRouter:
        return u"https://openrouter.ai/api/v1"_s;
    case Provider::DeepSeek:
        return settings.deepseekUrl.isEmpty() ? u"https://api.deepseek.com"_s : settings.deepseekUrl;
    case Provider::OpenAICompatible:
        return settings.openaiCompatibleUrl;
    case Provider::ClaudeCompatible:
        return settings.claudeCompatibleUrl;
    case Provider::OpenCode:
        return settings.opencodeUrl.isEmpty() ? u"https://opencode.ai/zen/v1"_s : settings.opencodeUrl;
    case Provider::Kilo:
        return u"https://api.kilo.ai/v1"_s;
    case Provider::Acp:
        return settings.acpUrl;
    case Provider::Grok:
    default:
        return u"https://api.x.ai/v1"_s;
    }
}

QStringList defaultModels(Provider provider)
{
    // Intentionally empty. Model names used to be hard-coded here, which went
    // stale the moment a provider shipped a model, and offered users models
    // their key cannot reach. The list now comes from the provider itself via
    // LlmClient::fetchModels(); this function remains so callers have one place
    // to ask, and it reports "nothing known yet" honestly.
    Q_UNUSED(provider)
    return {};
}

bool providerSupportsModelListing(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
    case Provider::OpenRouter:
    case Provider::DeepSeek:
    case Provider::OpenAICompatible:
    case Provider::ClaudeCompatible:
    case Provider::Kilo:
    case Provider::Grok:
    case Provider::OpenCode:
        return true;
    case Provider::Acp:
        break;
    }
    // ACP agents advertise themselves; there is no catalogue endpoint.
    return false;
}

bool usesAcpNative(const Settings &settings)
{
    return settings.provider == Provider::Acp && settings.apiFormat == ApiFormat::AcpNative;
}

bool providerRequiresApiKey(const Settings &settings, Provider provider)
{
    if (provider == Provider::Acp && settings.apiFormat == ApiFormat::AcpNative) {
        return false;
    }
    return true;
}

bool providerIsSelectable(const Settings &settings, Provider provider)
{
    if (provider == Provider::Acp) {
        return true;
    }
    Settings copy = settings;
    copy.provider = provider;
    return !apiKeyFor(copy).trimmed().isEmpty();
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
    case Provider::DeepSeek:
        return settings.deepseekApiKey;
    case Provider::OpenAICompatible:
        return settings.openaiCompatibleApiKey;
    case Provider::ClaudeCompatible:
        return settings.claudeCompatibleApiKey;
    case Provider::Kilo:
        return settings.kiloApiKey;
    case Provider::Acp:
        return settings.acpApiKey;
    case Provider::OpenCode:
        return settings.opencodeApiKey;
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
    case Provider::DeepSeek:
        return settings.deepseekModel;
    case Provider::OpenAICompatible:
        return settings.openaiCompatibleModel;
    case Provider::ClaudeCompatible:
        return settings.claudeCompatibleModel;
    case Provider::Kilo:
        return settings.kiloModel;
    case Provider::Acp:
        return settings.acpModel;
    case Provider::OpenCode:
        return settings.opencodeModel;
    case Provider::Grok:
    default:
        return settings.grokModel;
    }
}

QStringList pickerModelsFor(const Settings &settings, Provider provider, const QStringList &catalog)
{
    QStringList models;
    for (const QString &model : catalog) {
        const QString trimmed = model.trimmed();
        if (!trimmed.isEmpty() && !models.contains(trimmed)) {
            models.append(trimmed);
        }
    }
    Settings slot = settings;
    slot.provider = provider;
    const QString configured = modelFor(slot).trimmed();
    if (!configured.isEmpty() && !models.contains(configured)) {
        models.prepend(configured);
    }
    return models;
}

static QJsonObject toolDef(const QString &name, const QString &description, const QJsonObject &properties, const QStringList &required, bool strict = false)
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
    fn.insert(u"strict"_s, strict);
    QJsonObject tool;
    tool.insert(u"type"_s, u"function"_s);
    tool.insert(u"function"_s, fn);
    return tool;
}

QJsonArray toolDefinitions(const ToolAccess &access)
{
    QJsonArray tools;

    tools.append(toolDef(u"read_file"_s,
                         u"Read a UTF-8 text file. Use offset/limit (1-based lines) for large files."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"offset"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"First line to return (1-based)."_s}}},
                             {u"limit"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Maximum number of lines to return."_s}}},
                         },
                         {u"path"_s},
                         false));

    tools.append(toolDef(u"write_file"_s,
                         u"Create or overwrite a UTF-8 text file with the given contents."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"content"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Full file contents to write."_s}}},
                         },
                         {u"path"_s, u"content"_s},
                         false));

    tools.append(toolDef(u"edit_file"_s,
                         u"Replace exact text in a file. old_string must match exactly, including whitespace. "
                         u"By default it must occur once; set replace_all to true to replace every occurrence."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"old_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Exact text to find."_s}}},
                             {u"new_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Replacement text."_s}}},
                             {u"replace_all"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, replace every occurrence instead of requiring a unique match."_s}}},
                         },
                         {u"path"_s, u"old_string"_s, u"new_string"_s},
                         false));


    QJsonObject chunkProperties{
        {u"old_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Exact text to find in the file."_s}}},
        {u"new_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Replacement text."_s}}},
        {u"replace_all"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, replace every occurrence instead of requiring a unique match."_s}}},
    };
    QJsonObject chunkItem{
        {u"type"_s, u"object"_s},
        {u"properties"_s, chunkProperties},
        {u"required"_s, QJsonArray{u"old_string"_s, u"new_string"_s}},
    };
    tools.append(toolDef(u"multi_edit_file"_s,
                         u"Perform multiple non-contiguous edits to the same file in a single tool call. "
                         u"Each edit specifies an exact old_string to replace with new_string."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"edits"_s, QJsonObject{{u"type"_s, u"array"_s}, {u"items"_s, chunkItem}, {u"description"_s, u"Array of edit chunks to apply in order."_s}}},
                         },
                         {u"path"_s, u"edits"_s},
                         false));


    tools.append(toolDef(u"list_dir"_s,
                         u"List files and directories in a folder."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Directory to list. Defaults to the workspace root."_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"grep"_s,
                         u"Search file contents with a regular expression. Returns path:line:content."_s,
                         QJsonObject{
                             {u"pattern"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Regular expression to search for."_s}}},
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File or directory to search. Defaults to the workspace."_s}}},
                             {u"glob"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional filename glob such as *.cpp."_s}}},
                             {u"case_insensitive"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, match without regard to case."_s}}},
                             {u"context"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Number of context lines to include before and after each match."_s}}},
                         },
                         {u"pattern"_s},
                         false));

    tools.append(toolDef(u"glob"_s,
                         u"Find files whose paths match a glob pattern."_s,
                         QJsonObject{
                             {u"pattern"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Glob such as **/*.h or src/**/*.cpp."_s}}},
                         },
                         {u"pattern"_s},
                         false));

    tools.append(toolDef(u"bash"_s,
                         u"Run a shell command in the workspace. Commands are sandboxed according to the active profile."_s,
                         QJsonObject{
                             {u"command"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Shell command to execute."_s}}},
                         },
                         {u"command"_s},
                         false));


    tools.append(toolDef(u"query_project_graph"_s,
                         u"Query the indexed project graph: files, symbols, imports, and relationships. "
                         u"Use this before blindly searching when you need structure or dependencies."_s,
                         QJsonObject{
                             {u"query_type"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"summary, nodes, edges, dependencies, dependents, find_related, find_path, dependency_chain"_s}}},
                             {u"node_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Node id, path, or name for node-scoped queries"_s}}},
                             {u"relationship"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional relationship filter: imports, calls, extends, contains, references"_s}}},
                             {u"source_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Source node for path finding"_s}}},
                             {u"target_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Target node for path finding"_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"web_search"_s,
                             u"Search the public web and return ranked results with titles, URLs and snippets. "
                             u"Use it for library documentation, error messages, API references, or anything outside "
                             u"this workspace. Prefer a specific query over a broad one; cite the URL when you use a result."_s,
                             QJsonObject{
                                 {u"query"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"The search query."_s}}},
                                 {u"max_results"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"How many results to return (1-20). Defaults to 5."_s}}},
                             },
                             {u"query"_s},
                             false));

    tools.append(toolDef(u"web_fetch"_s,
                             u"Fetch a web page and return its readable text, with scripts and navigation stripped. "
                             u"Use it after web_search to read a specific result instead of relying on the snippet. "
                             u"Only http and https URLs are fetched."_s,
                             QJsonObject{
                                 {u"url"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"The absolute http(s) URL to fetch."_s}}},
                             },
                             {u"url"_s},
                             false));


        tools.append(toolDef(subtaskToolName(),
                         u"Spawn a sub-agent to handle a self-contained piece of work, then return its result to you. "
                         u"Give it a complete task description with all context it needs, since it cannot see this "
                         u"conversation. Independent subtasks requested in the same response run in parallel, so batch "
                         u"them. Pick an agent by name when one fits, or a mode when you need a specific tool set. "
                         u"Returns the sub-agent's final answer."_s,
                         QJsonObject{
                             {u"description"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"The full task for the sub-agent, including the goal, relevant files, and the expected output format."_s}}},
                             {u"agent"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"A named agent that suits the work: scout (read-only questions), architect (design and plans), coder (implementation), debugger (root-cause fixes), reviewer (check someone else's work), or a custom agent."_s}}},
                             {u"mode"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Mode id to run the sub-agent in, overriding the agent's default: code, ask, architect, debug, orchestrator, or a custom mode. Defaults to the agent's mode, or code."_s}}},
                             {u"include_transcript"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, the result also includes the sub-agent's tool log, so you can see how it reached its answer. Use it when you need to verify the work."_s}}},
                         },
                         {u"description"_s},
                         false));


    if (access.allowAll) {
        return tools;
    }

    QJsonArray allowedTools;
    for (const QJsonValue &tool : tools) {
        const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
        if (access.allows(name)) {
            allowedTools.append(tool);
        }
    }
    return allowedTools;
}

QString defaultSystemPrompt(const QString &workspace)
{
    return u"You are Kate AI, a coding agent inside the Kate text editor.\n"
           "You solve the user's request by inspecting the workspace with tools, making focused edits, and verifying the result.\n"
           "\n"
           "How to work:\n"
           "- Start by locating the relevant code (query_project_graph, glob, grep, list_dir) before guessing paths.\n"
           "- Read only the files and ranges you need. Prefer offset/limit on large files.\n"
           "- Prefer edit_file or multi_edit_file for surgical changes. Use multi_edit_file when modifying multiple non-contiguous locations in the same file. Use replace_all when the same unique snippet should change everywhere.\n"
           "- Use write_file only for new files or complete rewrites. Do not invent files outside the workspace.\n"
           "- Match existing style, naming, imports, and architecture. Do not add unrelated refactors or comments.\n"
           "- After edits, verify with a focused read, test, build, or lint. Treat tool output as evidence.\n"
           "- If a tool fails, diagnose the actual error and change approach; do not retry the identical call.\n"
           "- Prefer one purposeful batch of tools over speculative exploration.\n"
           "- Shell commands must be non-interactive. Do not request secrets or write credential files.\n"
                      "\n"
                      "WEB RESEARCH:\n"
                      "- Use web_search for anything outside this workspace: library and API documentation, error messages, "
                      "release notes, or the current state of a dependency. Prefer it over answering from memory when a version "
                      "or API could have changed.\n"
                      "- Read a result with web_fetch before relying on it; snippets alone are often misleading.\n"
                      "- Cite the URL when a web result informs your answer.\n"
                      "- Never send workspace contents, file contents, secrets, or user code to a search provider. Queries only.\n"
           "\n"
           "REASONING & PLANNING (required):\n"
           "1. THINKING: Before every response, output your internal reasoning in a <thinking> block. This is your private chain-of-thought: analyze the request, consider alternatives, plan steps, and anticipate issues. The user will NOT see this block - it is collapsed by default. Be thorough.\n"
           "2. PLAN: After thinking, output a structured implementation plan as a numbered list under a 'Plan:' or 'Implementation Plan:' heading. Each step should be a concrete, verifiable action. This plan IS visible to the user as a checklist.\n"
           "3. EXECUTION: Follow your plan step by step. After completing each step, you may update the plan by marking steps complete.\n"
           "\n"
           "THINKING PROTOCOL (detailed):\n"
           "- Your <thinking> block MUST come FIRST, before any visible text.\n"
           "- Include in thinking: problem analysis, root cause hypotheses, alternative approaches considered, risk assessment, file/dependency mapping, and a detailed step-by-step plan.\n"
           "- Be honest about uncertainty. If you don't know something, say so in thinking and plan to investigate.\n"
           "- The thinking block is your private workspace - use it fully. There is no penalty for thorough reasoning.\n"
           "\n"
           "PLAN FORMAT (structured):\n"
           "- After </thinking>, output a plan under '## Plan' or '## Implementation Plan' heading.\n"
           "- Use numbered steps (1., 2., 3.) with concrete, verifiable actions.\n"
           "- Each step = ONE tool call or a small batch of related calls.\n"
           "- Good: 'Read auth/login.cpp lines 40-80 to understand token handling'\n"
           "- Good: 'Edit auth/login.cpp to fix token refresh logic'\n"
           "- Good: 'Run tests for auth module to verify fix'\n"
           "- Bad: 'Fix the login bug' (too vague)\n"
           "- Bad: 'Explore the codebase' (not actionable)\n"
           "\n"
           "EXECUTION DISCIPLINE:\n"
           "- Execute ONE plan step per model turn when possible.\n"
           "- After each step, briefly narrate what you learned/changed and what's next (1-2 sentences).\n"
           "- If a step fails, diagnose in thinking, then adapt the plan - don't blindly retry.\n"
           "- Mark completed steps in the plan by outputting an updated plan with 'completed: true'.\n"
           "\n"
           "VERIFICATION REQUIREMENT:\n"
           "- After ANY file mutation, you MUST verify with a focused read, test, build, or lint.\n"
           "- Verification is not optional - it's part of the step.\n"
           "- If verification fails, thinking must analyze the failure and plan a targeted fix.\n"
           "\n"
           "COLLABORATION STYLE:\n"
           "- Your visible response should be conversational and useful to the user.\n"
           "- Narrate progress naturally: 'I'll check the auth module first, then apply the fix and run tests.'\n"
           "- Never expose tool names, JSON, or internal protocol in visible text.\n"
           "- When done, give a concise summary: what changed, how verified, any follow-ups.\n"
           "\n"
           "Workspace root: "_s
        + workspace;
}

QString compressText(const QString &text, int maxLength, bool enabled)
{
    if (!enabled || text.length() <= maxLength) {
        return text;
    }

    // Truncate and add indicator
    QString result = text.left(maxLength);
    result += u"... (truncated)"_s;
    return result;
}

// Smart context compression - preserves important parts while reducing size
QString smartCompressContext(const QString &text, int maxLength, bool enabled)
{
    if (!enabled || text.length() <= maxLength) {
        return text;
    }

    // Strategy: Keep first 30% (context/setup), last 50% (recent/important), summarize middle
    int keepStart = maxLength * 30 / 100;
    int keepEnd = maxLength * 50 / 100;
    int summaryBudget = maxLength - keepStart - keepEnd - 50; // 50 for summary marker
    
    if (summaryBudget < 100) {
        // Fall back to simple truncation if budget too small
        return compressText(text, maxLength, true);
    }

    QString start = text.left(keepStart);
    QString end = text.right(keepEnd);
    
    // Create a summary of what was in the middle
    QString middle = text.mid(keepStart, text.length() - keepStart - keepEnd);
    int middleLines = middle.count(u'\n');
    int middleChars = middle.length();
    
    QString summary = u"\n[... %1 lines, %2 chars compressed ...]\n"_s.arg(middleLines).arg(middleChars);
    
    return start + summary + end;
}

// Compress a list of messages intelligently
QList<ChatMessage> compressMessageHistory(const QList<ChatMessage> &messages,
                                           int maxMessages,
                                           int maxTotalChars,
                                           bool enabled)
{
    // maxMessages <= 0 means "do not trim". Falling through with a
    // non-positive keep count would return the system prompt and nothing else.
    if (!enabled || maxMessages <= 0 || messages.size() <= maxMessages) {
        return messages;
    }

    QList<ChatMessage> result;
    // Always keep system message
    if (!messages.isEmpty() && messages.first().role == ChatMessage::Role::System) {
        result.append(messages.first());
    }

    // Keep last N messages, but never drop every non-system message.
    int keepCount = qBound(1, maxMessages - result.size(), messages.size() - result.size());
    for (int i = messages.size() - keepCount; i < messages.size(); ++i) {
        result.append(messages[i]);
    }

    // If still over char budget, compress older messages
    int totalChars = 0;
    for (const auto &msg : result) {
        totalChars += msg.content.length() + msg.thinking.length();
    }

    if (totalChars > maxTotalChars && result.size() > 2) {
        // Compress the oldest non-system message
        for (int i = 1; i < result.size() - 1; ++i) {
            ChatMessage &msg = result[i];
            if (msg.content.length() > 500) {
                msg.content = smartCompressContext(msg.content, 500, true);
            }
            if (msg.thinking.length() > 1000) {
                msg.thinking = smartCompressContext(msg.thinking, 1000, true);
            }
        }
    }

    return result;
}

// Structured planning helpers ------------------------------------------------
QJsonArray parsePlanFromText(const QString &text)
{
    QJsonArray plan;
    const QString lower = text.toLower();
    int start = -1;
    const QStringList markers = QStringList() << u"plan:"_s << u"implementation plan:"_s << u"steps:"_s << u"action plan:"_s;
    for (const QString &m : markers) {
        start = lower.indexOf(m);
        if (start >= 0) {
            break;
        }
    }
    if (start < 0) {
        return plan;
    }
    int i = start;
    while (i < text.size() && text[i] != u'\n') {
        ++i;
    }
    ++i;
    int lineStart = i;
    while (i <= text.size() && plan.size() < 12) {
        if (i == text.size() || text[i] == u'\n') {
            const QString line = text.mid(lineStart, i - lineStart).trimmed();
            if (!line.isEmpty()) {
                QString step = line;
                int s = 0;
                while (s < step.size() && (step[s].isDigit() || step[s] == u'.' || step[s] == u')')) {
                    ++s;
                }
                if (s > 0 && s < step.size() && (step[s] == u' ' || step[s] == u'.')) {
                    step = step.mid(s).trimmed();
                } else if (step.startsWith(u"- "_s) || step.startsWith(u"* "_s)) {
                    step = step.mid(2);
                }
                if (!step.isEmpty()) {
                    QJsonObject obj;
                    obj.insert(u"id"_s, u"step%1"_s.arg(plan.size() + 1));
                    obj.insert(u"description"_s, step);
                    obj.insert(u"completed"_s, false);
                    obj.insert(u"inProgress"_s, false);
                    plan.append(obj);
                }
            }
            if (i < text.size()) {
                ++i;
            }
            lineStart = i;
        } else {
            ++i;
        }
    }
    return plan;
}

QJsonArray mergePlanIntoAssistantMessage(const QJsonArray &existingPlan,
                                         const QString &assistantText)
{
    const QJsonArray fresh = parsePlanFromText(assistantText);
    if (fresh.isEmpty()) {
        return existingPlan;
    }
    QHash<QString, bool> completedByDesc;
    for (const QJsonValue &v : existingPlan) {
        const QJsonObject o = v.toObject();
        completedByDesc.insert(o.value(u"description"_s).toString().toLower(),
                              o.value(u"completed"_s).toBool());
    }
    QJsonArray merged;
    for (const QJsonValue &v : fresh) {
        QJsonObject o = v.toObject();
        const QString desc = o.value(u"description"_s).toString();
        o.insert(u"completed"_s, completedByDesc.value(desc.toLower(), false));
        merged.append(o);
    }
    return merged;
}

QJsonArray markPlanStepCompleted(const QJsonArray &plan, const QString &stepId)
{
    QJsonArray out;
    for (const QJsonValue &v : plan) {
        QJsonObject o = v.toObject();
        if (o.value(u"id"_s).toString() == stepId) {
            o.insert(u"completed"_s, true);
            o.insert(u"inProgress"_s, false);
        }
        out.append(o);
    }
    return out;
}

bool planIsComplete(const QJsonArray &plan)
{
    for (const QJsonValue &v : plan) {
        if (!v.toObject().value(u"completed"_s).toBool()) {
            return false;
        }
    }
    return !plan.isEmpty();
}

QString apiFormatId(ApiFormat format)
{
    switch (format) {
    case ApiFormat::AnthropicCompatible:
        return u"anthropic-compatible"_s;
    case ApiFormat::AcpNative:
        return u"acp-native"_s;
    case ApiFormat::OpenAICompatible:
    default:
        return u"openai-compatible"_s;
    }
}

QString apiFormatLabel(ApiFormat format)
{
    switch (format) {
    case ApiFormat::AnthropicCompatible:
        return i18n("Anthropic/Claude Compatible");
    case ApiFormat::AcpNative:
        return i18n("ACP native (stdio JSON-RPC)");
    case ApiFormat::OpenAICompatible:
    default:
        return i18n("OpenAI Compatible");
    }
}

ApiFormat apiFormatFromId(const QString &id)
{
    if (id == u"anthropic-compatible"_s) {
        return ApiFormat::AnthropicCompatible;
    }
    if (id == u"acp-native"_s) {
        return ApiFormat::AcpNative;
    }
    return ApiFormat::OpenAICompatible;
}

QList<AcpAgentPreset> acpAgentPresets()
{
    return {
        {u"grok-build"_s, i18n("Grok Build"), u"grok"_s, u"agent stdio"_s, u"XAI_API_KEY"_s},
        {u"claude-acp"_s, i18n("Claude Agent"), u"npx"_s, u"-y @agentclientprotocol/claude-agent-acp"_s, u"ANTHROPIC_API_KEY"_s},
        {u"codex-acp"_s, i18n("Codex"), u"npx"_s, u"-y @agentclientprotocol/codex-acp"_s, u"OPENAI_API_KEY"_s},
        {u"gemini"_s, i18n("Gemini CLI"), u"gemini"_s, u"--acp"_s, u"GEMINI_API_KEY"_s},
        {u"github-copilot-cli"_s, i18n("GitHub Copilot"), u"copilot"_s, u"--acp"_s, {}},
        {u"goose"_s, i18n("goose"), u"goose"_s, u"acp"_s, {}},
        {u"opencode"_s, i18n("OpenCode"), u"opencode"_s, u"acp"_s, {}},
        {u"cline"_s, i18n("Cline"), u"npx"_s, u"-y cline --acp"_s, {}},
        {u"qwen-code"_s, i18n("Qwen Code"), u"npx"_s, u"-y @qwen-code/qwen-code --acp"_s, {}},
        {u"auggie"_s, i18n("Auggie CLI"), u"npx"_s, u"-y @augmentcode/auggie --acp"_s, {}},
        {u"cursor"_s, i18n("Cursor"), u"cursor-agent"_s, u"acp"_s, {}},
        {u"custom"_s, i18n("Custom"), {}, {}, {}},
    };
}

AcpAgentPreset acpAgentPreset(const QString &id)
{
    for (const AcpAgentPreset &preset : acpAgentPresets()) {
        if (preset.id == id) {
            return preset;
        }
    }
    return {u"custom"_s, i18n("Custom"), {}, {}, {}};
}

QString acpEffectiveCommand(const Settings &settings)
{
    const QString command = settings.acpCommand.trimmed();
    if (!command.isEmpty()) {
        return command;
    }
    return acpAgentPreset(settings.acpAgentId).command;
}

QString acpEffectiveArgs(const Settings &settings)
{
    const QString args = settings.acpArgs.trimmed();
    if (!args.isEmpty()) {
        return args;
    }
    return acpAgentPreset(settings.acpAgentId).args;
}

QString acpEffectiveApiKeyEnv(const Settings &settings)
{
    const QString env = settings.acpApiKeyEnv.trimmed();
    if (!env.isEmpty()) {
        return env;
    }
    return acpAgentPreset(settings.acpAgentId).apiKeyEnv;
}

bool acpAgentIsGrok(const Settings &settings)
{
    return QFileInfo(acpEffectiveCommand(settings)).fileName() == u"grok"_s;
}

QString acpAgentDisplayName(const Settings &settings)
{
    if (!usesAcpNative(settings)) {
        return i18n("ACP");
    }
    const QJsonArray installed = QJsonDocument::fromJson(settings.acpInstalledAgents.toUtf8()).array();
    for (const QJsonValue &value : installed) {
        const QJsonObject agent = value.toObject();
        if (agent.value(u"id"_s).toString() == settings.acpAgentId) {
            const QString name = agent.value(u"name"_s).toString().trimmed();
            if (!name.isEmpty()) return name;
        }
    }
    const AcpAgentPreset preset = acpAgentPreset(settings.acpAgentId);
    if (preset.id != u"custom"_s && !preset.label.isEmpty()) {
        return preset.label;
    }
    const QString command = acpEffectiveCommand(settings);
    if (!command.isEmpty()) {
        return QFileInfo(command).fileName();
    }
    return i18n("ACP agent");
}

QString acpAgentIdMatching(const QString &command, const QString &args)
{
    const QString trimmedCommand = command.trimmed();
    const QString trimmedArgs = args.trimmed();
    const QList<AcpAgentPreset> presets = acpAgentPresets();
    for (const AcpAgentPreset &preset : presets) {
        if (preset.id == u"custom"_s) {
            continue;
        }
        if (trimmedCommand == preset.command && trimmedArgs == preset.args) {
            return preset.id;
        }
    }
    if (trimmedCommand.isEmpty() || (trimmedCommand == u"grok"_s && (trimmedArgs.isEmpty() || trimmedArgs == u"agent stdio"_s))) {
        return u"grok-build"_s;
    }
    return u"custom"_s;
}

QStringList discoverAcpAgentModels(const QString &agentId, const QString &command)
{
    QStringList models;
    
    // Try agent-specific model discovery based on known agents
    if (agentId == u"grok-build"_s || QFileInfo(command).fileName() == u"grok"_s) {
        // Grok Build supports these models
        models = {u"grok-2"_s, u"grok-2-beta"_s, u"grok-2-mini"_s, u"grok-1.5"_s, u"grok-1.5-mini"_s};
    } else if (agentId == u"claude-acp"_s || command.contains(u"claude-agent-acp"_s)) {
        // Claude Agent supports these models
        models = {u"claude-3-5-sonnet-20250620"_s, u"claude-3-haiku-20240307"_s, u"claude-3-sonnet-20240229"_s, u"claude-3-opus-20240229"_s};
    } else if (agentId == u"codex-acp"_s || command.contains(u"codex-acp"_s)) {
        // Codex supports these models
        models = {u"codex-pro"_s, u"codex-plus"_s, u"codex"_s};
    } else if (agentId == u"gemini"_s || command.contains(u"gemini"_s)) {
        // Gemini CLI supports these models
        models = {u"gemini-2.0-pro-exp"_s, u"gemini-2.0-pro"_s, u"gemini-1.5-pro"_s, u"gemini-1.5-flash"_s};
    } else if (agentId == u"opencode"_s || command.contains(u"opencode"_s)) {
        // OpenCode supports various models
        models = {u"gpt-4o-mini"_s, u"gpt-4o"_s, u"claude-3-5-sonnet-20250620"_s, u"claude-3-haiku-20240307"_s};
    }
    
    return models;
}

QStringList acpAgentAvailableModels(const Settings &settings, const QString &agentId)
{
    QStringList models;
    
    // If we have stored agent models in settings, use them
    const QJsonDocument doc = QJsonDocument::fromJson(settings.acpAgentModels.toUtf8());
    if (doc.isObject()) {
        const QJsonObject agentModels = doc.object();
        const QString effectiveAgentId = agentId.isEmpty() ? settings.acpAgentId : agentId;
        const QJsonValue modelsValue = agentModels.value(effectiveAgentId);
        if (modelsValue.isArray()) {
            const QJsonArray modelsArray = modelsValue.toArray();
            for (const QJsonValue &modelValue : modelsArray) {
                const QString model = modelValue.toString();
                if (!model.isEmpty()) {
                    models.append(model);
                }
            }
        }
    }
    
    // If no models are stored, try to discover them from the agent
    if (models.isEmpty()) {
        const QString effectiveAgentId = agentId.isEmpty() ? settings.acpAgentId : agentId;
        const QString effectiveCommand = acpEffectiveCommand(settings);
        models = discoverAcpAgentModels(effectiveAgentId, effectiveCommand);
    }
    
    // Always include the currently configured model if it's not already there
    if (!settings.acpModel.isEmpty() && !models.contains(settings.acpModel)) {
        models.prepend(settings.acpModel);
    }
    
    return models;
}

} // namespace KateAi
