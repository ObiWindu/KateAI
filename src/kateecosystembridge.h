/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "ecosystem.h"

#include <QPointer>
#include <QString>

#include <functional>

class QAction;
class QMenu;
class QWidget;

namespace KTextEditor
{
class MainWindow;
}

namespace KateAi
{

/**
 * Concrete EcosystemProvider bridging Kate's companion plugins at runtime:
 *  - katebuildplugin: target listing + triggering Kate Build actions
 *  - katekonsoleplugin: running commands in Kate's embedded terminal
 *  - kateprojectplugin: injecting Kate AI actions into the Project tree menu
 *  - katebacktracebrowserplugin: reading loaded crash backtraces
 *  - KTextEditor::MainWindow::addWidget(): opening rich native Kate tabs for
 *    multi-file review diffs, implementation plans, and project graphs
 */
class KateEcosystemBridge : public EcosystemProvider
{
    Q_OBJECT

public:
    explicit KateEcosystemBridge(QObject *parent = nullptr);
    ~KateEcosystemBridge() override;

    void setMainWindow(KTextEditor::MainWindow *mainWindow);

    bool isBuildPluginAvailable() const override;
    QList<BuildTargetInfo> buildTargets(const QString &workspace) const override;
    EcosystemActionResult triggerBuild(const QString &targetName = QString()) override;

    bool isTerminalAvailable() const override;
    EcosystemActionResult runInTerminal(const QString &command,
                                        const QString &workingDir = QString()) override;
    QString terminalContext() const override;

    QString activeBacktraceText() const override;

    // Opens a rich read-only/interactive document tab natively inside Kate's
    // tab bar via KTextEditor::MainWindow::addWidget().
    QWidget *openNativeTab(const QString &title,
                           const QString &subtitle,
                           const QString &content,
                           bool isDiff = false,
                           const std::function<void()> &onAcceptAll = {},
                           const std::function<void()> &onRejectAll = {});

Q_SIGNALS:
    // Emitted when the user picks a Kate AI action from Kate's Project tree
    // context menu. actionId is "ask", "summarize", or "tests".
    void projectTreeActionRequested(const QString &actionId, const QString &path, const QString &baseDir);

private Q_SLOTS:
    void onPluginViewCreated(const QString &name, QObject *pluginView);
    void onProjectTreeContextMenu(QMenu *menu, const QString &path, const QString &baseDir, int itemType);

private:
    void hookProjectTreeMenu();
    QObject *buildPluginView() const;
    QObject *konsolePluginView() const;
    QAction *findPluginAction(QObject *pluginView, const QString &actionName) const;

    QPointer<KTextEditor::MainWindow> m_mainWindow;
    QPointer<QObject> m_hookedProjectView;
};

} // namespace KateAi
