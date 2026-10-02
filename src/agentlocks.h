/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QString>

namespace KateAi
{

// Guards files that are being edited right now. Two sub-agents working in
// parallel would otherwise silently overwrite each other, because each one
// reads the file once and writes the whole result back.
//
// The root agent owns the locker and hands the same instance to every
// sub-agent, so the whole team shares one table.
class WorkspaceLocks : public QObject
{
    Q_OBJECT

public:
    explicit WorkspaceLocks(QObject *parent = nullptr);

    // Re-acquiring a path already owned by `owner` succeeds, so a single agent
    // can touch the same file across several calls.
    bool tryAcquire(const QString &path, const QString &owner);
    void release(const QString &path, const QString &owner);
    void releaseAll(const QString &owner);
    // Empty when the path is free, otherwise the owner currently editing it.
    QString holder(const QString &path) const;
    bool isLocked(const QString &path) const;
    // Paths currently locked by anyone.
    QStringList lockedPaths() const;
    int count() const
    {
        return m_locks.size();
    }

Q_SIGNALS:
    // Emitted when an agent is blocked so the chat can say why.
    void lockDenied(const QString &path, const QString &owner, const QString &holder);

private:
    // Normalises so "a/b.txt", "./a/b.txt" and an absolute path collide.
    static QString normalize(const QString &path);

    QHash<QString, QString> m_locks; // path -> owner
};

} // namespace KateAi