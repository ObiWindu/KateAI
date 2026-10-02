/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "modes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace KateAi
{

QString subtaskToolName()
{
    return u"new_task"_s;
}

QStringList builtInToolsForGroup(const QString &group)
{
    if (group == ToolGroup::read()) {
        return {u"read_file"_s, u"list_dir"_s, u"grep"_s, u"glob"_s, u"query_project_graph"_s};
    }
    if (group == ToolGroup::edit()) {
        return {u"write_file"_s, u"edit_file"_s, u"multi_edit_file"_s};
    }
    if (group == ToolGroup::command()) {
        return {u"bash"_s};
    }
    if (group == ToolGroup::web()) {
        return {u"web_search"_s, u"web_fetch"_s};
    }
    if (group == ToolGroup::orchestrate()) {
        return {subtaskToolName()};
    }
    // The "mcp" group is resolved by prefix at request time.
    return {};
}

QStringList allBuiltInToolNames()
{
    return {u"read_file"_s,
            u"write_file"_s,
            u"edit_file"_s,
            u"multi_edit_file"_s,
            u"list_dir"_s,
            u"grep"_s,
            u"glob"_s,
            u"bash"_s,
            u"web_search"_s,
            u"web_fetch"_s,
            u"query_project_graph"_s,
            subtaskToolName()};
}

ToolAccess ModeRegistry::toolAccessForGroups(const QStringList &groups)
{
    ToolAccess access;
    for (const QString &group : groups) {
        if (group == ToolGroup::mcp()) {
            access.allowPrefix(u"mcp__"_s);
            continue;
        }
        const QStringList names = builtInToolsForGroup(group);
        for (const QString &name : names) {
            access.allow(name);
        }
    }
    return access;
}

ToolAccess ModeRegistry::toolAccessFor(const ModeDefinition &mode, bool planMode)
{
    if (planMode) {
        // Plan mode is a hard read-only restriction regardless of mode.
        return toolAccessForGroups({ToolGroup::read()});
    }
    return toolAccessForGroups(mode.groups);
}

// --- frontmatter -------------------------------------------------------------

DocumentFrontmatter ModeRegistry::parseFrontmatter(const QString &document)
{
    DocumentFrontmatter out;
    static const QRegularExpression delimiter(QStringLiteral("\\A---\\s*\\r?\\n"), QRegularExpression::UseUnicodePropertiesOption);

    const QRegularExpressionMatch match = delimiter.match(document);
    if (!match.hasMatch()) {
        out.body = document;
        return out;
    }

    int pos = match.capturedLength();
    QStringList pendingKey;
    QString pendingBlock;
    bool inBlock = false;
    bool inFrontmatter = true;

    const QStringList lines = document.mid(pos).split(u'\n');
    int endLine = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        const QString trimmed = line.trimmed();
        if (trimmed == u"---"_s || trimmed == u"..."_s) {
            out.present = true;
            endLine = i;
            break;
        }
        if (!inFrontmatter) {
            continue;
        }

        if (inBlock) {
            // Block scalars continue while lines are indented or blank.
            if (!line.isEmpty() && !line.startsWith(u' ') && !line.startsWith(u'\t')) {
                out.keys.insert(pendingKey.join(u'\n'), pendingBlock);
                pendingKey.clear();
                pendingBlock.clear();
                inBlock = false;
            } else {
                pendingBlock += line.trimmed() + u'\n';
                continue;
            }
        }

        if (trimmed.isEmpty() || trimmed.startsWith(u'#')) {
            continue;
        }

        const int colon = line.indexOf(u':');
        if (colon <= 0) {
            continue;
        }
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (value == u"|"_s || value == u">"_s) {
            pendingKey = {key};
            pendingBlock.clear();
            inBlock = true;
            continue;
        }
        out.keys.insert(key, value);
    }
    if (inBlock && !pendingKey.isEmpty()) {
        out.keys.insert(pendingKey.join(u'\n'), pendingBlock);
    }
    if (!out.present) {
        out.body = document;
        out.keys.clear();
        return out;
    }
    out.body = lines.mid(endLine + 1).join(u'\n').trimmed();
    return out;
}

