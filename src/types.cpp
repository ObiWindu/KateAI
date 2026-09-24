/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

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
    case Provider::OpenAICompatible:
        return u"openai-compatible"_s;
    case Provider::ClaudeCompatible:
        return u"claude-compatible"_s;
    case Provider::Kilo:
        return u"kilo"_s;
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
    case Provider::OpenAICompatible:
        return i18n("OpenAI Compatible");
    case Provider::ClaudeCompatible:
        return i18n("Claude Compatible");
    case Provider::Kilo:
        return i18n("Kilo.ai");
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
    if (id == u"openai-compatible"_s) {
        return Provider::OpenAICompatible;
    }
    if (id == u"claude-compatible"_s) {
        return Provider::ClaudeCompatible;
    }
    if (id == u"kilo"_s) {
        return Provider::Kilo;
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
    case Provider::OpenAICompatible:
        return u"http://localhost:11434/v1"_s;
    case Provider::ClaudeCompatible:
        return u"https://api.anthropic.com/v1"_s;
    case Provider::Kilo:
        return u"https://api.kilo.ai/v1"_s;
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
    case Provider::OpenAICompatible:
        return settings.openaiCompatibleUrl;
    case Provider::ClaudeCompatible:
        return settings.claudeCompatibleUrl;
    case Provider::Kilo:
        return u"https://api.kilo.ai/v1"_s;
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
    case Provider::OpenAICompatible:
        return {u"llama3"_s, u"mistral"_s};
    case Provider::ClaudeCompatible:
        return {u"claude-3-5-sonnet-20241022"_s, u"claude-3-opus-20240229"_s};
    case Provider::Kilo:
        return {u"kilo-code"_s, u"kilo-code-fast"_s};
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
    case Provider::OpenAICompatible:
        return settings.openaiCompatibleApiKey;
    case Provider::ClaudeCompatible:
        return settings.claudeCompatibleApiKey;
    case Provider::Kilo:
        return settings.kiloApiKey;
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
    case Provider::OpenAICompatible:
        return settings.openaiCompatibleModel;
    case Provider::ClaudeCompatible:
        return settings.claudeCompatibleModel;
    case Provider::Kilo:
        return settings.kiloModel;
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
                         u"Replace exact text in a file. old_string must match exactly, including whitespace. "
                         u"By default it must occur once; set replace_all to true to replace every occurrence."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"old_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Exact text to find."_s}}},
                             {u"new_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Replacement text."_s}}},
                             {u"replace_all"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, replace every occurrence instead of requiring a unique match."_s}}},
                         },
                         {u"path"_s, u"old_string"_s, u"new_string"_s}));

    tools.append(toolDef(u"list_dir"_s,
                         u"List files and directories in a folder."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Directory to list. Defaults to the workspace root."_s}}},
                         },
                         {}));

    tools.append(toolDef(u"grep"_s,
                         u"Search file contents with a regular expression. Returns path:line:content."_s,
                         QJsonObject{
                             {u"pattern"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Regular expression to search for."_s}}},
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File or directory to search. Defaults to the workspace."_s}}},
                             {u"glob"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional filename glob such as *.cpp."_s}}},
                             {u"case_insensitive"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, match without regard to case."_s}}},
                             {u"context"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Number of context lines to include before and after each match."_s}}},
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
                         u"Query the indexed project graph: files, symbols, imports, and relationships. "
                         u"Use this before blindly searching when you need structure or dependencies."_s,
                         QJsonObject{
                             {u"query_type"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"summary, nodes, edges, dependencies, dependents, find_related, find_path, dependency_chain"_s}}},
                             {u"node_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Node id, path, or name for node-scoped queries"_s}}},
                             {u"relationship"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional relationship filter: imports, calls, extends, contains, references"_s}}},
                             {u"source_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Source node for path finding"_s}}},
                             {u"target_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Target node for path finding"_s}}},
                         },
                         {}));

    if (!readOnlyOnly) {
        return tools;
    }

    QJsonArray readOnlyTools;
    for (const QJsonValue &tool : tools) {
        const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
        if (name == u"read_file"_s || name == u"list_dir"_s || name == u"grep"_s || name == u"glob"_s
            || name == u"query_project_graph"_s) {
            readOnlyTools.append(tool);
        }
    }
    return readOnlyTools;
}

QString defaultSystemPrompt(const QString &workspace)
{
    return u"You are Kate AI, a coding agent inside the Kate text editor.\n"
           "You solve the user's request by inspecting the workspace with tools, making focused edits, and verifying the result.\n"
           "\n"
           "How to work:\n"
           "- Start by locating the relevant code (query_project_graph, glob, grep, list_dir) before guessing paths.\n"
           "- Read only the files and ranges you need. Prefer offset/limit on large files.\n"
           "- Prefer edit_file for surgical changes. Use replace_all when the same unique snippet should change everywhere.\n"
           "- Use write_file only for new files or complete rewrites. Do not invent files outside the workspace.\n"
           "- Match existing style, naming, imports, and architecture. Do not add unrelated refactors or comments.\n"
           "- After edits, verify with a focused read, test, build, or lint. Treat tool output as evidence.\n"
           "- If a tool fails, diagnose the actual error and change approach; do not retry the identical call.\n"
           "- Prefer one purposeful batch of tools over speculative exploration.\n"
           "- Shell commands must be non-interactive. Do not request secrets or write credential files.\n"
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
    if (!enabled || messages.size() <= maxMessages) {
        return messages;
    }

    QList<ChatMessage> result;
    // Always keep system message
    if (!messages.isEmpty() && messages.first().role == ChatMessage::Role::System) {
        result.append(messages.first());
    }

    // Keep last N messages
    int keepCount = qMin(maxMessages - result.size(), messages.size() - result.size());
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

} // namespace KateAi
