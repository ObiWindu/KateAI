/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "workspace.h"

#include <KTextEditor/Application>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>
#include <KTextEditor/Plugin>
#include <KTextEditor/View>

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QVariant>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QObject *projectPlugin()
{
    auto *editor = KTextEditor::Editor::instance();
    auto *application = editor ? editor->application() : nullptr;
    return application ? application->plugin(u"kateprojectplugin"_s) : nullptr;
}

namespace
{

// The projects currently open in Kate, newest last: the plugin appends a
// project when it is created, so the last entry is the folder the user
// opened most recently. Read through the plugin's "projects" property; a
// Kate without the property yields an empty list and the fallbacks below
// take over.
QObjectList openProjects(QObject *plugin)
{
    if (!plugin) {
        return {};
    }
    const QVariant projects = plugin->property("projects");
    return projects.canConvert<QObjectList>() ? projects.value<QObjectList>() : QObjectList();
}

QString projectBaseDir(QObject *project)
{
    if (!project) {
        return QString();
    }
    const QString base = project->property("baseDir").toString().trimmed();
    return base.isEmpty() ? QString() : QDir::cleanPath(base);
}

// The base directory of the project that owns filePath, or null when no
// open project contains the file.
QString owningProjectBaseDir(QObject *plugin, const QString &filePath)
{
    if (filePath.isEmpty()) {
        return QString();
    }
    const QString clean = QDir::cleanPath(filePath);
    for (QObject *project : openProjects(plugin)) {
        const QString base = projectBaseDir(project);
        if (!base.isEmpty() && clean.startsWith(base + QLatin1Char('/'))) {
            return base;
        }
    }
    return QString();
}

} // namespace

QString newestProjectBaseDir()
{
    const QObjectList list = openProjects(projectPlugin());
    for (int i = list.size() - 1; i >= 0; --i) {
        const QString base = projectBaseDir(list.at(i));
        if (!base.isEmpty()) {
            return base;
        }
    }
    return QString();
}

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
    QObject *projects = projectPlugin();

    if (mainWindow) {
        if (auto *view = mainWindow->activeView()) {
            const QString local = view->document()->url().toLocalFile();
            if (!local.isEmpty()) {
                // A document that belongs to a project carries the folder the
                // user opened, not only the directory the file sits in.
                const QString base = owningProjectBaseDir(projects, local);
                if (!base.isEmpty()) {
                    return base;
                }
                return detectWorkspaceFromPath(local);
            }
        }
        for (auto *view : mainWindow->views()) {
            const QString local = view->document()->url().toLocalFile();
            if (!local.isEmpty()) {
                const QString base = owningProjectBaseDir(projects, local);
                if (!base.isEmpty()) {
                    return base;
                }
                return detectWorkspaceFromPath(local);
            }
        }
    }

    // No document is open: "Open Folder" still shows up here as a project,
    // while the process working directory still points where Kate was
    // started. The newest project is the folder the user just opened.
    const QObjectList list = openProjects(projects);
    for (int i = list.size() - 1; i >= 0; --i) {
        const QString base = projectBaseDir(list.at(i));
        if (!base.isEmpty()) {
            return base;
        }
    }
    return QDir::currentPath();
}

} // namespace KateAi
