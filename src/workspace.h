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
