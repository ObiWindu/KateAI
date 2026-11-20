/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QDateTime>
#include <QJsonDocument>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace KateAi
{

enum class DebugSessionState {
    Unavailable,
    Idle,
    Running,
    Paused,
    Terminated,
};

struct DebugStopInfo {
    QString reason;        // e.g. "breakpoint", "step", "exception", "signal SIGSEGV"
    QString description;   // optional adapter description
    QString file;          // local path, if known
    int line = 0;          // 1-based, 0 = unknown
    int threadId = -1;
    QList<int> hitBreakpointIds;
    bool isCrashOrException = false;
};

struct DebugThread {
    int id = -1;
    QString name;
    QString state; // "running", "stopped", "unknown"
    bool active = false;
};

struct DebugStackFrame {
    int level = 0;       // 0 = innermost frame
    int id = -1;
    QString function;
    QString file;
    int line = 0;        // 1-based, 0 = unknown
    int column = 0;      // 1-based, 0 = unknown
    QString module;
    bool active = false;
};

struct DebugVariable {
    QString name;
    QString type;
    QString value;
    QString scope;       // e.g. "Locals", "Globals", "Registers"
    int variablesReference = 0;
    QList<DebugVariable> children;
};

struct DebugBreakpoint {
    int id = -1;
    QString kind;        // "line", "function", "exception"
    QString file;
    int line = 0;        // 1-based
    QString functionName;
    QString condition;
    QString hitCondition;
    QString logMessage;
    bool verified = true;
    bool enabled = true;
};

struct DebugTarget {
    QString name;
    QString adapter;     // e.g. "gdb", "lldb-dap", "python", "delve"
    QString profile;
    QString executable;
    QString workingDir;
    QString arguments;
    bool fromLaunchJson = false;
    bool active = false;
};

struct DebugActionResult {
    bool ok = false;
    QString output;
    DebugStopInfo stopInfo;
};

struct DebugSnapshot {
    QString label;
    QDateTime capturedAt;
    DebugStopInfo stopInfo;
    QList<DebugStackFrame> frames;
    QList<DebugVariable> locals;
    QString consoleTail;
};

/**
 * Abstract debugger surface seen by AgentLoop, ToolRunner, AcpClient, and
 * EditorAssist. The concrete Kate implementation (KateDebugBridge) talks to
 * Kate's DAP debugger plugin ("kategdbplugin") at runtime without linking
 * private Kate headers, and automatically upgrades to clean Q_INVOKABLE
 * JSON/QVariant methods when upstream Kate exposes them.
 */
class DebugProvider : public QObject
{
    Q_OBJECT

public:
    explicit DebugProvider(QObject *parent = nullptr);
    ~DebugProvider() override = default;

    static QString stateName(DebugSessionState state);
    static bool isCrashStopReason(const QString &reason, const QString &description = QString());

    virtual bool isAvailable() const = 0;
    virtual DebugSessionState sessionState() const = 0;
    virtual QString activeTarget() const = 0;
    virtual QList<DebugTarget> targets() const = 0;
    virtual DebugStopInfo currentStop() const = 0;
    virtual QList<DebugThread> threads() const = 0;
    virtual QList<DebugStackFrame> stackFrames(int threadId = -1) const = 0;
    virtual QList<DebugVariable> locals(int frameLevel = -1) const = 0;
    virtual QList<DebugVariable> expandVariable(int variablesReference, int timeoutMs = 3000)
    {
        Q_UNUSED(variablesReference);
        Q_UNUSED(timeoutMs);
        return {};
    }
    virtual QList<DebugBreakpoint> breakpoints() const = 0;
    virtual QString recentOutput(int maxChars = 4000) const = 0;

    // Non-mutating frame/thread selection for inspection.
    virtual DebugActionResult selectFrame(int frameLevel)
    {
        Q_UNUSED(frameLevel);
        return {false, QStringLiteral("Selecting stack frames is not supported by this debugger provider."), {}};
    }
    virtual DebugActionResult selectThread(int threadId)
    {
        Q_UNUSED(threadId);
        return {false, QStringLiteral("Selecting threads is not supported by this debugger provider."), {}};
    }

    // Breakpoint management.
    virtual DebugActionResult setBreakpoint(const QString &file,
                                            int line,
                                            const QString &condition = QString(),
                                            const QString &hitCondition = QString(),
                                            const QString &logMessage = QString(),
                                            bool remove = false)
    {
        Q_UNUSED(file);
        Q_UNUSED(line);
        Q_UNUSED(condition);
        Q_UNUSED(hitCondition);
        Q_UNUSED(logMessage);
        Q_UNUSED(remove);
        return {false, QStringLiteral("Setting breakpoints is not supported by this debugger provider."), {}};
    }
    virtual DebugActionResult clearAllBreakpoints()
    {
        return {false, QStringLiteral("Clearing breakpoints is not supported by this debugger provider."), {}};
    }

    // Execution control (action: continue, step_over, step_into, step_out,
    // run_to_cursor, pause).
    virtual DebugActionResult step(const QString &action,
                                   const QString &file = QString(),
                                   int line = 0,
                                   int timeoutMs = 15000)
    {
        Q_UNUSED(action);
        Q_UNUSED(file);
        Q_UNUSED(line);
        Q_UNUSED(timeoutMs);
        return {false, QStringLiteral("Stepping is not supported by this debugger provider."), {}};
    }

    // Expression evaluation in the active stack frame (may have side effects).
    virtual DebugActionResult evaluate(const QString &expression, int timeoutMs = 5000)
    {
        Q_UNUSED(expression);
        Q_UNUSED(timeoutMs);
        return {false, QStringLiteral("Expression evaluation is not supported by this debugger provider."), {}};
    }

    // Session lifecycle (action: start, restart, stop, hot_reload, hot_restart).
    virtual DebugActionResult controlSession(const QString &action,
                                             const QString &targetName = QString(),
                                             int timeoutMs = 15000)
    {
        Q_UNUSED(action);
        Q_UNUSED(targetName);
        Q_UNUSED(timeoutMs);
        return {false, QStringLiteral("Session control is not supported by this debugger provider."), {}};
    }

    // Raw DAP console REPL command (e.g. "whereami", "modules", "blist").
    virtual DebugActionResult sendConsoleCommand(const QString &command)
    {
        Q_UNUSED(command);
        return {false, QStringLiteral("Console commands are not supported by this debugger provider."), {}};
    }

    // Ask Kate's debugger ConfigView to reload .vscode/launch.json targets.
    virtual bool reloadLaunchJsonTargets()
    {
        return false;
    }

    // --- Debug state snapshots (for comparing "good" vs "bad" stops) --------
    DebugSnapshot captureSnapshot(const QString &label = QString());
    QList<DebugSnapshot> snapshots() const
    {
        return m_snapshots;
    }
    void clearSnapshots()
    {
        m_snapshots.clear();
    }
    static QString diffSnapshots(const DebugSnapshot &first, const DebugSnapshot &second);

    // Compact prompt-ready context summary injected into editorContext() while
    // a debug session is active/paused.
    QString contextSummary(int maxFrames = 8, int maxLocals = 24, int maxOutputChars = 1200) const;

    // Auto-generates a VS Code / Kate DAP compatible .vscode/launch.json by
    // inspecting the workspace (CMake/CMake File API, Cargo, Python, Node/TS, Go).
    static QJsonDocument generateLaunchConfig(const QString &workspace, QString *summary = nullptr);

Q_SIGNALS:
    void availabilityChanged();
    void stateChanged();
    void stopped(const KateAi::DebugStopInfo &info);
    void outputReceived(const QString &text);
    void breakpointsChanged();
    void localsUpdated();

private:
    QList<DebugSnapshot> m_snapshots;
};

} // namespace KateAi
