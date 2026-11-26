/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QFile>
#include <QFileInfo>
#include <QString>

namespace KateAi
{

/**
 * Reads a text file only when it is a regular file no larger than `maxBytes`.
 *
 * `readAll()` on a workspace- or user-controlled path can block on a FIFO or
 * consume a device such as /dev/zero. The size check handles ordinary files;
 * the bounded `read()` handles paths whose reported size is zero or stale.
 */
inline QString readBoundedFile(const QString &path, qint64 maxBytes)
{
    if (path.trimmed().isEmpty() || maxBytes <= 0) {
        return {};
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.size() > maxBytes) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QByteArray bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes) {
        return {};
    }
    return QString::fromUtf8(bytes);
}

} // namespace KateAi
