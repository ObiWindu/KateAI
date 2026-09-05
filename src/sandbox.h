#pragma once

#include "types.h"

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
    Sandbox(QString workspaceRoot, SandboxProfile profile, QStringList extraDenyGlobs = {});

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

    QStringList wrapCommand(const QString &command, QString *error) const;
    bool bubblewrapAvailable() const;

    static QStringList defaultDenyGlobs();
    static bool globMatch(const QString &pattern, const QString &path);
    static QString primaryCommand(const QString &command);

private:
    bool pathInsideWorkspace(const QString &absolutePath) const;
    QString normalizePath(const QString &path, QString *error, bool forWrite) const;

    QString m_workspaceRoot;
    SandboxProfile m_profile = SandboxProfile::Workspace;
    QStringList m_extraDenyGlobs;
};

} // namespace KateAi