QString ModeRegistry::slugify(const QString &name)
{
    QString slug;
    bool lastDash = false;
    for (const QChar c : name) {
        if (c.isLetterOrNumber()) {
            slug.append(c.toLower());
            lastDash = false;
        } else if (!lastDash && !slug.isEmpty()) {
            slug.append(u'-');
            lastDash = true;
        }
    }
    while (slug.endsWith(u'-')) {
        slug.chop(1);
    }
    return slug;
}

bool ModeRegistry::isBuiltInModeId(const QString &id)
{
    return id == u"code"_s || id == u"ask"_s || id == u"architect"_s || id == u"debug"_s || id == u"orchestrator"_s;
}

namespace
{

QString unquote(const QString &value)
{
    QString v = value.trimmed();
    if (v.size() >= 2 && ((v.startsWith(u'"') && v.endsWith(u'"')) || (v.startsWith(u'\'') && v.endsWith(u'\'')))) {
        v = v.mid(1, v.size() - 2);
    }
    return v;
}

QStringList parseList(const QString &value)
{
    QString v = value.trimmed();
    if (v.startsWith(u'[') && v.endsWith(u']')) {
        v = v.mid(1, v.size() - 2);
        QStringList out;
        const QStringList parts = v.split(u',', Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QString item = unquote(part);
            if (!item.isEmpty()) {
                out.append(item);
            }
        }
        return out;
    }
    const QString single = unquote(v);
    return single.isEmpty() ? QStringList() : QStringList{single};
}

QStringList validGroups()
{
    return {ToolGroup::read(), ToolGroup::edit(), ToolGroup::command(), ToolGroup::mcp(), ToolGroup::web(), ToolGroup::orchestrate()};
}

} // namespace

// --- registry ----------------------------------------------------------------

ModeRegistry::ModeRegistry(QObject *parent)
    : QObject(parent)
{
    loadBuiltIns();
}

QList<ModeDefinition> ModeRegistry::builtInModes()
{
    ModeRegistry registry;
    return registry.m_modes;
}

void ModeRegistry::loadBuiltIns()
{
    m_modes.clear();

    ModeDefinition code;
    code.id = u"code"_s;
    code.name = u"Code"_s;
    code.icon = u"code"_s;
    code.description = u"Read, edit, and run commands. The default working mode."_s;
    code.builtIn = true;
    code.groups = {ToolGroup::read(), ToolGroup::edit(), ToolGroup::command(), ToolGroup::mcp(), ToolGroup::web(), ToolGroup::orchestrate()};
    code.roleDefinition = u"You are working in Code mode: implement the requested change directly. You may hand a "
                              u"well-scoped, self-contained piece of the work to a sub-agent with new_task when that is the "
                              u"clearer route, but you stay responsible for the result."_s;
    m_modes.append(code);

    ModeDefinition ask;
    ask.id = u"ask"_s;
    ask.name = u"Ask"_s;
    ask.icon = u"question"_s;
    ask.description = u"Answer questions about the codebase. No edits, no commands."_s;
    ask.builtIn = true;
    ask.readOnly = true;
    ask.groups = {ToolGroup::read(), ToolGroup::mcp(), ToolGroup::web()};
    ask.roleDefinition =
        u"You are working in Ask mode. Answer questions about this codebase from evidence you gather with tools. "
        "You cannot change files or run commands. If the user asks for an implementation, explain what it would take and "
        "suggest switching to Code mode. Cite concrete files and line numbers."_s;
    m_modes.append(ask);

    ModeDefinition architect;
    architect.id = u"architect"_s;
    architect.name = u"Architect"_s;
    architect.icon = u"blueprint"_s;
    architect.description = u"Plan and design a solution before any code is written."_s;
    architect.builtIn = true;
    architect.readOnly = true;
    architect.groups = {ToolGroup::read(), ToolGroup::mcp(), ToolGroup::web()};
    architect.roleDefinition =
        u"You are working in Architect mode. Design the solution, not the code. Inspect the codebase to understand the "
        "constraints, then produce a concrete plan: the files to touch, the interfaces to change, the ordering of the "
        "steps, and the risks. Do not edit files or run commands. State trade-offs and pick one option explicitly rather "
        "than listing every possibility."_s;
    m_modes.append(architect);

    ModeDefinition debug;
    debug.id = u"debug"_s;
    debug.name = u"Debug"_s;
    debug.icon = u"tools"_s;
    debug.description = u"Find the root cause of a failure and fix it."_s;
    debug.builtIn = true;
    debug.groups = {ToolGroup::read(), ToolGroup::edit(), ToolGroup::command(), ToolGroup::mcp(), ToolGroup::web(), ToolGroup::orchestrate()};
    debug.roleDefinition =
        u"You are working in Debug mode. Diagnose before you change anything: reproduce the failure or read the exact "
        "error, form a concrete hypothesis about the root cause, and prove it from the code or a run. Only then make the "
        "smallest repair, and re-run the failing case to confirm it is actually fixed. Never guess-fix."_s;
    m_modes.append(debug);

    ModeDefinition orchestrator;
    orchestrator.id = u"orchestrator"_s;
    orchestrator.name = u"Orchestrator"_s;
    orchestrator.icon = u"brainstorm"_s;
    orchestrator.description = u"Split work across sub-agents and integrate their results."_s;
    orchestrator.builtIn = true;
    orchestrator.readOnly = true;
    orchestrator.groups = {ToolGroup::read(), ToolGroup::mcp(), ToolGroup::web(), ToolGroup::orchestrate()};
    orchestrator.roleDefinition =
        u"You are working in Orchestrator mode. You coordinate; you do not implement. Break the user's request into "
        "independent, well-specified subtasks and hand each one to a sub-agent with new_task, choosing the mode that fits "
        "(Code for implementation, Architect for design, Debug for diagnosis, Ask for a focused question). Give each "
        "sub-agent enough context to work without seeing your conversation. Run independent subtasks in the same model "
        "turn so they execute together, then read every returned result before deciding the next step. You cannot edit "
        "files yourself; delegate all edits."_s;
    m_modes.append(orchestrator);
}

