/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace KateAi
{

// A named role a sub-agent can be spawned as. The description is what the
// orchestrating model reads when it decides who to delegate to, so it needs to
// say what the agent is good at rather than what it is called.
struct AgentProfile {
    QString id;
    QString name;
    QString description;
    // The mode the sub-agent runs in. Any mode id, including custom ones.
    QString modeId;
    bool enabled = true;
    bool builtIn = false;

    QJsonObject toJson() const;
    static AgentProfile fromJson(const QJsonObject &object, const QString &fallbackId = QString());
    // True when the object has enough to spawn with.
    bool isValid() const
    {
        return !id.trimmed().isEmpty();
    }
};

// The roster of agents available to the orchestrator: the built-in roles plus
// whatever the project or the user defined.
class AgentTeam : public QObject
{
    Q_OBJECT

public:
    explicit AgentTeam(QObject *parent = nullptr);

    // Custom agents come from <workspace>/.kateai/agents/*.md and from the
    // user roster in settings. Changing the workspace reloads the project ones.
    void setWorkspace(const QString &workspace);
    QString workspace() const
    {
        return m_workspace;
    }
    // User-defined agents, replacing the previous custom roster.
    void setCustomAgents(const QList<AgentProfile> &agents);
    QList<AgentProfile> customAgents() const
    {
        return m_custom;
    }
    void reload();

    // Enabled agents, built-ins first.
    QList<AgentProfile> agents() const;
    // The enabled agent with this id, if any. Returned by value because the
    // roster is rebuilt per call and a pointer into it would not outlive us.
    std::optional<AgentProfile> byId(const QString &id) const;
    // Resolves a requested agent id to a profile, falling back to the Code
    // mode with the id as its name.
    AgentProfile resolve(const QString &id) const;
    // Ids and names for the orchestrator's system prompt.
    QString describeRoster() const;

    static QList<AgentProfile> builtinAgents();
    static AgentProfile parseAgentFile(const QString &path, bool *ok = nullptr);
    static AgentProfile parseAgentDocument(const QString &document, const QString &fallbackId, bool *ok = nullptr);

Q_SIGNALS:
    void teamChanged();

private:
    QString m_workspace;
    QList<AgentProfile> m_custom; // from settings
    QList<AgentProfile> m_project; // from the workspace
};

} // namespace KateAi