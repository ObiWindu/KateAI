#include "workspace.h"

#include <KTextEditor/Document>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>

#include <QDir>
#include <QFileInfo>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QString detectWorkspaceFromPath(const QString &filePath)
{
    if (filePath.isEmpty()) {
        return QDir::currentPath();
    }

    QDir dir = QFileInfo(filePath).isDir() ? QDir(filePath) : QFileInfo(filePath).absoluteDir();
    while (true) {
        if (QFileInfo::exists(dir.filePath(u".git"_s)) || QFileInfo::exists(dir.filePath(u".kateproject"_s))) {
            return dir.absolutePath();
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QFileInfo(filePath).isDir() ? QDir(filePath).absolutePath() : QFileInfo(filePath).absolutePath();
}

QString detectWorkspace(KTextEditor::MainWindow *mainWindow)
{
    if (mainWindow) {
        if (auto *view = mainWindow->activeView()) {
            const QString local = view->document()->url().toLocalFile();
            if (!local.isEmpty()) {
                return detectWorkspaceFromPath(local);
            }
        }
        for (auto *view : mainWindow->views()) {
            const QString local = view->document()->url().toLocalFile();
            if (!local.isEmpty()) {
                return detectWorkspaceFromPath(local);
            }
        }
    }
    return QDir::currentPath();
}

} // namespace KateAi