void ModeRegistry::setWorkspace(const QString &workspace)
{
    if (m_workspace == workspace) {
        return;
    }
    m_workspace = workspace;
    reload();
}

void ModeRegistry::reload()
{
    QList<ModeDefinition> previous;
    for (const ModeDefinition &mode : m_modes) {
        if (!mode.builtIn) {
            previous.append(mode);
        }
    }
    loadBuiltIns();
    for (const ModeDefinition &mode : previous) {
        m_modes.append(mode);
    }
    loadCustomModes();
    Q_EMIT modesChanged();
}

void ModeRegistry::loadCustomModes()
{
    if (m_workspace.isEmpty()) {
        return;
    }
    const QStringList dirs = {
        m_workspace + u"/.kateai/modes"_s,
        // Kilo Code / Roo Code compatible locations, so existing setups work.
        m_workspace + u"/.kilocodemodes"_s,
        m_workspace + u"/.roomodes"_s,
    };
    const QStringList known = validGroups();
    for (const QString &dirPath : dirs) {
        QDir dir(dirPath);
        if (!dir.exists()) {
            continue;
        }
        const QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.md"), QStringLiteral("*.markdown")}, QDir::Files, QDir::Name);
        for (const QFileInfo &info : entries) {
            bool ok = false;
            ModeDefinition mode = parseModeFile(info.absoluteFilePath(), &ok);
            if (!ok) {
                continue;
            }
            if (mode.id.isEmpty() || mode.id == u"code"_s || mode.id == u"ask"_s || mode.id == u"architect"_s
                || mode.id == u"debug"_s || mode.id == u"orchestrator"_s) {
                // A custom mode must not shadow a built-in one.
                mode.id = mode.id + u"-custom"_s;
            }
            mode.builtIn = false;
            if (mode.groups.isEmpty()) {
                mode.groups = {ToolGroup::read()};
            }
            // Drop unknown groups so a typo cannot silently widen access.
            mode.groups.erase(std::remove_if(mode.groups.begin(), mode.groups.end(), [&known](const QString &group) {
                                 return !known.contains(group);
                             }),
                              mode.groups.end());
            if (mode.readOnly) {
                const QStringList editGroups = {ToolGroup::edit(), ToolGroup::command(), ToolGroup::orchestrate()};
                for (const QString &group : mode.groups) {
                    if (editGroups.contains(group)) {
                        mode.readOnly = false;
                        break;
                    }
                }
            }
            m_modes.append(mode);
        }
    }
}

