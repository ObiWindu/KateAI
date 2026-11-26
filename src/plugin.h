/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <KTextEditor/Plugin>
#include <KTextEditor/SessionConfigInterface>

#include <QHash>
#include <QList>
#include <QStringList>

#include <memory>

class KConfigGroup;
class QNetworkAccessManager;

namespace KateAi
{

class KateAiCommand;
class KateAiView;
class WorkspaceServices;

class KateAiPlugin : public KTextEditor::Plugin, public KTextEditor::SessionConfigInterface
{
    Q_OBJECT
    Q_INTERFACES(KTextEditor::SessionConfigInterface)

public:
    explicit KateAiPlugin(QObject *parent, const QVariantList &args = {});

    QObject *createView(KTextEditor::MainWindow *mainWindow) override;
    int configPages() const override;
    KTextEditor::ConfigPage *configPage(int number = 0, QWidget *parent = nullptr) override;

    // --- Kate session integration --------------------------------------------
    // Kate persists these with the session; the per-view conversation ids
    // restore each window's chat when the session is reopened.
    void readSessionConfig(const KConfigGroup &config) override;
    void writeSessionConfig(KConfigGroup &config) override;

    // Views register themselves on creation so the command line command and
    // the session config can find the view that owns an editor view.
    void registerView(KateAiView *view);
    void unregisterView(KateAiView *view);
    const QList<KateAiView *> &views() const
    {
        return m_views;
    }

    // The next conversation id the stored session has for a new window, or
    // an empty string when the session has none left to hand out.
    QString claimSessionConversation();

    // The current conversation of a view, remembered for writeSessionConfig.
    void noteConversationChanged(KateAiView *view, const QString &conversationId);

    Settings settings() const
    {
        return m_settings;
    }
    void setSettings(const Settings &settings);

    // The shared graph/MCP/event-store bundle for a workspace. The plugin
    // keeps only a weak reference; the views that use it keep it alive, and
    // closing the last one releases the resources.
    std::shared_ptr<WorkspaceServices> workspaceServices(const QString &workspace);

    // The plugin-wide network manager. Settings dialogs used to construct their
    // own, so every open allocated a fresh connection pool and re-downloaded
    // the ACP registry; sharing one also lets Qt reuse the connections.
    QNetworkAccessManager *networkManager() const
    {
        return m_network;
    }

Q_SIGNALS:
    void settingsChanged(const Settings &settings);

private:
    Settings m_settings;
    // The KateAiView of every main window, in creation order. KateAiView
    // removes itself in its destructor, so the list never holds dangling
    // pointers.
    QList<KateAiView *> m_views;
    // The conversation each view is currently showing.
    QHash<KateAiView *, QString> m_viewConversations;
    // Conversation ids restored from the session config, handed out in
    // order as windows appear.
    QStringList m_sessionConversations;
    // The last known live view order. Kate can destroy its windows before it
    // asks the plugin to write the session config; without this snapshot every
    // view had unregistered first and the next session reopened blank.
    QStringList m_lastConversationIds;
    // Canonical workspace path -> shared services. Weak so services are
    // released when the last view on the workspace goes away.
    QHash<QString, std::weak_ptr<WorkspaceServices>> m_workspaceServices;
    // One network manager for the plugin: the update check, the settings
    // dialogs and the ACP installer share its connection pool instead of each
    // allocating their own.
    QNetworkAccessManager *m_network = nullptr;
    QStringList liveConversationIds() const;
    // The ":kateai" command line command. One instance for the whole
    // process: a second registration of the same command name would replace
    // the first in the editor's registry, and closing one window would then
    // unregister the command for the others.
    KateAiCommand *m_command = nullptr;
};

} // namespace KateAi
