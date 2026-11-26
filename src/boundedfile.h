/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
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

/**
 * Cross-check for an HTTP reply before its body is read with `readAll()`.
 *
 * A completed reply has already buffered its whole body, so this cannot avoid
 * the allocation that Qt's reply object already made - it stops the copy and
 * the JSON tree on top of it. A non-streaming provider response is small, so a
 * body above the cap is a misconfiguration or a hostile endpoint rather than a
 * legitimate answer.
 */
inline bool replyBodyWithinLimit(const QNetworkReply *reply, qint64 maxBytes)
{
    if (!reply || maxBytes <= 0) {
        return false;
    }
    const qint64 declaredSize = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if (declaredSize > maxBytes) {
        return false;
    }
    return reply->bytesAvailable() <= maxBytes;
}

} // namespace KateAi
