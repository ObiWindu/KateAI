/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "registry.h"

#include "../modes.h"
#include "tokens.h"

#include <QJsonDocument>
#include <QPair>
#include <QRegularExpression>

#include <algorithm>
#include <utility>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace
{

bool isEditTool(const QString &name)
{
    return name == u"write_file"_s || name == u"edit_file"_s || name == u"multi_edit_file"_s
        || name == u"apply_patch"_s || name == u"memory_write"_s;
}

bool isDebugControl(const QString &name)
{
    return name == u"debug_set_breakpoint"_s || name == u"debug_step"_s || name == u"debug_eval"_s
        || name == u"debug_session"_s || name == u"debug_console"_s;
}

} // namespace

QStringList tokenizeToolText(const QString &text)
{
    QStringList tokens;
    static const QRegularExpression word(u"[A-Za-z0-9]+"_s);
    auto it = word.globalMatch(text.toLower());
    while (it.hasNext()) {
        const QString token = it.next().captured(0);
        if (token.size() > 1 && !tokens.contains(token)) {
            tokens.append(token);
        }
    }
    return tokens;
}

QString ToolRegistry::builtinTitle(const QString &name)
{
    if (name == u"read_file"_s) return u"Read file"_s;
    if (name == u"read_tool_result"_s) return u"Read stored tool result"_s;
    if (name == u"write_file"_s) return u"Write file"_s;
    if (name == u"edit_file"_s) return u"Edit file"_s;
    if (name == u"multi_edit_file"_s) return u"Multi-edit file"_s;
    if (name == u"apply_patch"_s) return u"Apply patch"_s;
    if (name == u"memory_write"_s) return u"Write memory note"_s;
    if (name == u"list_dir"_s) return u"List directory"_s;
    if (name == u"grep"_s) return u"Search contents"_s;
    if (name == u"glob"_s) return u"Find files"_s;
    if (name == u"bash"_s) return u"Run shell command"_s;
    if (name == u"query_project_graph"_s) return u"Query project graph"_s;
    if (name == u"write_plan_file"_s) return u"Write plan file"_s;
    if (name == u"suggest"_s) return u"Suggest improvement"_s;
    if (name == u"question"_s) return u"Ask the user"_s;
    if (name == u"skill"_s) return u"Load skill"_s;
    if (name == u"todo_write"_s) return u"Update todo list"_s;
    if (name == u"todo_read"_s) return u"Read todo list"_s;
    if (name == u"think"_s) return u"Reason privately"_s;
    if (name == u"exit_plan_mode"_s) return u"Exit plan mode"_s;
    if (name == u"web_search"_s) return u"Web search"_s;
    if (name == u"web_fetch"_s) return u"Fetch web page"_s;
    if (name == u"build_targets"_s) return u"List build targets"_s;
    if (name == u"build_run"_s) return u"Run build"_s;
    if (name == u"terminal_run"_s) return u"Run in terminal"_s;
    if (name == u"subagent_transcript"_s) return u"Read sub-agent transcript"_s;
    if (name.startsWith(u"lsp_"_s)) return u"Language server"_s;
    if (name.startsWith(u"debug_"_s)) return u"Debugger"_s;
    if (name == subtaskToolName()) return u"Delegate to sub-agent"_s;
    return name;
}

QString ToolRegistry::builtinGroup(const QString &name)
{
    if (name == u"web_search"_s || name == u"web_fetch"_s) {
        return u"web"_s;
    }
    if (name == u"bash"_s || name == u"build_run"_s || name == u"terminal_run"_s) {
        return u"command"_s;
    }
    if (name.startsWith(u"debug_"_s)) {
        return u"debug"_s;
    }
    if (isEditTool(name) || name == u"write_plan_file"_s || name == u"todo_write"_s
        || name == u"exit_plan_mode"_s || name == u"suggest"_s || name == u"question"_s) {
        return u"edit"_s;
    }
    if (name == subtaskToolName()) {
        return u"orchestrate"_s;
    }
    if (name == u"subagent_transcript"_s) {
        return u"orchestrate"_s;
    }
    return u"read"_s;
}

ToolRisk ToolRegistry::builtinRisk(const QString &name)
{
    if (name == u"bash"_s || name == u"terminal_run"_s || name == u"build_run"_s
        || isDebugControl(name) || name == subtaskToolName()) {
        return ToolRisk::Execute;
    }
    if (isEditTool(name) || name == u"write_plan_file"_s || name == u"todo_write"_s) {
        return ToolRisk::Write;
    }
    return ToolRisk::Read;
}

ToolRegistry::ToolRegistry()
{
    loadBuiltins();
}

