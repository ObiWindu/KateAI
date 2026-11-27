/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "workspaceservices.h"

#include "graph/projectgraph.h"
#include "harness/eventstore.h"
#include "mcp.h"

#include <QDir>
#include <QFile>

#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

WorkspaceServices::WorkspaceServices(QString workspace, QNetworkAccessManager *network)
    : m_workspace(std::move(workspace))
{
    m_graph = std::make_shared<ProjectGraph>();
    m_graph->setWorkspacePath(m_workspace);

    const QString metadataDir = m_workspace + u"/.kateai"_s;
    const QString graphPath = metadataDir + u"/project_graph.json"_s;
    bool loaded = false;
    if (QFile::exists(graphPath) && m_graph->loadFromFile(graphPath)) {
        loaded = m_graph->staleFileCount() <= 0;
    }
    if (!loaded) {
        QDir().mkpath(metadataDir);
        m_graph->generateGraph(m_workspace);
        m_graph->saveToFile(graphPath);
    }

    m_events = std::make_shared<EventStore>();

    m_mcp = std::make_unique<McpManager>();
    if (network) {
        m_mcp->setNetworkManager(network);
    }
    // Record the workspace without loading its server list. The view calls
    // setLoadWorkspaceConfig(true) + reloadFromDisk() only after the user has
    // seen the commands the project wants to run.
    m_mcp->setLoadWorkspaceConfig(false);
    m_mcp->setWorkspace(m_workspace);
}

WorkspaceServices::~WorkspaceServices() = default;

} // namespace KateAi
