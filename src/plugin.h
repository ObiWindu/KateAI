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

class KConfigGroup;

namespace KateAi
{

class KateAiCommand;
class KateAiView;

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
    // The ":kateai" command line command. One instance for the whole
    // process: a second registration of the same command name would replace
    // the first in the editor's registry, and closing one window would then
    // unregister the command for the others.
    KateAiCommand *m_command = nullptr;
};

} // namespace KateAi
