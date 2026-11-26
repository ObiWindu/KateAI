/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>

#include <memory>

namespace KateAi
{

class EventStore;
class McpManager;
class ProjectGraph;

/**
 * Expensive harness state shared by every window on one workspace.
 *
 * A Kate window otherwise creates its own project graph, MCP manager and
 * SQLite event connection. The services object is acquired through
 * KateAiPlugin::workspaceServices() and kept alive by the views that use it;
 * the plugin only holds weak references, so closing the last window releases
 * the graph, stops the MCP servers and closes the event connection.
 *
 * The project's MCP server list is deliberately not loaded here: a workspace
 * can ship a server command, and starting it stays behind the per-workspace
 * confirmation in KateAiView.
 */
class WorkspaceServices
{
public:
    explicit WorkspaceServices(QString workspace);
    ~WorkspaceServices();

    WorkspaceServices(const WorkspaceServices &) = delete;
    WorkspaceServices &operator=(const WorkspaceServices &) = delete;

    const QString &workspace() const
    {
        return m_workspace;
    }
    const std::shared_ptr<ProjectGraph> &graph() const
    {
        return m_graph;
    }
    const std::shared_ptr<EventStore> &eventStore() const
    {
        return m_events;
    }
    McpManager *mcpManager() const
    {
        return m_mcp.get();
    }

private:
    QString m_workspace;
    std::shared_ptr<ProjectGraph> m_graph;
    std::shared_ptr<EventStore> m_events;
    std::unique_ptr<McpManager> m_mcp;
};

} // namespace KateAi
