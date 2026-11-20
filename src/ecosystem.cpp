/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "ecosystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

EcosystemProvider::EcosystemProvider(QObject *parent)
    : QObject(parent)
{
}

QString EcosystemProvider::compileCommandsPath(const QString &workspace) const
{
    return findCompileCommands(workspace);
}

QString EcosystemProvider::findCompileCommands(const QString &workspace)
{
    if (workspace.isEmpty()) {
        return {};
    }
    const QDir root(workspace);
    const QStringList candidates = {
        u"compile_commands.json"_s,
        u"build/compile_commands.json"_s,
        u"cmake-build-debug/compile_commands.json"_s,
        u"cmake-build-release/compile_commands.json"_s,
        u"out/build/compile_commands.json"_s,
        u"builddir/compile_commands.json"_s,
    };
    for (const QString &rel : candidates) {
        const QString full = root.absoluteFilePath(rel);
        if (QFileInfo::exists(full)) {
            return QDir::cleanPath(full);
        }
    }
    return {};
}

QList<BuildTargetInfo> EcosystemProvider::discoverWorkspaceBuildTargets(const QString &workspace)
{
    QList<BuildTargetInfo> out;
    if (workspace.isEmpty()) {
        return out;
    }
    const QDir root(workspace);
    QSet<QString> seen;

    auto addTarget = [&](BuildTargetInfo info) {
        const QString key = info.name + u'|' + info.buildDir + u'|' + info.command;
        if (info.name.isEmpty() || seen.contains(key)) {
            return;
        }
        seen.insert(key);
        out.append(std::move(info));
    };

    // 1. CMake File API reply (exact build dir, target type, and output artifact)
    const QStringList buildDirs = {u"build"_s, u"cmake-build-debug"_s, u"cmake-build-release"_s, u"out/build"_s};
    for (const QString &relBuild : buildDirs) {
        const QString buildPath = QDir::cleanPath(root.absoluteFilePath(relBuild));
        const QString replyPath = buildPath + u"/.cmake/api/v1/reply"_s;
        QDir replyDir(replyPath);
        if (replyDir.exists()) {
            const QStringList files = replyDir.entryList({u"target-*.json"_s}, QDir::Files, QDir::Name);
            for (const QString &file : files) {
                QFile f(replyDir.absoluteFilePath(file));
                if (!f.open(QIODevice::ReadOnly)) {
                    continue;
                }
                const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
                const QString name = obj.value(u"name"_s).toString();
                const QString type = obj.value(u"type"_s).toString();
                if (name.isEmpty()) {
                    continue;
                }
                BuildTargetInfo info;
                info.name = name;
                info.type = type.isEmpty() ? u"cmake"_s : type;
                info.buildDir = buildPath;
                info.command = u"cmake --build \"%1\" --target %2"_s.arg(relBuild, name);
                const QJsonArray artifacts = obj.value(u"artifacts"_s).toArray();
                if (!artifacts.isEmpty()) {
                    const QString relArt = artifacts.first().toObject().value(u"path"_s).toString();
                    if (!relArt.isEmpty()) {
                        info.artifactPath = QDir::cleanPath(buildPath + u'/' + relArt);
                    }
                }
                addTarget(std::move(info));
            }
        } else if (QFileInfo::exists(buildPath + u"/CMakeCache.txt"_s)) {
            BuildTargetInfo info;
            info.name = u"all (%1)"_s.arg(relBuild);
            info.type = u"cmake"_s;
            info.buildDir = buildPath;
            info.command = u"cmake --build \"%1\""_s.arg(relBuild);
            info.isDefault = out.isEmpty();
            addTarget(std::move(info));
        }
    }

    // 2. Fallback: CMakeLists.txt without CMake File API reply yet
    if (out.isEmpty() && root.exists(u"CMakeLists.txt"_s)) {
        const QString buildDir = QDir::cleanPath(root.absoluteFilePath(u"build"_s));
        QFile cm(root.absoluteFilePath(u"CMakeLists.txt"_s));
        if (cm.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString text = QString::fromUtf8(cm.read(256 * 1024));
            static const QRegularExpression re(
                QStringLiteral(R"(\badd_(executable|library)\s*\(\s*([A-Za-z0-9_.-]+))"),
                QRegularExpression::CaseInsensitiveOption);
            auto it = re.globalMatch(text);
            while (it.hasNext()) {
                const auto m = it.next();
                const QString kind = m.captured(1).toLower() == u"executable"_s ? u"EXECUTABLE"_s : u"LIBRARY"_s;
                const QString tname = m.captured(2);
                if (tname.startsWith(u'$')) {
                    continue;
                }
                BuildTargetInfo info;
                info.name = tname;
                info.type = kind;
                info.buildDir = buildDir;
                info.command = u"cmake --build build --target %1"_s.arg(tname);
                info.isDefault = out.isEmpty();
                addTarget(std::move(info));
            }
        }
        if (out.isEmpty()) {
            BuildTargetInfo info;
            info.name = u"cmake-default"_s;
            info.type = u"cmake"_s;
            info.buildDir = buildDir;
            info.command = u"cmake -B build && cmake --build build"_s;
            info.isDefault = true;
            addTarget(std::move(info));
        }
    }

    // 3. Cargo
    if (root.exists(u"Cargo.toml"_s)) {
        addTarget({u"cargo build"_s, u"cargo"_s, workspace, u"cargo build"_s, {}, out.isEmpty()});
        addTarget({u"cargo test"_s, u"cargo"_s, workspace, u"cargo test"_s, {}, false});
    }

    // 4. package.json scripts
    if (root.exists(u"package.json"_s)) {
        QFile pkg(root.absoluteFilePath(u"package.json"_s));
        if (pkg.open(QIODevice::ReadOnly)) {
            const QJsonObject scripts = QJsonDocument::fromJson(pkg.readAll()).object().value(u"scripts"_s).toObject();
            for (auto it = scripts.constBegin(); it != scripts.constEnd(); ++it) {
                BuildTargetInfo info;
                info.name = u"npm:%1"_s.arg(it.key());
                info.type = u"npm"_s;
                info.buildDir = workspace;
                info.command = u"npm run %1"_s.arg(it.key());
                info.isDefault = (it.key() == u"build"_s);
                addTarget(std::move(info));
            }
        }
    }

    // 5. Go
    if (root.exists(u"go.mod"_s)) {
        addTarget({u"go build"_s, u"go"_s, workspace, u"go build ./..."_s, {}, out.isEmpty()});
        addTarget({u"go test"_s, u"go"_s, workspace, u"go test ./..."_s, {}, false});
    }

    // 6. Python (pyproject.toml / pytest)
    if (root.exists(u"pyproject.toml"_s) || root.exists(u"pytest.ini"_s)) {
        addTarget({u"pytest"_s, u"python"_s, workspace, u"pytest"_s, {}, out.isEmpty()});
    }

    return out;
}

} // namespace KateAi
