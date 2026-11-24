/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "agentteam.h"

#include "modes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <optional>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

QString unquoteValue(const QString &value)
{
    QString v = value.trimmed();
    if (v.size() >= 2
        && ((v.startsWith(u'"') && v.endsWith(u'"'))
            || (v.startsWith(u'\'') && v.endsWith(u'\'')))) {
        v = v.mid(1, v.size() - 2);
    }
    return v.trimmed();
}

QStringList parseFrontmatterList(const QString &raw)
{
    QString value = raw.trimmed();
    if (value.isEmpty()) {
        return {};
    }
    if (value.startsWith(u'[') && value.endsWith(u']')) {
        value = value.mid(1, value.size() - 2);
    }
    QStringList out;
    const QStringList parts = value.contains(u'\n') ? value.split(u'\n', Qt::SkipEmptyParts)
                                                    : value.split(u',', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        QString item = part.trimmed();
        if (item.startsWith(u'-')) {
            item = item.mid(1).trimmed();
        }
        item = unquoteValue(item);
        if (!item.isEmpty() && !out.contains(item)) {
            out.append(item);
        }
    }
    return out;
}

QStringList stringListFromJson(const QJsonValue &value)
{
    QStringList out;
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) {
            const QString text = item.toString().trimmed();
            if (!text.isEmpty() && !out.contains(text)) {
                out.append(text);
            }
        }
    } else if (value.isString()) {
        out = parseFrontmatterList(value.toString());
    }
    return out;
}

QJsonArray jsonFromStringList(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values) {
        array.append(value);
    }
    return array;
}

} // namespace

QJsonObject AgentProfile::toJson() const
{
    QJsonObject object;
    object.insert(u"id"_s, id);
    object.insert(u"name"_s, name);
    object.insert(u"description"_s, description);
    object.insert(u"mode"_s, modeId);
    object.insert(u"enabled"_s, enabled);
    object.insert(u"model"_s, model);
    if (!effort.isEmpty()) {
        object.insert(u"effort"_s, effort);
    }
    object.insert(u"permissionMode"_s, permissionMode);
    object.insert(u"tools"_s, jsonFromStringList(tools));
    object.insert(u"disallowedTools"_s, jsonFromStringList(disallowedTools));
    object.insert(u"skills"_s, jsonFromStringList(skills));
    object.insert(u"maxTurns"_s, maxTurns);
    object.insert(u"isolation"_s, isolation);
    object.insert(u"memory"_s, memoryScope);
    return object;
}

AgentProfile AgentProfile::fromJson(const QJsonObject &object, const QString &fallbackId)
{
    AgentProfile profile;
    profile.id = object.value(u"id"_s).toString(fallbackId).trimmed();
    profile.name = object.value(u"name"_s).toString(profile.id).trimmed();
    profile.description = object.value(u"description"_s).toString().trimmed();
    profile.modeId = object.value(u"mode"_s).toString(QStringLiteral("code")).trimmed();
    profile.model = object.value(u"model"_s).toString(QStringLiteral("inherit")).trimmed();
    if (profile.model.isEmpty()) {
        profile.model = u"inherit"_s;
    }
    profile.effort = object.value(u"effort"_s).toString().trimmed();
    profile.permissionMode = object.value(u"permissionMode"_s).toString(QStringLiteral("inherit")).trimmed();
    profile.tools = stringListFromJson(object.value(u"tools"_s));
    profile.disallowedTools = stringListFromJson(object.value(u"disallowedTools"_s));
    profile.skills = stringListFromJson(object.value(u"skills"_s));
    profile.maxTurns = object.value(u"maxTurns"_s).toInt(0);
    profile.isolation = object.value(u"isolation"_s).toString(QStringLiteral("in-process")).trimmed();
    profile.memoryScope = object.value(u"memory"_s).toString(QStringLiteral("project")).trimmed();
    profile.enabled = object.value(u"enabled"_s).toBool(true);
    return profile;
}

QList<AgentProfile> AgentTeam::builtinAgents()
{
    auto make = [](const QString &id, const QString &name, const QString &mode, const QString &description) {
        AgentProfile profile;
        profile.id = id;
        profile.name = name;
        profile.modeId = mode;
        profile.description = description;
        profile.builtIn = true;
        profile.enabled = true;
        return profile;
    };

    return {
        make(u"scout"_s,
             u"Scout"_s,
             u"ask"_s,
             u"Read-only reconnaissance. Use it to answer 'where is X defined', 'what calls this', or to gather the "
             u"exact context another agent needs before it edits anything. Give it precise questions rather than tasks."_s),
        make(u"architect"_s,
             u"Architect"_s,
             u"architect"_s,
             u"Designs a solution and returns a concrete plan: files to touch, interfaces to change, ordering, and "
             u"risks. Use it before implementation, or when the approach is unclear."_s),
        make(u"coder"_s,
             u"Coder"_s,
             u"code"_s,
             u"Implements a change end to end and verifies it. Hand it a well-scoped task with the files and the "
             u"expected outcome already known."_s),
        make(u"debugger"_s,
             u"Debugger"_s,
             u"debug"_s,
             u"Reproduces a failure, isolates the root cause, and makes the smallest fix. Use it for bugs, crashes, "
             u"and failing tests rather than guessing at edits."_s),
        make(u"reviewer"_s,
             u"Reviewer"_s,
             u"ask"_s,
             u"Reviews a change for correctness, edge cases, and consistency with the surrounding code. Read-only; "
             u"use it to check another agent's work before accepting it."_s),
    };
}