void ToolRegistry::clear()
{
    m_specs.clear();
    m_discovered.clear();
}

void ToolRegistry::loadBuiltins()
{
    const QJsonArray definitions = toolDefinitions(ToolAccess::unrestricted());
    const QStringList known = allBuiltInToolNames();
    for (const QJsonValue &value : definitions) {
        const QJsonObject definition = value.toObject();
        const QJsonObject function = definition.value(u"function"_s).toObject();
        const QString name = function.value(u"name"_s).toString();
        if (name.isEmpty()) {
            continue;
        }

        ToolSpec spec;
        spec.name = name;
        spec.title = builtinTitle(name);
        spec.description = function.value(u"description"_s).toString();
        spec.group = builtinGroup(name);
        spec.risk = builtinRisk(name);
        spec.schema = definition;
        spec.aliases = toolNameAliases(name);

        const bool edit = spec.risk == ToolRisk::Write;
        const bool execute = spec.risk == ToolRisk::Execute;
        spec.concurrencySafe = !edit && !execute;
        spec.idempotent = !edit && name != u"bash"_s && name != u"question"_s;
        spec.requiresPriorRead = name == u"write_file"_s || name == u"edit_file"_s
            || name == u"multi_edit_file"_s;

        // The always-loaded set is the small core a coding turn needs even when
        // search is on. Everything that can be discovered without danger stays
        // out of the initial schema payload.
        spec.alwaysLoaded = name == u"read_file"_s || name == u"write_file"_s
            || name == u"edit_file"_s || name == u"multi_edit_file"_s
            || name == u"apply_patch"_s || name == u"bash"_s || name == u"grep"_s
            || name == u"glob"_s || name == u"list_dir"_s || name == u"read_tool_result"_s
            || name == u"todo_write"_s || name == u"todo_read"_s || name == u"question"_s
            || name == u"think"_s || name == u"suggest"_s || name == u"memory_write"_s
            || name == u"skill"_s;
        spec.deferByDefault = name.startsWith(u"debug_"_s) || name.startsWith(u"lsp_"_s)
            || name == u"build_targets"_s || name == u"build_run"_s
            || name == u"terminal_run"_s || name == u"web_search"_s || name == u"web_fetch"_s
            || name == u"query_project_graph"_s || name == u"write_plan_file"_s
            || name == u"exit_plan_mode"_s || name == subtaskToolName();

        if (name == u"bash"_s) {
            spec.maxResultChars = 48000;
        } else if (name == u"read_file"_s) {
            spec.maxResultChars = 40000;
        } else if (name == u"skill"_s) {
            spec.maxResultChars = 48000;
        } else if (name == u"subagent_transcript"_s) {
            spec.maxResultChars = 48000;
        } else if (name == u"grep"_s) {
            spec.maxResultChars = 32000;
        } else if (name == u"glob"_s || name == u"list_dir"_s) {
            spec.maxResultChars = 16000;
        } else if (name == u"read_tool_result"_s) {
            spec.maxResultChars = 80000;
        } else if (execute) {
            spec.maxResultChars = 48000;
        }

        if (!known.contains(name)) {
            // A definition added after the metadata table still gets a spec;
            // the report says which ones lack curated metadata.
            spec.group = u"read"_s;
        }
        m_specs.append(spec);
    }
}

void ToolRegistry::addSpec(const ToolSpec &spec)
{
    if (spec.name.isEmpty()) {
        return;
    }
    for (ToolSpec &existing : m_specs) {
        if (existing.name == spec.name) {
            existing = spec;
            return;
        }
    }
    m_specs.append(spec);
}

void ToolRegistry::removeSpec(const QString &name)
{
    m_specs.erase(std::remove_if(m_specs.begin(), m_specs.end(),
                                 [&name](const ToolSpec &spec) { return spec.name == name; }),
                  m_specs.end());
    m_discovered.remove(name);
}

ToolSpec ToolRegistry::spec(const QString &name) const
{
    for (const ToolSpec &spec : m_specs) {
        if (spec.name == name) {
            return spec;
        }
    }
    return {};
}

bool ToolRegistry::contains(const QString &name) const
{
    for (const ToolSpec &spec : m_specs) {
        if (spec.name == name) {
            return true;
        }
    }
    return false;
}

