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

QJsonObject AgentProfile::toJson() const
{
    QJsonObject object;
    object.insert(u"id"_s, id);
    object.insert(u"name"_s, name);
    object.insert(u"description"_s, description);
    object.insert(u"mode"_s, modeId);
    object.insert(u"enabled"_s, enabled);
    return object;
}

AgentProfile AgentProfile::fromJson(const QJsonObject &object, const QString &fallbackId)
{
    AgentProfile profile;
    profile.id = object.value(u"id"_s).toString(fallbackId).trimmed();
    profile.name = object.value(u"name"_s).toString(profile.id).trimmed();
    profile.description = object.value(u"description"_s).toString().trimmed();
    profile.modeId = object.value(u"mode"_s).toString(QStringLiteral("code")).trimmed();
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
        lines.append(u"- %1 (mode: %2): %3"_s.arg(profile.name, profile.modeId, profile.description));
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
    const QString document = QString::fromUtf8(file.readAll());
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

    if (ok) {
        *ok = true;
    }
    return profile;
}

} // namespace KateAi