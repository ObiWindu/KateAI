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

#include <QHash>
#include <QObject>
#include <QSet>

class QProcess;

namespace KateAi
{

class ToolRunner : public QObject
{
    Q_OBJECT

public:
    ToolRunner(Sandbox sandbox, DocumentBridge *bridge, QObject *parent = nullptr);

    void setSandbox(Sandbox sandbox)
    {
        m_sandbox = std::move(sandbox);
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
    PermissionRequest describe(const ToolCall &call) const;

    // Runs `bash` without blocking the caller, so a long command streams its
    // output into the transcript while it runs instead of arriving as one lump
    // at the end. One command at a time per call id; `cancelBash` stops it.
    void runBashAsync(const ToolCall &call);
    void cancelBash(const QString &toolCallId);

Q_SIGNALS:
    void bashFinished(const ToolResult &result);
    // Partial output captured so far. Emitted as the process writes, and once
    // more when it exits so the live text is never left stale.
    void bashProgress(const QString &toolCallId, const QString &partialOutput);

private:
    ToolResult readFile(const QJsonObject &args) const;
    ToolResult writeFile(const QJsonObject &args);
    ToolResult editFile(const QJsonObject &args);
    ToolResult multiEditFile(const QJsonObject &args);
    ToolResult listDir(const QJsonObject &args) const;
    ToolResult grep(const QJsonObject &args) const;
    ToolResult glob(const QJsonObject &args) const;
    ToolResult bash(const QJsonObject &args);
    ToolResult queryProjectGraph(const QJsonObject &args) const;
    ToolResult goalReport(const QJsonObject &args);
    ToolResult writePlanFile(const QJsonObject &args);
    ToolResult suggest(const QJsonObject &args);
    // Defensive only: a question is answered by the UI, never by the runner.
    ToolResult question(const QJsonObject &args);
    // Shared tail of the sync and async bash paths: validates, wraps and
    // starts the process. Returns false and fills `error` when it cannot.
    bool startBashProcess(const QJsonObject &args, QString *program, QStringList *programArgs, QString *error) const;
    // Turns captured output plus an exit status into a tool result.
    ToolResult finishBashOutput(const QString &output, int exitCode, bool normalExit) const;
    ToolResult finishBash(QProcess &process) const;

    Sandbox m_sandbox;
    DocumentBridge *m_bridge = nullptr;
    ProjectGraph *m_projectGraph = nullptr;
    int m_timeoutMs = 60000;
    // Running async bash commands, keyed by tool call id, so progress and
    // cancellation can find the right process, plus the output captured from
    // each so far (the final result is the whole stream, not only the tail).
    QHash<QString, QProcess *> m_bashProcesses;
    QHash<QString, QString> m_bashAccumulated;
    // Call ids whose timeout timer fired. The kill only makes the process
    // report a failure; without this marker the finished result looks like an
    // ordinary crash and the model never learns the command was cut short.
    QSet<QString> m_bashTimedOut;
};

QString unifiedDiff(const QString &path, const QString &before, const QString &after);
QString shellCommandFor(const QString &toolName, const QJsonObject &args);

// Unified tool argument parsing - canonicalizes arguments from ToolCall
// Prefers pre-parsed `arguments` object, falls back to parsing `argumentsJson`
// Returns empty object on parse failure (error output parameter is set)
QJsonObject parseToolArgs(const ToolCall &call, QString *error = nullptr);

// Path of the implementation-plan file the agent writes and the user follows.
// It lives in the system temp directory — never in the workspace — so an
// agent planning a large change cannot litter the project, and the path is
// stable per workspace so repeated writes update the one file the user has
// open. Kate picks the path; the model only supplies the content, which is
// why the plan tool does not go through the sandbox.
QString planFilePath(const QString &workspaceRoot);

} // namespace KateAi
