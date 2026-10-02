/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

namespace KateAi
{

// Tool groups, mirroring the groups Kilo Code exposes for mode configuration.
namespace ToolGroup
{
inline QString read()
{
    return u"read"_s;
}
inline QString edit()
{
    return u"edit"_s;
}
inline QString command()
{
    return u"command"_s;
}
inline QString mcp()
{
    return u"mcp"_s;
}
inline QString orchestrate()
{
    return u"orchestrate"_s;
}
// Read-only tools that reach outside the workspace.
inline QString web()
{
    return u"web"_s;
}
} // namespace ToolGroup

// Tool names exposed to the model for a given mode. MCP tools are dynamic, so
// they are matched by prefix instead of by exact name; see KateAi::ToolAccess.
struct ModeDefinition {
    QString id;
    QString name;
    QString icon;
    QString description;
    // Persona text prepended to the system prompt when the mode is active.
    QString roleDefinition;
    // Extra instructions appended to the system prompt.
    QStringList customInstructions;
    // Tool group ids; unknown groups are ignored.
    QStringList groups;
    bool builtIn = false;
    // True when the mode must never mutate the workspace (Ask, Architect).
    bool readOnly = false;
};

// Built-in tool names belonging to a group. The "mcp" group is dynamic and
// returns no fixed names.
QStringList builtInToolsForGroup(const QString &group);
// Every tool Kate AI can expose, in a stable order.
QStringList allBuiltInToolNames();
// Subtask/boomerang tool name.
QString subtaskToolName();

// Parsed frontmatter of a mode or agent document.
struct DocumentFrontmatter {
    bool present = false;
    QHash<QString, QString> keys;
    QString body;
};

class ModeRegistry : public QObject
{
    Q_OBJECT

public:
    explicit ModeRegistry(QObject *parent = nullptr);

        // The five built-in modes, without needing an instance. Custom modes are
        // only known once a workspace is known.
        static QList<ModeDefinition> builtInModes();

    // Custom modes live in <workspace>/.kateai/modes/*.md. Changing the
    // workspace reloads the custom modes.
    void setWorkspace(const QString &workspace);
    QString workspace() const
    {
        return m_workspace;
    }
    void reload();

    QList<ModeDefinition> modes() const
    {
        return m_modes;
    }
    const ModeDefinition *modeById(const QString &id) const;
    // Returns the named mode, or the Code mode when it does not exist.
    ModeDefinition modeOrDefault(const QString &id) const;
    QStringList customModeIds() const;

    // Tool access for a mode. Plan mode always narrows the result to the
    // read-only group, matching the pre-existing plan-mode behaviour.
    static ToolAccess toolAccessFor(const ModeDefinition &mode, bool planMode);
    // Tool access built from group ids alone.
    static ToolAccess toolAccessForGroups(const QStringList &groups);

    // Parses a mode definition from a markdown file with a frontmatter block.
    // The slug is derived from the file name when frontmatter has no "id".
    static ModeDefinition parseModeFile(const QString &path, bool *ok = nullptr);
    // Parses the frontmatter + body of a mode document.
    static ModeDefinition parseModeDocument(const QString &document, const QString &fallbackId, bool *ok = nullptr);
    // A commented starting point users can copy into .kateai/modes/.
    static QString modeFileTemplate(const QString &id);

    // Minimal YAML subset: "key: value", "key: [a, b]" and "key: |" block
    // scalars. Shared with the agent roster, which uses the same file shape.
    static DocumentFrontmatter parseFrontmatter(const QString &document);
    // Lower-cased, dash-separated identifier derived from a display name.
    static QString slugify(const QString &name);
    // True for the built-in mode ids (code, ask, architect, debug, orchestrator).
    static bool isBuiltInModeId(const QString &id);

Q_SIGNALS:
    void modesChanged();

private:
    void loadBuiltIns();
    void loadCustomModes();

    QString m_workspace;
    QList<ModeDefinition> m_modes;
};

} // namespace KateAi