AgentTeam::AgentTeam(QObject *parent)
    : QObject(parent)
{
}

void AgentTeam::setWorkspace(const QString &workspace)
{
    if (m_workspace == workspace) {
        return;
    }
    m_workspace = workspace;
    reload();
}

void AgentTeam::setCustomAgents(const QList<AgentProfile> &agents)
{
    m_custom = agents;
    Q_EMIT teamChanged();
}

void AgentTeam::reload()
{
    m_project.clear();
    if (!m_workspace.isEmpty()) {
        QDir dir(m_workspace + u"/.kateai/agents"_s);
        if (dir.exists()) {
            const QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
            for (const QFileInfo &info : entries) {
                bool ok = false;
                AgentProfile profile = parseAgentFile(info.absoluteFilePath(), &ok);
                if (ok && profile.isValid()) {
                    m_project.append(profile);
                }
            }
        }
    }
    Q_EMIT teamChanged();
}

QList<AgentProfile> AgentTeam::agents() const
{
    QList<AgentProfile> all;
    QSet<QString> used;

    auto append = [&all, &used](const AgentProfile &profile) {
        if (!profile.isValid() || !profile.enabled || used.contains(profile.id)) {
            return;
        }
        used.insert(profile.id);
        all.append(profile);
    };

    for (const AgentProfile &profile : builtinAgents()) {
        append(profile);
    }
    for (const AgentProfile &profile : m_custom) {
        append(profile);
    }
    for (const AgentProfile &profile : m_project) {
        append(profile);
    }
    return all;
}

std::optional<AgentProfile> AgentTeam::byIdIncludingDisabled(const QString &id) const
{
    if (id.isEmpty()) {
        return std::nullopt;
    }
    // The roster is built from three sources in priority order, and the first
    // one that claims an id wins - the same order agents() uses.
    const QList<QList<AgentProfile>> sources = {builtinAgents(), m_custom, m_project};
    for (const QList<AgentProfile> &source : sources) {
        for (const AgentProfile &profile : source) {
            if (!profile.isValid() || profile.id != id) {
                continue;
            }
            return profile;
        }
    }
    return std::nullopt;
}

std::optional<AgentProfile> AgentTeam::byId(const QString &id) const
{
    if (id.isEmpty()) {
        return std::nullopt;
    }
    // agents() builds a fresh list on every call, so the profile is returned by
    // value: a pointer into that temporary would dangle the moment we return.
    for (const AgentProfile &profile : agents()) {
        if (profile.id == id) {
            return profile;
        }
    }
    return std::nullopt;
}

AgentProfile AgentTeam::resolve(const QString &id) const
{
    if (const std::optional<AgentProfile> found = byId(id)) {
        return *found;
    }
    // An agent the user turned off is not in the enabled roster, so it is not
    // found above. Falling back to the generic code agent would hand it write
    // and command rights the roster deliberately withheld: naming a disabled
    // agent is enough to get them. It falls back to the read-only Ask mode
    // instead, so the subtask can still answer a question but cannot change
    // anything.
    if (const std::optional<AgentProfile> disabled = byIdIncludingDisabled(id)) {
        AgentProfile fallback;
        fallback.id = disabled->id;
        fallback.name = disabled->name.isEmpty() ? disabled->id : disabled->name;
        fallback.modeId = QStringLiteral("ask");
        fallback.description = u"Disabled agent '%1': it runs read-only and cannot change files or run commands."_s.arg(fallback.id);
        return fallback;
    }
    // Unknown ids fall back to a generic code agent rather than failing the
    // subtask; the orchestrator gets told what actually ran.
    AgentProfile fallback;
    fallback.id = id.isEmpty() ? QStringLiteral("agent") : id;
    fallback.name = fallback.id;
    fallback.modeId = QStringLiteral("code");
    fallback.description = u"Custom sub-agent."_s;
    return fallback;
}

