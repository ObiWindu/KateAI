/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>

namespace KTextEditor
{
class MainWindow;
}

namespace KateAi
{

QString detectWorkspace(KTextEditor::MainWindow *mainWindow);
QString detectWorkspaceFromPath(const QString &filePath);

} // namespace KateAi
