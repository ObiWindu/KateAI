/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace KateAi
{

struct BuildTargetInfo {
    QString name;
    QString type;       // e.g. "EXECUTABLE", "STATIC_LIBRARY", "SHARED_LIBRARY", "kate-build"
    QString buildDir;
    QString command;
    QString artifactPath;
    bool isDefault = false;
};

struct EcosystemActionResult {
    bool ok = false;
    QString output;
};

/**
 * Abstract bridge to Kate's build, terminal, and project-ecosystem plugins
 * (katebuildplugin, katekonsoleplugin, kateprojectplugin,
 * katebacktracebrowserplugin). Core components (ToolRunner, AgentLoop,
 * AcpClient) only see this interface, keeping tests guiless and deterministic.
 */
class EcosystemProvider : public QObject
{
    Q_OBJECT

public:
    explicit EcosystemProvider(QObject *parent = nullptr);
    ~EcosystemProvider() override = default;

    // --- Build plugin & CMake/compile_commands discovery --------------------
    virtual bool isBuildPluginAvailable() const = 0;
    virtual QList<BuildTargetInfo> buildTargets(const QString &workspace) const = 0;
    virtual QString compileCommandsPath(const QString &workspace) const;
    virtual EcosystemActionResult triggerBuild(const QString &targetName = QString()) = 0;

    // --- Konsole terminal plugin --------------------------------------------
    virtual bool isTerminalAvailable() const = 0;
    virtual EcosystemActionResult runInTerminal(const QString &command,
                                                const QString &workingDir = QString()) = 0;
    virtual QString terminalContext() const
    {
        return {};
    }

    // --- Project / Git / Backtrace helpers ----------------------------------
    virtual QString activeBacktraceText() const
    {
        return {};
    }

    // Pure filesystem discovery helper shared by both KateEcosystemBridge and
    // unit tests: inspects CMake File API reply files, CMakeLists.txt,
    // compile_commands.json, Cargo.toml, package.json, and Makefiles.
    static QList<BuildTargetInfo> discoverWorkspaceBuildTargets(const QString &workspace);
    static QString findCompileCommands(const QString &workspace);
};

} // namespace KateAi
