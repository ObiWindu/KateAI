/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "debug.h"
#include "documentbridge.h"
#include "ecosystem.h"
#include "graph/projectgraph.h"
#include "lsp.h"
#include "permissions.h"
#include "sandbox.h"
#include "types.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <atomic>
#include <memory>

class QProcess;
class QThread;

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
    void setTimeoutMs(int timeoutMs)
    {
        m_timeoutMs = timeoutMs;
    }
    void setProjectGraph(ProjectGraph *graph)
    {
        m_projectGraph = graph;
    }
    // The editor's language-server surface. Null means "no editor": the LSP
    // tools then report the feature as unavailable instead of failing.
    void setLspProvider(LspProvider *provider)
    {
        m_lsp = provider;
    }
    // The editor's live debugger surface (kategdbplugin / DAP). Null means
    // no debugger bridge is attached.
    void setDebugProvider(DebugProvider *provider)
    {
        m_debug = provider;
    }
    // Kate build/konsole/project/backtrace ecosystem surface.
    void setEcosystemProvider(EcosystemProvider *provider)
    {
        m_ecosystem = provider;
    }

    ToolResult run(const ToolCall &call);
    PermissionRequest describe(const ToolCall &call) const;

    // Forgets which files the agent has read this turn. Called when a turn
    // starts so a write_file conflict check never compares against a stale
    // read from an earlier turn.
    void resetReadLedger();

    // The structured todo list written by todo_write and read by todo_read.
    // Lives here (not in AgentLoop) so both the native path and the ACP client
    // method path share one store per turn.
    QJsonArray todos() const
    {
        return m_todos;
    }
    void setTodos(const QJsonArray &todos)
    {
        m_todos = todos;
    }

    // Runs `bash` without blocking the caller, so a long command streams its
    // output into the transcript while it runs instead of arriving as one lump
    // at the end. One command at a time per call id; `cancelBash` stops it.
    void runBashAsync(const ToolCall &call);
    void cancelBash(const QString &toolCallId);

    // Runs a read-only, disk-only tool (list_dir, grep, glob) on a worker
    // thread so a long search or listing does not freeze the transcript. The
    // worker uses a bridge that never touches KTextEditor documents: reading an
    // open buffer from a non-GUI thread is not safe. read_file stays
    // synchronous because it must see the editor's unsaved content. Emits
    // bashProgress once with the starting status and bashFinished with the
    // result, reusing the bash signals so the loop needs no extra plumbing.
    // Returns false when the tool is not async-safe and the caller must run it
    // synchronously.
    bool runReadToolAsync(const ToolCall &call);
    // True for the tools runReadToolAsync will offload (list_dir, grep, glob).
    // Lets the caller decide without starting the job twice.
    static bool isAsyncReadTool(const QString &toolName);
    // Asks every in-flight read job to stop at its next loop boundary. Called
    // on abort and before the worker thread is torn down.
    void cancelReadTools();

Q_SIGNALS:
    void bashFinished(const ToolResult &result);
    // Partial output captured so far. Emitted as the process writes, and once
    // more when it exits so the live text is never left stale.
    void bashProgress(const QString &toolCallId, const QString &partialOutput);