const ModeDefinition *ModeRegistry::modeById(const QString &id) const
{
    for (const ModeDefinition &mode : m_modes) {
        if (mode.id == id) {
            return &mode;
        }
    }
    return nullptr;
}

ModeDefinition ModeRegistry::modeOrDefault(const QString &id) const
{
    const ModeDefinition *mode = modeById(id);
    if (mode) {
        return *mode;
    }
    ModeDefinition fallback;
    fallback.id = u"code"_s;
    fallback.name = u"Code"_s;
    fallback.builtIn = true;
    fallback.groups = {ToolGroup::read(), ToolGroup::edit(), ToolGroup::command(), ToolGroup::mcp(), ToolGroup::web()};
    return fallback;
}

QStringList ModeRegistry::customModeIds() const
{
    QStringList ids;
    for (const ModeDefinition &mode : m_modes) {
        if (!mode.builtIn) {
            ids.append(mode.id);
        }
    }
    return ids;
}

ModeDefinition ModeRegistry::parseModeFile(const QString &path, bool *ok)
{
    if (ok) {
        *ok = false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QString document = QString::fromUtf8(file.readAll());
    const QString fallbackId = slugify(QFileInfo(path).completeBaseName());
    bool parsed = false;
    ModeDefinition mode = parseModeDocument(document, fallbackId, &parsed);
    if (ok) {
        *ok = parsed;
    }
    return mode;
}

ModeDefinition ModeRegistry::parseModeDocument(const QString &document, const QString &fallbackId, bool *ok)
{
    if (ok) {
        *ok = false;
    }
    const DocumentFrontmatter fm = parseFrontmatter(document);
    if (!fm.present) {
        return {};
    }

    ModeDefinition mode;
    mode.id = fm.keys.value(u"id"_s).trimmed();
    if (mode.id.isEmpty()) {
        mode.id = fallbackId;
    }
    if (mode.id.isEmpty()) {
        return {};
    }
    mode.id = slugify(mode.id);

    mode.name = unquote(fm.keys.value(u"name"_s).trimmed());
    if (mode.name.isEmpty()) {
        mode.name = mode.id;
    }
    mode.icon = unquote(fm.keys.value(u"icon"_s).trimmed());
    if (mode.icon.isEmpty()) {
        mode.icon = u"mode"_s;
    }
    mode.description = unquote(fm.keys.value(u"description"_s).trimmed());
    mode.roleDefinition = fm.keys.value(u"roleDefinition"_s).trimmed();
    mode.groups = parseList(fm.keys.value(u"groups"_s));

    const QString custom = fm.keys.value(u"customInstructions"_s).trimmed();
    if (!custom.isEmpty()) {
        mode.customInstructions.append(custom);
    }
    if (!fm.body.isEmpty()) {
        // Anything after the frontmatter counts as additional instructions,
        // which is how Kilo Code mode files carry their long-form prompt.
        mode.customInstructions.append(fm.body);
    }
    mode.builtIn = false;

    if (ok) {
        *ok = true;
    }
    return mode;
}

QString ModeRegistry::modeFileTemplate(const QString &id)
{
    return u"---\n"_s + u"id: "_s + id + u"\n"_s
        + u"name: "_s + id + u"\n"_s
        + u"description: What this mode is for\n"_s
        + u"icon: mode\n"_s
        + u"groups: [read, edit, command, mcp]\n"_s
        + u"roleDefinition: |\n"_s
        + u"  You are working in this mode. Describe the persona, the tools you\n"_s
        + u"  should reach for, and what a good result looks like.\n"_s
        + u"customInstructions: |\n"_s
        + u"  Any extra rules the model must follow in this mode.\n"_s
        + u"---\n"_s
        + u"\n"_s
        + u"Optional long-form instructions go here and are appended to the\n"_s
        + u"system prompt whenever this mode is active.\n"_s;
}

} // namespace KateAi