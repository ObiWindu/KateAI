/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <KTextEditor/Command>

namespace KTextEditor
{
class View;
}

namespace KateAi
{

class KateAiPlugin;

/**
 * The ":kateai" command for the editor command line, including the vi-mode
 * command line and the "Command Line" tool view.
 *
 * Subcommands:
 *   kateai ask <text>      - send a prompt to the current chat
 *   kateai explain         - explain the selection (or the range the command
 *                            line carries); the active line when nothing is
 *                            selected
 *   kateai fix             - find and fix problems in the selection
 *   kateai refactor        - refactor the selection
 *   kateai tests           - propose tests for the selection
 *   kateai stop            - abort the running turn
 *   kateai new             - start a new chat
 *   kateai mode <id>       - switch agent mode
 *   kateai panel           - show the Kate AI panel
 *
 * A single instance lives on the plugin (see KateAiPlugin): registering the
 * same command name twice would leave the remaining windows without a
 * command when one window closes. The instance dispatches to the view of
 * the window the command was invoked in.
 */
class KateAiCommand : public KTextEditor::Command
{
    Q_OBJECT

public:
    explicit KateAiCommand(KateAiPlugin *plugin, QObject *parent = nullptr);

    bool exec(KTextEditor::View *view, const QString &cmd, QString &msg,
              const KTextEditor::Range &range = KTextEditor::Range::invalid()) override;
    bool help(KTextEditor::View *view, const QString &cmd, QString &msg) override;
    KCompletion *completionObject(KTextEditor::View *view, const QString &cmdname) override;
    bool supportsRange(const QString &cmd) override;

private:
    KateAiPlugin *m_plugin = nullptr;
};

} // namespace KateAi