private:
    ToolResult readFile(const QJsonObject &args) const;
    ToolResult readToolResult(const QJsonObject &args);
    ToolResult writeFile(const QJsonObject &args);
    ToolResult editFile(const QJsonObject &args);
    ToolResult multiEditFile(const QJsonObject &args);
    ToolResult listDir(const QJsonObject &args) const;
    ToolResult grep(const QJsonObject &args) const;
    ToolResult glob(const QJsonObject &args) const;
    ToolResult bash(const QJsonObject &args);
    ToolResult queryProjectGraph(const QJsonObject &args) const;
    ToolResult lspDiagnostics(const QJsonObject &args) const;
    ToolResult lspSymbols(const QJsonObject &args) const;
    ToolResult lspHover(const QJsonObject &args) const;
    ToolResult lspDefinition(const QJsonObject &args) const;
    ToolResult lspReferences(const QJsonObject &args) const;
    ToolResult lspCodeActions(const QJsonObject &args) const;
    ToolResult debugStatus(const QJsonObject &args) const;
    ToolResult debugStack(const QJsonObject &args) const;
    ToolResult debugLocals(const QJsonObject &args) const;
    ToolResult debugBreakpoints(const QJsonObject &args) const;
    ToolResult debugSelectFrame(const QJsonObject &args);
    ToolResult debugSelectThread(const QJsonObject &args);
    ToolResult debugSnapshot(const QJsonObject &args);
    ToolResult debugSetBreakpoint(const QJsonObject &args);
    ToolResult debugStep(const QJsonObject &args);
    ToolResult debugEval(const QJsonObject &args);
    ToolResult debugSession(const QJsonObject &args);
    ToolResult debugConsole(const QJsonObject &args);
    ToolResult buildTargets(const QJsonObject &args) const;
    ToolResult buildRun(const QJsonObject &args);
    ToolResult terminalRun(const QJsonObject &args);
    ToolResult writePlanFile(const QJsonObject &args);
    ToolResult suggest(const QJsonObject &args);
    // Defensive only: a question is answered by the UI, never by the runner.
    ToolResult question(const QJsonObject &args);
    // Multi-file unified diff (apply_patch). Atomic: all files or none.
    ToolResult applyPatch(const QJsonObject &args);
    ToolResult todoWrite(const QJsonObject &args);
    ToolResult todoRead(const QJsonObject &args);
    ToolResult think(const QJsonObject &args);
    ToolResult exitPlanMode(const QJsonObject &args);
    // Shared tail of the sync and async bash paths: validates, wraps and
    // starts the process. Returns false and fills `error` when it cannot.
    bool startBashProcess(const QJsonObject &args, QString *program, QStringList *programArgs, QString *error) const;
    // Turns captured output plus an exit status into a tool result.
    ToolResult finishBashOutput(const QString &output, int exitCode, bool normalExit) const;

    Sandbox m_sandbox;
    DocumentBridge *m_bridge = nullptr;
    ProjectGraph *m_projectGraph = nullptr;
    LspProvider *m_lsp = nullptr;
    DebugProvider *m_debug = nullptr;
    EcosystemProvider *m_ecosystem = nullptr;
    int m_timeoutMs = 60000;
    // Running async bash commands, keyed by tool call id, so progress and
    // cancellation can find the right process, plus the output captured from
    // each so far (the final result is the whole stream, not only the tail).
    QHash<QString, QProcess *> m_bashProcesses;
    // The captured output of a running command. The head and the tail are
    // kept, the middle is dropped once the ceiling is hit: the tail carries
    // what the command last said, which is the half the model most needs.
    struct BashBuffer {
        QString head;
        QString tail;
        int droppedChars = 0;
        void append(const QString &chunk, int cap);
        QString view() const;
    };
    QHash<QString, BashBuffer> m_bashAccumulated;
    // Call ids whose timeout timer fired. The kill only makes the process
    // report a failure; without this marker the finished result looks like an
    // ordinary crash and the model never learns the command was cut short.
    QSet<QString> m_bashTimedOut;
    // The structured todo list (todo_write/todo_read), in the plan-step shape
    // {id, description, completed, inProgress} the TodoWidget renders.
    QJsonArray m_todos;
    // Cancellation flag for the worker-side runner. Only the worker reads it;
    // the GUI side sets it through the shared_ptr held in m_readJobs.
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    bool readCancelled() const;

    // Dedicated worker thread for runReadToolAsync. A per-runner thread (not
    // the global pool) so it is stopped in ~ToolRunner, before the
    // application tears down: a global-pool thread still alive at exit
    // crashes QApplication teardown in some KF6 builds.
    QThread *m_workerThread = nullptr;
    QObject *m_workerContext = nullptr;
    QList<std::shared_ptr<std::atomic_bool>> m_readJobs;
    // Full content of files read this turn, keyed by canonical path. write_file
    // compares against it before overwriting an existing file, so a change made
    // by the user or another process after the read is a conflict rather than
    // silently lost. Empty for files the agent never read: writing a new file
    // (or overwriting without a read) keeps the pre-0.6.1 behaviour.
    mutable QHash<QString, QString> m_readSnapshots;
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
