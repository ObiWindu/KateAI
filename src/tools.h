/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "documentbridge.h"
#include "graph/projectgraph.h"
#include "permissions.h"
#include "sandbox.h"
#include "types.h"

#include <QObject>
#include <QByteArray>
#include <QProcess>
#include <QTimer>

namespace KateAi
{

class ToolRunner : public QObject
{
    Q_OBJECT

public:
    ToolRunner(Sandbox sandbox, DocumentBridge *bridge, QObject *parent = nullptr);
    ~ToolRunner() override;

    void setSandbox(Sandbox sandbox)
    {
        m_sandbox = std::move(sandbox);
    }
    void setDocumentBridge(DocumentBridge *bridge)
    {
        m_bridge = bridge;
    }
    void setTimeoutMs(int timeoutMs)
    {
        m_timeoutMs = timeoutMs;
    }
    void setProjectGraph(ProjectGraph *graph)
    {
        m_projectGraph = graph;
    }

    ToolResult run(const ToolCall &call);
    void runBashAsync(const ToolCall &call);
    void cancelAsyncBash();
    PermissionRequest describe(const ToolCall &call) const;

Q_SIGNALS:
    void bashFinished(const QString &toolCallId, const ToolResult &result);

private:
    ToolResult readFile(const QJsonObject &args) const;
    ToolResult writeFile(const QJsonObject &args);
    ToolResult editFile(const QJsonObject &args);
    ToolResult listDir(const QJsonObject &args) const;
    ToolResult grep(const QJsonObject &args) const;
    ToolResult glob(const QJsonObject &args) const;
    ToolResult bash(const QJsonObject &args);
    ToolResult queryProjectGraph(const QJsonObject &args) const;
    void finishAsyncBash(ToolResult result);

    Sandbox m_sandbox;
    DocumentBridge *m_bridge = nullptr;
    ProjectGraph *m_projectGraph = nullptr;
    int m_timeoutMs = 60000;
    QProcess m_bashProcess;
    QTimer m_bashTimeoutTimer;
    QByteArray m_bashOutput;
    ToolCall m_bashCall;
    bool m_bashRunning = false;
    bool m_bashTimedOut = false;
    bool m_bashOutputTruncated = false;
};

QString unifiedDiff(const QString &path, const QString &before, const QString &after);

} // namespace KateAi