QString AgentTeam::describeRoster() const
{
    QStringList lines;
    for (const AgentProfile &profile : agents()) {
        QString line = u"- %1 (mode: %2"_s.arg(profile.name, profile.modeId);
        if (!profile.model.isEmpty() && profile.model != u"inherit"_s) {
            line += u", model: "_s + profile.model;
        }
        if (!profile.effort.isEmpty()) {
            line += u", effort: "_s + profile.effort;
        }
        if (profile.isWorktreeIsolated()) {
            line += u", isolation: worktree"_s;
        }
        if (!profile.tools.isEmpty()) {
            line += u", tools: "_s + profile.tools.join(u", "_s);
        }
        if (!profile.disallowedTools.isEmpty()) {
            line += u", disallowed: "_s + profile.disallowedTools.join(u", "_s);
        }
        line += u"): "_s + profile.description;
        lines.append(line);
    }
    return lines.join(u'\n');
}

AgentProfile AgentTeam::parseAgentFile(const QString &path, bool *ok)
{
    if (ok) {
        *ok = false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    // Bound memory like modes (malicious workspace could OOM on setWorkspace).
    constexpr qint64 kMaxAgentFileBytes = 256 * 1024;
    if (file.size() > kMaxAgentFileBytes) {
        return {};
    }
    const QString document = QString::fromUtf8(file.read(kMaxAgentFileBytes + 1));
    if (document.size() > kMaxAgentFileBytes) {
        return {};
    }
    const QString fallbackId = QFileInfo(path).completeBaseName().toLower();
    bool parsed = false;
    AgentProfile profile = parseAgentDocument(document, fallbackId, &parsed);
    if (ok) {
        *ok = parsed;
    }
    return profile;
}

AgentProfile AgentTeam::parseAgentDocument(const QString &document, const QString &fallbackId, bool *ok)
{
    if (ok) {
        *ok = false;
    }
    const auto frontmatter = ModeRegistry::parseFrontmatter(document);

    AgentProfile profile;
    profile.id = ModeRegistry::slugify(frontmatter.keys.value(u"id"_s).trimmed().isEmpty() ? fallbackId
                                                                                              : frontmatter.keys.value(u"id"_s).trimmed());
    if (profile.id.isEmpty()) {
        return profile;
    }
    profile.name = frontmatter.keys.value(u"name"_s).trimmed();
    if (profile.name.isEmpty()) {
        profile.name = profile.id;
    }
    profile.description = frontmatter.keys.value(u"description"_s).trimmed();
    if (profile.description.isEmpty()) {
        // Fall back to the first line of the body so an agent file still tells
        // the orchestrator something useful.
        const QStringList bodyLines = frontmatter.body.split(u'\n', Qt::SkipEmptyParts);
        profile.description = bodyLines.isEmpty() ? QString() : bodyLines.first().trimmed();
    }

    const QString requestedMode = frontmatter.keys.value(u"mode"_s).trimmed();
    if (!requestedMode.isEmpty()) {
        profile.modeId = requestedMode;
    } else if (ModeRegistry::isBuiltInModeId(profile.id)) {
        // An agent named after a mode just runs that mode.
        profile.modeId = profile.id;
    } else {
        profile.modeId = u"code"_s;
    }

    QHash<QString, QString> keys;
    for (auto it = frontmatter.keys.constBegin(); it != frontmatter.keys.constEnd(); ++it) {
        keys.insert(it.key().trimmed().toLower().replace(u'-', u'_'), it.value().trimmed());
    }
    profile.model = unquoteValue(keys.value(u"model"_s));
    if (profile.model.isEmpty()) {
        profile.model = u"inherit"_s;
    }
    profile.effort = unquoteValue(keys.value(u"effort"_s));
    profile.permissionMode = unquoteValue(keys.value(u"permissionmode"_s,
                                                     keys.value(u"permission_mode"_s)));
    if (profile.permissionMode.isEmpty()) {
        profile.permissionMode = u"inherit"_s;
    }
    profile.tools = parseFrontmatterList(keys.value(u"tools"_s));
    profile.disallowedTools = parseFrontmatterList(keys.value(
        u"disallowedtools"_s, keys.value(u"disallowed_tools"_s)));
    profile.skills = parseFrontmatterList(keys.value(u"skills"_s));
    profile.maxTurns = keys.value(u"maxturns"_s, keys.value(u"max_turns"_s)).toInt();
    profile.isolation = unquoteValue(keys.value(u"isolation"_s));
    if (profile.isolation.isEmpty()) {
        profile.isolation = u"in-process"_s;
    }
    profile.memoryScope = unquoteValue(keys.value(u"memory"_s, keys.value(u"memory_scope"_s)));
    if (profile.memoryScope.isEmpty()) {
        profile.memoryScope = u"project"_s;
    }

    if (ok) {
        *ok = true;
    }
    return profile;
}

} // namespace KateAi