void ToolRegistry::addMcpDefinition(const QJsonObject &definition)
{
    const QJsonObject function = definition.value(u"function"_s).toObject();
    const QString name = function.value(u"name"_s).toString();
    if (name.isEmpty()) {
        return;
    }
    ToolSpec spec;
    spec.name = name;
    const int separator = name.lastIndexOf(u"__"_s);
    spec.title = separator >= 0 ? name.mid(separator + 2) : name;
    spec.description = function.value(u"description"_s).toString();
    spec.group = u"mcp"_s;
    spec.risk = ToolRisk::Execute;
    spec.concurrencySafe = false;
    spec.idempotent = false;
    // A server tool may change external state; it is not safe to run in
    // parallel with another MCP call without the server's own guarantees.
    spec.alwaysLoaded = false;
    spec.deferByDefault = true;
    spec.maxResultChars = 30000;
    spec.schema = definition;
    spec.mcp = true;
    addSpec(spec);
}

QJsonArray ToolRegistry::definitions(const ToolAccess &access, bool includeDeferred) const
{
    QJsonArray tools;
    for (const ToolSpec &spec : m_specs) {
        if (!access.isEmpty() && !access.allows(spec.name)) {
            continue;
        }
        if (!includeDeferred && spec.deferByDefault && !spec.alwaysLoaded
            && !m_discovered.contains(spec.name)) {
            continue;
        }
        tools.append(spec.schema);
    }
    return tools;
}

int ToolRegistry::schemaTokens(const QJsonArray &definitions) const
{
    return Tokens::estimate(QString::fromUtf8(
        QJsonDocument(definitions).toJson(QJsonDocument::Compact)));
}

AdvertisedTools ToolRegistry::advertised(const ToolAccess &access, int contextWindow,
                                         bool searchEnabled, int toolCountThreshold) const
{
    AdvertisedTools out;
    QList<ToolSpec> accessible;
    for (const ToolSpec &spec : m_specs) {
        if (access.isEmpty() || access.allows(spec.name)) {
            accessible.append(spec);
        }
    }

    int totalTokens = 0;
    for (const ToolSpec &spec : std::as_const(accessible)) {
        totalTokens += schemaTokens(QJsonArray{spec.schema});
    }

    const bool force = accessible.size() > qMax(1, toolCountThreshold)
        || totalTokens > qMax(1024, contextWindow / 10);

    if (!searchEnabled || !force) {
        for (const ToolSpec &spec : std::as_const(accessible)) {
            if (searchEnabled && spec.mcp) {
                // MCP catalogs are the expensive case and always defer when
                // the search bridge is available. Built-ins stay advertised
                // until the count/window threshold is crossed.
                out.deferred.append(spec.name);
            } else {
                out.tools.append(spec.schema);
            }
        }
        out.deferralActive = searchEnabled && !out.deferred.isEmpty();
        if (out.deferralActive) {
            out.tools.append(searchDefinition());
            out.tools.append(describeDefinition());
            out.tools.append(callDefinition());
        }
        out.schemaTokens = schemaTokens(out.tools);
        return out;
    }

    for (const ToolSpec &spec : std::as_const(accessible)) {
        if (spec.alwaysLoaded || m_discovered.contains(spec.name)) {
            out.tools.append(spec.schema);
        } else {
            out.deferred.append(spec.name);
        }
    }
    out.deferralActive = true;
    out.tools.append(searchDefinition());
    out.tools.append(describeDefinition());
    out.tools.append(callDefinition());
    out.schemaTokens = schemaTokens(out.tools);
    return out;
}

void ToolRegistry::markDiscovered(const QString &name)
{
    if (contains(name)) {
        m_discovered.insert(name);
    }
}

