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
#include <QMetaObject>
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
    // Q_PROPERTY(QList<QObject *> projects READ projectsObjects): the normal
    // path. Fall back to invoking the method directly so a future property
    // rename does not silently empty the list (which would pin the workspace
    // to the old folder).
    const QVariant projects = plugin->property("projects");
    if (projects.isValid()) {
        if (projects.canConvert<QObjectList>()) {
            return projects.value<QObjectList>();
        }
        // QList<QObject *> and QObjectList share the type but canConvert can
        // be picky across Qt versions; try the direct conversion.
        if (projects.userType() == qMetaTypeId<QList<QObject *>>()) {
            return projects.value<QList<QObject *>>();
        }
    }
    QObjectList viaMethod;
    if (QMetaObject::invokeMethod(plugin, "projectsObjects", Q_RETURN_ARG(QObjectList, viaMethod))) {
        return viaMethod;
    }
    if (QMetaObject::invokeMethod(plugin, "projects", Q_RETURN_ARG(QObjectList, viaMethod))) {
        return viaMethod;
    }
    return {};
}

QString projectBaseDir(QObject *project)
{
    if (!project) {
        return QString();
    }
    // KateProject exposes baseDir as Q_PROPERTY; accept the neighbouring
    // spelling and the getter too so a rename does not silently unpin us.
    const QString fromProp = project->property("baseDir").toString().trimmed();
    if (!fromProp.isEmpty()) {
        return QDir::cleanPath(fromProp);
    }
    const QString altProp = project->property("projectBaseDir").toString().trimmed();
    if (!altProp.isEmpty()) {
        return QDir::cleanPath(altProp);
    }
    QString viaMethod;
    if (QMetaObject::invokeMethod(project, "baseDir", Q_RETURN_ARG(QString, viaMethod)) && !viaMethod.trimmed().isEmpty()) {
        return QDir::cleanPath(viaMethod.trimmed());
    }
    return QString();
}

// The base directory of the project that owns filePath, or null when no
// open project contains the file. Exact match counts (the folder itself),
// otherwise the file must sit under the base with a separator.
QString owningProjectBaseDir(QObject *plugin, const QString &filePath)
{
    if (filePath.isEmpty()) {
        return QString();
    }
    const QString clean = QDir::cleanPath(filePath);
    for (QObject *project : openProjects(plugin)) {
        const QString base = projectBaseDir(project);
        if (base.isEmpty()) {
            continue;
        }
        if (clean == base || clean.startsWith(base + QLatin1Char('/'))) {
            return base;
        }
    }
    return QString();
}

// The project Kate's own panel reports as active in this window. Unlike
// openProjects() order (creation order), this follows Prev/Next, the
// dropdown, and clicking the tree — none of which add or remove a project.
QString pluginViewProjectBaseDir(KTextEditor::MainWindow *mainWindow)
{
    if (!mainWindow) {
        return QString();
    }
    QObject *view = mainWindow->pluginView(u"kateprojectplugin"_s);
    if (!view) {
        return QString();
    }
    const QString base = view->property("projectBaseDir").toString().trimmed();
    if (base.isEmpty()) {
        return QString();
    }
    const QString clean = QDir::cleanPath(base);
    return QFileInfo(clean).isDir() ? clean : QString();
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

QString activeProjectBaseDir(KTextEditor::MainWindow *mainWindow)
{
    return pluginViewProjectBaseDir(mainWindow);
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

    // Ask Kate itself which project owns a document: it knows projectRoots,
    // not just baseDir prefixes, so it stays right for nested/out-of-source
    // layouts where a prefix check would pick the wrong folder.
    auto baseDirForLocalFile = [projects](const QString &local) -> QString {
        if (projects && !local.isEmpty()) {
            QString viaApi;
            // projectBaseDirForDocument(KTextEditor::Document*) needs the
            // document; fall back to the prefix check below when we only
            // have a path (other views loop). Handled by callers.
            Q_UNUSED(viaApi);
        }
        if (!local.isEmpty()) {
            const QString base = owningProjectBaseDir(projects, local);
            if (!base.isEmpty()) {
                return base;
            }
        }
        return QString();
    };

    if (mainWindow) {
        if (auto *view = mainWindow->activeView()) {
            const QString local = view->document()->url().toLocalFile();
            if (!local.isEmpty()) {
                // Prefer Kate's document -> project map over a manual
                // prefix check: it follows projectRoots.
                QString viaApi;
                if (projects
                    && QMetaObject::invokeMethod(projects, "projectBaseDirForDocument",
                                                 Q_RETURN_ARG(QString, viaApi),
                                                 Q_ARG(KTextEditor::Document *, view->document()))
                    && !viaApi.trimmed().isEmpty()) {
                    return QDir::cleanPath(viaApi.trimmed());
                }
                const QString base = baseDirForLocalFile(local);
                if (!base.isEmpty()) {
                    return base;
                }
                return detectWorkspaceFromPath(local);
            }
            // Active view is untitled/remote but a project is active in the
            // panel: that panel selection is the folder the user opened.
            const QString active = pluginViewProjectBaseDir(mainWindow);
            if (!active.isEmpty()) {
                return active;
            }
        }
        for (auto *v : mainWindow->views()) {
            const QString local = v->document()->url().toLocalFile();
            if (!local.isEmpty()) {
                QString viaApi;
                if (projects
                    && QMetaObject::invokeMethod(projects, "projectBaseDirForDocument",
                                                 Q_RETURN_ARG(QString, viaApi),
                                                 Q_ARG(KTextEditor::Document *, v->document()))
                    && !viaApi.trimmed().isEmpty()) {
                    return QDir::cleanPath(viaApi.trimmed());
                }
                const QString base = baseDirForLocalFile(local);
                if (!base.isEmpty()) {
                    return base;
                }
                return detectWorkspaceFromPath(local);
            }
        }
        // No local document: "Open Folder" still shows up as a project,
        // while the process working directory still points where Kate was
        // started. Prefer the panel's active project (what the user
        // selected), then the newest opened folder.
        const QString active = pluginViewProjectBaseDir(mainWindow);
        if (!active.isEmpty()) {
            return active;
        }
    }

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
