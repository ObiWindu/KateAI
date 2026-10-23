/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace KateAi
{

class SandboxError
{
public:
    explicit SandboxError(QString message)
        : m_message(std::move(message))
    {
    }

    QString message() const
    {
        return m_message;
    }

private:
    QString m_message;
};

class Sandbox
{
public:
    // allowOutsideReads lifts the workspace confinement for the read tools.
    // It is off by default: the read tools are approved without a prompt, so
    // a confined read is the only thing that keeps the agent inside the
    // project.
    Sandbox(QString workspaceRoot, SandboxProfile profile, QStringList extraDenyGlobs = {}, bool allowOutsideReads = false);

    QString workspaceRoot() const
    {
        return m_workspaceRoot;
    }
    SandboxProfile profile() const
    {
        return m_profile;
    }

    QStringList denyGlobs() const;

    bool isDenied(const QString &path) const;
    bool allowsRead(const QString &path, QString *error) const;
    bool allowsWrite(const QString &path, QString *error) const;

    QString resolve(const QString &path, QString *error, bool forWrite = false) const;

    bool isReadOnlyCommand(const QString &command) const;
    bool isDangerousCommand(const QString &command) const;
    bool isAlwaysDeniedCommand(const QString &command) const;
    // True when a shell command names a path the deny rules cover, e.g.
    // "cat ~/.ssh/id_rsa". Deny globs apply to file tools already; the shell
    // needs the same guard or a read-only command sidesteps them entirely.
    bool commandTouchesDeniedPath(const QString &command) const;

    QStringList wrapCommand(const QString &command, QString *error) const;
    // Wrap an argv vector (ACP terminal/create). The program and each argument
    // stay separate; nothing is re-quoted into `bash -c`.
    QStringList wrapArgv(const QString &program, const QStringList &args, QString *error) const;
    bool bubblewrapAvailable() const;
    bool isolationAvailable() const;

    static QStringList defaultDenyGlobs();
    static bool globMatch(const QString &pattern, const QString &path);
    static QString primaryCommand(const QString &command);

private:
    bool pathInsideWorkspace(const QString &absolutePath) const;
    QString normalizePath(const QString &path, QString *error, bool forWrite) const;
    bool globMatchCached(const QString &pattern, const QString &path) const;

    QString m_workspaceRoot;
    SandboxProfile m_profile = SandboxProfile::Workspace;
    QStringList m_extraDenyGlobs;
    bool m_allowOutsideReads = false;
    // Compiled deny-glob patterns. globToRegex() is cheap but QRegularExpression
    // construction is not, and isDenied() runs one per glob per candidate path.
    mutable QHash<QString, QRegularExpression> m_globCache;
    mutable QStringList m_cachedDenyGlobs;
};

} // namespace KateAi