QStringList ToolRegistry::search(const QString &query, const ToolAccess &access, int limit) const
{
    const QStringList queryTerms = tokenizeToolText(query);
    if (queryTerms.isEmpty()) {
        return {};
    }

    struct Document {
        const ToolSpec *spec = nullptr;
        QHash<QString, int> frequencies;
        int length = 0;
    };

    QList<Document> documents;
    QHash<QString, int> documentFrequency;
    double totalLength = 0.0;
    for (const ToolSpec &spec : m_specs) {
        if (!access.isEmpty() && !access.allows(spec.name)) {
            continue;
        }
        Document document;
        document.spec = &spec;
        QString haystack = spec.name + u' ' + spec.title + u' ' + spec.description + u' '
            + spec.aliases.join(u' ');
        for (const QString &term : tokenizeToolText(haystack)) {
            document.frequencies[term] += 1;
            document.length += 1;
        }
        for (auto it = document.frequencies.constBegin(); it != document.frequencies.constEnd(); ++it) {
            documentFrequency[it.key()] += 1;
        }
        totalLength += document.length;
        documents.append(document);
    }
    if (documents.isEmpty()) {
        return {};
    }

    constexpr double k1 = 1.2;
    constexpr double b = 0.75;
    const double averageLength = qMax(1.0, totalLength / documents.size());
    const int count = documents.size();

    QList<QPair<double, QString>> scored;
    for (const Document &document : std::as_const(documents)) {
        double score = 0.0;
        for (const QString &term : queryTerms) {
            const auto frequency = document.frequencies.constFind(term);
            if (frequency == document.frequencies.constEnd()) {
                continue;
            }
            const int df = documentFrequency.value(term, 0);
            const double idf = std::log(1.0 + (count - df + 0.5) / (df + 0.5));
            const double tf = frequency.value();
            score += idf * (tf * (k1 + 1.0))
                / (tf + k1 * (1.0 - b + b * document.length / averageLength));
        }
        // An exact name or alias hit is what the model usually means; BM25 can
        // otherwise rank a wordy description above it.
        for (const QString &term : queryTerms) {
            if (document.spec->name.compare(term, Qt::CaseInsensitive) == 0) {
                score += 8.0;
            }
            const QStringList nameTokens = tokenizeToolText(document.spec->name);
            if (nameTokens.contains(term)) {
                score += 2.5;
            }
            for (const QString &alias : document.spec->aliases) {
                if (alias.compare(term, Qt::CaseInsensitive) == 0) {
                    score += 4.0;
                }
            }
        }
        if (score > 0.0) {
            scored.append({score, document.spec->name});
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return a.second < b.second;
    });
    QStringList names;
    for (const auto &entry : std::as_const(scored)) {
        if (names.size() >= qMax(1, limit)) {
            break;
        }
        names.append(entry.second);
    }
    return names;
}

int ToolRegistry::resultBudgetFor(const QString &name) const
{
    const ToolSpec spec = this->spec(name);
    return spec.maxResultChars > 0 ? spec.maxResultChars : 20000;
}

QJsonObject ToolRegistry::searchDefinition()
{
    return QJsonObject{
        {u"type"_s, u"function"_s},
        {u"function"_s,
         QJsonObject{
             {u"name"_s, u"tool_search"_s},
             {u"description"_s,
              u"Search the deferred tool catalog by intent. Use this when a needed capability (debugger, web, "
              u"project graph, sub-agent, MCP server) is not in the advertised tools. Returns names and short "
              u"descriptions; call tool_describe for the full schema, then tool_call to invoke it."_s},
             {u"parameters"_s,
              QJsonObject{
                  {u"type"_s, u"object"_s},
                  {u"properties"_s,
                   QJsonObject{
                       {u"query"_s, QJsonObject{{u"type"_s, u"string"_s},
                                                {u"description"_s, u"What you need to do, in a few words."_s}}},
                       {u"limit"_s, QJsonObject{{u"type"_s, u"integer"_s},
                                                {u"description"_s, u"Maximum results (default 8)."_s}}},
                   }},
                  {u"required"_s, QJsonArray{u"query"_s}},
              }},
         }},
    };
}

QJsonObject ToolRegistry::describeDefinition()
{
    return QJsonObject{
        {u"type"_s, u"function"_s},
        {u"function"_s,
         QJsonObject{
             {u"name"_s, u"tool_describe"_s},
             {u"description"_s, u"Return the full JSON schema of one tool, including deferred tools. Read-only."_s},
             {u"parameters"_s,
              QJsonObject{
                  {u"type"_s, u"object"_s},
                  {u"properties"_s,
                   QJsonObject{
                       {u"name"_s, QJsonObject{{u"type"_s, u"string"_s},
                                               {u"description"_s, u"Tool name from tool_search."_s}}},
                   }},
                  {u"required"_s, QJsonArray{u"name"_s}},
              }},
         }},
    };
}

QJsonObject ToolRegistry::callDefinition()
{
    return QJsonObject{
        {u"type"_s, u"function"_s},
        {u"function"_s,
         QJsonObject{
             {u"name"_s, u"tool_call"_s},
             {u"description"_s,
              u"Invoke a deferred tool by name with its normal arguments. The target's own permission policy, "
              u"result budget, and mode access still apply. Use this only after tool_search/tool_describe."_s},
             {u"parameters"_s,
              QJsonObject{
                  {u"type"_s, u"object"_s},
                  {u"properties"_s,
                   QJsonObject{
                       {u"name"_s, QJsonObject{{u"type"_s, u"string"_s},
                                               {u"description"_s, u"Target tool name."_s}}},
                       {u"arguments"_s, QJsonObject{{u"type"_s, u"object"_s},
                                                     {u"description"_s, u"Arguments for the target tool."_s}}},
                   }},
                  {u"required"_s, QJsonArray{u"name"_s, u"arguments"_s}},
              }},
         }},
    };
}

} // namespace KateAi
