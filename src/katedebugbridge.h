/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "debug.h"

#include <QPointer>
#include <QString>
#include <QUrl>

class QAbstractItemModel;
class QAction;
class QComboBox;
class QTextEdit;
class QTimer;
class QTreeView;
class QTreeWidget;
class QWidget;

namespace KTextEditor
{
class MainWindow;
}

namespace KateAi
{

/**
 * Concrete DebugProvider bridging Kate's DAP debugger plugin ("kategdbplugin").
 *
 * Integration tiers:
 *  - Tier 2 (auto-detected): if KatePluginGDBView exposes Q_INVOKABLE
 *    QVariant/JSON introspection methods in a future Kate release, KateDebugBridge
 *    uses them directly.
 *  - Tier 1 (works on stock Kate 25.x/26.x): probes MainWindow::pluginView("kategdbplugin"),
 *    connects to its child Backend signals/slots via Qt Meta-Object, reads
 *    StackView / LocalsView / BreakpointView / ConfigView / Debug Console models
 *    and KTextEditor::Document Execution/Breakpoint marks, and invokes actions/slots.
 */
class KateDebugBridge : public DebugProvider
{
    Q_OBJECT

public:
    explicit KateDebugBridge(QObject *parent = nullptr);
    ~KateDebugBridge() override;

    void setMainWindow(KTextEditor::MainWindow *mainWindow);

    bool isAvailable() const override;
    DebugSessionState sessionState() const override;
    QString activeTarget() const override;
    QList<DebugTarget> targets() const override;
    DebugStopInfo currentStop() const override;
    QList<DebugThread> threads() const override;
    QList<DebugStackFrame> stackFrames(int threadId = -1) const override;
    QList<DebugVariable> locals(int frameLevel = -1) const override;
    QList<DebugVariable> expandVariable(int variablesReference, int timeoutMs = 3000) override;
    QList<DebugBreakpoint> breakpoints() const override;
    QString recentOutput(int maxChars = 4000) const override;

    DebugActionResult selectFrame(int frameLevel) override;
    DebugActionResult selectThread(int threadId) override;
    DebugActionResult setBreakpoint(const QString &file,
                                    int line,
                                    const QString &condition = QString(),
                                    const QString &hitCondition = QString(),
                                    const QString &logMessage = QString(),
                                    bool remove = false) override;
    DebugActionResult clearAllBreakpoints() override;
    DebugActionResult step(const QString &action,
                           const QString &file = QString(),
                           int line = 0,
                           int timeoutMs = 15000) override;
    DebugActionResult evaluate(const QString &expression, int timeoutMs = 5000) override;
    DebugActionResult controlSession(const QString &action,
                                     const QString &targetName = QString(),
                                     int timeoutMs = 15000) override;
    DebugActionResult sendConsoleCommand(const QString &command) override;
    bool reloadLaunchJsonTargets() override;

    // --- Offline-testable parsers matching Kate's kategdbplugin models ------
    static QList<DebugStackFrame> readStackFramesFromModel(const QAbstractItemModel *model, int activeRow = 0);
    static QList<DebugVariable> readLocalsFromTree(const QTreeWidget *tree);
    static QList<DebugBreakpoint> readBreakpointsFromModel(const QAbstractItemModel *model);
    static bool parseStopEventFromOutput(const QString &text, DebugStopInfo *info);
    static bool parseEvalResultFromOutput(const QString &text, const QString &expression, QString *value);

private Q_SLOTS:
    void onPluginCreated(const QString &name, QObject *plugin);
    void onPluginDeleted(const QString &name, QObject *plugin);
    void onPluginViewCreated(const QString &name, QObject *pluginView);
    void onDebugLocationChanged(const QUrl &url, int lineNum);
    void onStackFrameChanged(int level);
    void onOutputText(const QString &text);
    void onOutputError(const QString &text);
    void onReadyForInput(bool ready);
    void onProgramEnded();
    void onGdbEnded();
    void onSourceFileNotFound(const QString &fileName);
    void pollDebuggerState();

private:
    void probeGdbPlugin();
    void bindBackend(QObject *backend);
    QWidget *findChildWidgetByClass(const char *className) const;
    QTreeView *findStackTreeView() const;
    QComboBox *findThreadCombo() const;
    QTreeWidget *findLocalsTreeWidget() const;
    QTreeView *findBreakpointTreeView() const;
    QTextEdit *findOutputTextEdit() const;
    QString readIoViewOutput(int maxChars) const;
    QAction *findPluginAction(const QString &actionName) const;
    bool sendDapReplCommand(const QString &command);
    bool waitForNextPauseOrEnd(int timeoutMs, bool *ended = nullptr);
    void appendOutputBuffer(const QString &chunk);
    bool hasTier2Method(const char *signature) const;

    QPointer<KTextEditor::MainWindow> m_mainWindow;
    QPointer<QObject> m_gdbPluginView;
    QPointer<QObject> m_backend;

    DebugSessionState m_state = DebugSessionState::Unavailable;
    DebugStopInfo m_lastStop;
    int m_activeFrameLevel = 0;
    QString m_outputBuffer;
    bool m_stopEmittedForCurrentPause = false;

    // Expression evaluation synchronization for async outputText replies.
    bool m_evalPending = false;
    QString m_evalExpression;
    QString m_evalResult;
    bool m_evalCaptured = false;

    QTimer *m_pollTimer = nullptr;
};

} // namespace KateAi
