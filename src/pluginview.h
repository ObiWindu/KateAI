/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "documentbridge.h"

#include <KTextEditor/MainWindow>
#include <KXMLGUIClient>

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>

class QDialog;

namespace KTextEditor
{
class Document;
class View;
}

namespace KateAi
{
class ChatWidget;
class EditorAssist;
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

    // The chat of this window's panel. The ":kateai" command line command
    // dispatches through this.
    ChatWidget *chat() const
    {
        return m_chat;
    }
    // True when this view's main window shows the editor view, so the
    // command line command reaches the panel of the window it was used in.
    bool ownsEditorView(KTextEditor::View *view) const;

private Q_SLOTS:
    // Kate opened or closed a folder (a project): the workspace can switch
    // without any editor view changing. String-based connection target.
    void projectListChanged();
    // The Projects panel changed which open folder is active (dropdown,
    // Prev/Next, clicking the tree): no project is added or removed and no
    // editor view has to change, but the workspace must follow. Passes the
    // panel's active folder explicitly for the same reason projectListChanged
    // does.
    void projectActiveChanged();
    // The project plugin loaded after this view (plugin load order): hook
    // its signals now and follow the folder it brings.
    void onPluginCreated(const QString &name);

private:
    void addEditorContextActions(KTextEditor::View *view, const QList<QAction *> &actions);
    // Takes the user to the file and line a suggestion was anchored to.
    void openFileAtLine(const QString &path, int line);
    void askSelectionWithInstruction(const QString &instruction);
    QString editorContext() const;
    // (Re)connects every project signal that can switch the folder:
    // the plugin's add/remove plus the per-window panel view's activation
    // (pluginProjectAdded/Removed, projectMapChanged, ...). Also hooks the
    // late-load paths (Application::pluginCreated, MainWindow::
    // pluginViewCreated) so a project plugin that loads after this view
    // still switches the workspace.
    void connectProjectSignals();
    // When explicitWorkspace is non-empty, it overrides detectWorkspace():
    // projectListChanged() passes the newest project's base dir so opening a
    // folder switches the workspace even when an editor view still points
    // at a file from the previous folder.
    void refreshWorkspace(const QString &explicitWorkspace = QString());
    // Asks the user before the MCP servers that the project defines are
    // started. A server is a program, and the list can come with the
    // project.
    void confirmWorkspaceMcpServers(const QString &workspace);
    void updateCompletions();
    // Watches a document for Save-As (rename of the pending-review queue)
    // and closing while the agent works on it. Called for every document
    // that already exists when the view is created and for each one the
    // editor creates later.
    void trackDocument(KTextEditor::Document *document);
    // Posts the in-editor banner for an agent edit of an open document,
    // unless it is already posted.
    void postEditBanner(const QString &path);
    // Mirrors the review queue's per-path line ranges into the editor assist,
    // so the hover hints follow the queue (pending and settled alike).
    void syncEditRanges();
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
    // In-editor AI completions and agent-edit hover hints, one per window.
    EditorAssist *m_assist = nullptr;
    // The answer of the user per workspace: whether the MCP servers of that
    // project may be started.
    QHash<QString, bool> m_mcpTrust;
    DiskDocumentBridge m_bridge;
    // The last local path of every tracked document. documentUrlChanged
    // does not carry the old URL, so the pre-rename path has to be remembered
    // here for the review queue's rename. Cleaned on aboutToClose and
    // destroyed, so the keys are always live documents.
    QHash<KTextEditor::Document *, QString> m_docPaths;
    // Paths with a banner currently posted, so one file gets one banner per
    // review round however many edits the turn makes to it.
    QSet<QString> m_bannerPaths;
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
    // The workspace currently applied to the agent, so a real switch (Kate
    // opened another folder) can be told apart from the refreshes that run
    // on every editor change. The age of the view suppresses the notice for
    // Kate's own session restore at startup.
    QString m_workspace;
    QElapsedTimer m_viewAge;
};

} // namespace KateAi
