/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace KateAi
{

class Sandbox;

struct HookDefinition {
    QString event;
    QString matcher;
    QStringList argv;
    bool shell = false;
    int timeoutMs = 5000;
    bool blockOnFailure = false;
    QString source;
    bool project = false;
};

struct HookOutcome {
    bool blocked = false;
    bool failed = false;
    bool timedOut = false;
    QString reason;
    QJsonObject updatedInput;
    QString additionalContext;
    QStringList warnings;
    QString hookId;
};

/**
 * Deterministic user/project automation at lifecycle points.
 *
 * Project hooks are untrusted until their file hash is approved; user and
 * managed hooks are trusted. Commands run with a scrubbed environment and,
 * when a sandbox is supplied, through the same sandbox wrapper as tools.
 */
class HookRegistry
{
public:
    void clear();
    bool load(const QString &workspace, QString *error = nullptr);
    void setWorkspace(const QString &workspace);

    bool projectTrusted() const
    {
        return m_projectTrusted;
    }
    void setProjectTrusted(bool trusted);
    bool saveProjectTrust();
    QString trustKey() const;
    static QString trustKeyFor(const QString &workspace);

    QStringList hookIds() const;
    bool hasHooks(const QString &event) const;
    QList<HookDefinition> hooks(const QString &event) const;

    // Runs every hook for the event. A block stops the remaining hooks; an
    // update from an earlier hook is kept and later updates merge over it.
    HookOutcome run(const QString &event, const QJsonObject &payload,
                    const Sandbox *sandbox = nullptr);

    static QStringList knownEvents();
    static QString userHooksPath();
    static QString managedHooksPath();

private:
    bool loadFile(const QString &path, bool project, QString *error);
    HookOutcome runOne(const HookDefinition &hook, const QJsonObject &payload,
                       const Sandbox *sandbox);

    QString m_workspace;
    QList<HookDefinition> m_hooks;
    bool m_projectTrusted = false;
};

} // namespace KateAi
