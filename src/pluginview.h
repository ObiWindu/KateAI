/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "documentbridge.h"

#include <KTextEditor/MainWindow>
#include <KXMLGUIClient>

#include <QHash>
#include <QObject>
#include <QPointer>

class QDialog;

namespace KateAi
{
class ChatWidget;
class KateAiPlugin;

class KateAiView : public QObject, public KXMLGUIClient
{
    Q_OBJECT

public:
    KateAiView(KateAiPlugin *plugin, KTextEditor::MainWindow *mainWindow);
    ~KateAiView() override;

    void showPanel();
    void newChat();
    void askSelection();
    void showConfiguration();

private:
    void addEditorContextActions(KTextEditor::View *view, const QList<QAction *> &actions);
    // Takes the user to the file and line a suggestion was anchored to.
    void openFileAtLine(const QString &path, int line);
    void askSelectionWithInstruction(const QString &instruction);
    QString editorContext() const;
    void refreshWorkspace();
    // Asks the user before the MCP servers that the project defines are
    // started. A server is a program, and the list can come with the
    // project.
    void confirmWorkspaceMcpServers(const QString &workspace);
    void updateCompletions();
    // Catches QEvent::ApplicationPaletteChange on qApp (Qt 6 deprecates
    // QGuiApplication::paletteChanged in favour of the event).
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Re-reads the editor theme and repaints the chat panel and the tool view
    // when it changed.
    void recheckEditorTheme();
    // Paints the tool view (the KateAI window) from the window-chrome colours.
    void applyWindowChrome();

    KateAiPlugin *m_plugin = nullptr;
    KTextEditor::MainWindow *m_mainWindow = nullptr;
    QPointer<QWidget> m_toolView;
    QPointer<QDialog> m_configDialog;
    ChatWidget *m_chat = nullptr;
    // The answer of the user per workspace: whether the MCP servers of that
    // project may be started.
    QHash<QString, bool> m_mcpTrust;
    DiskDocumentBridge m_bridge;
    // Editor colour scheme identity as of the last theme load, so configChanged
    // and ApplicationPaletteChange (which fire for every editor setting and
    // every system theme switch) only repaint on a real change. The background
    // colour is tracked alongside the name: KDE can swap the scheme under the
    // same name, and the signal can fire before theme() reflects the new
    // scheme, so the check is deferred to the next event-loop turn and
    // compares colours.
    QString m_lastEditorThemeName;
    QString m_lastEditorThemeBg;
    // The application palette's window colour as of the last theme load. In
    // inherit mode the window chrome follows the editor theme, but a system
    // theme switch can fire ApplicationPaletteChange without touching the
    // editor scheme, and the tool view must repaint regardless.
    QString m_lastWindowBg;
};

} // namespace KateAi
