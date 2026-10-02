/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "agentlocks.h"

#include <QDir>
#include <QFileInfo>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

WorkspaceLocks::WorkspaceLocks(QObject *parent)
    : QObject(parent)
{
}

QString WorkspaceLocks::normalize(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    QString cleaned = QDir::cleanPath(path);
    // cleanPath leaves a leading "./" off but keeps the relative form; make it
    // absolute so the same file always hashes to the same key.
    const QFileInfo info(cleaned);
    return info.isAbsolute() ? cleaned : QDir::current().absoluteFilePath(cleaned);
}

bool WorkspaceLocks::tryAcquire(const QString &path, const QString &owner)
{
    const QString key = normalize(path);
    if (key.isEmpty()) {
        return true;
    }
    const auto it = m_locks.constFind(key);
    if (it != m_locks.constEnd()) {
        if (*it == owner) {
            return true;
        }
        Q_EMIT lockDenied(key, owner, *it);
        return false;
    }
    m_locks.insert(key, owner);
    return true;
}

void WorkspaceLocks::release(const QString &path, const QString &owner)
{
    const QString key = normalize(path);
    const auto it = m_locks.find(key);
    if (it != m_locks.end() && *it == owner) {
        m_locks.erase(it);
    }
}

void WorkspaceLocks::releaseAll(const QString &owner)
{
    const QStringList paths = m_locks.keys();
    for (const QString &path : paths) {
        if (m_locks.value(path) == owner) {
            m_locks.remove(path);
        }
    }
}

QString WorkspaceLocks::holder(const QString &path) const
{
    return m_locks.value(normalize(path));
}

bool WorkspaceLocks::isLocked(const QString &path) const
{
    return m_locks.contains(normalize(path));
}

QStringList WorkspaceLocks::lockedPaths() const
{
    return m_locks.keys();
}

} // namespace KateAi