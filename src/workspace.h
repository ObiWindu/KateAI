/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>

class QObject;

namespace KTextEditor
{
class MainWindow;
}

namespace KateAi
{

// Kate's project plugin, or null when Kate runs without it (KWrite mode, or
// the user disabled the plugin). "Open Folder" creates a project for the
// folder; the plugin's QObject is the way to see that without Kate-private
// headers.
QObject *projectPlugin();

// The base directory of the most recently opened project, or null when no
// project is open. "Open Folder" appends to the project list, so the last
// entry is the folder the user opened most recently. Used by
// projectListChanged() to follow a newly opened folder even when an editor
// view still points at a file from the previous folder.
QString newestProjectBaseDir();

QString detectWorkspace(KTextEditor::MainWindow *mainWindow);
QString detectWorkspaceFromPath(const QString &filePath);

} // namespace KateAi
