/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "toolresults.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
#include <QUuid>

#include <algorithm>
#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QString ToolResults::s_directoryOverride;

QString ToolResults::directory()
{
    if (!s_directoryOverride.isEmpty()) {
        return s_directoryOverride;
    }
    const QString fromEnv = QProcessEnvironment::systemEnvironment().value(u"KATEAI_TOOL_RESULTS_DIR"_s);
    if (!fromEnv.isEmpty()) {
        return fromEnv;
    }
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath() + u"/kateai-cache"_s;
    }
    return QDir(base).filePath(u"tool-results"_s);
}

void ToolResults::setDirectoryForTests(const QString &directory)
{
    s_directoryOverride = directory;
}

bool ToolResults::validId(const QString &id)
{
    // UUID without braces, but accept any short alphanumeric/dash id so the
    // format can evolve. Rejects separators, dots and traversal.
    static const QRegularExpression pattern(u"^[A-Za-z0-9-]{8,64}$"_s);
    return pattern.match(id).hasMatch();
}

StoredToolResult ToolResults::store(const QString &text, int headChars, int tailChars)
{
    StoredToolResult result;
    result.totalChars = text.size();

    const int head = qMax(0, headChars);
    const int tail = qMax(0, tailChars);
    // Not worth a file when the preview would be almost the whole text.
    if (text.size() <= head + tail + 512) {
        result.preview = text;
        return result;
    }

    const QString dir = directory();
    if (!QDir().mkpath(dir)) {
        result.preview = text.left(head + tail);
        return result;
    }

    // 32 MiB cap. A command that produced more than that is already a problem
    // for the user; storing unbounded output would be worse.
    constexpr qsizetype kMaxStoredChars = 32 * 1024 * 1024;
    const QString stored = text.size() > kMaxStoredChars ? text.left(kMaxStoredChars) : text;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString path = QDir(dir).filePath(id + u".txt"_s);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.preview = text.left(head + tail);
        return result;
    }
    const QByteArray payload = stored.toUtf8();
    if (file.write(payload) != payload.size()) {
        file.close();
        QFile::remove(path);
        result.preview = text.left(head + tail);
        return result;
    }
    file.close();
    // Tool output can contain source code or command output the user considers
    // sensitive; keep the cache owner-only.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    // Total character count for paging. Without it, read() had to parse the
    // whole file just to fill totalChars, even for a 20 KB page.
    QFile meta(path + u".len"_s);
    if (meta.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        meta.write(QByteArray::number(stored.size()));
        meta.close();
        QFile::setPermissions(meta.fileName(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    result.id = id;
    result.totalChars = stored.size();
    const qsizetype omitted = stored.size() - head - tail;
    QString previewHead = stored.left(head);
    if (!previewHead.isEmpty() && previewHead.at(previewHead.size() - 1).isHighSurrogate()
        && head < stored.size() && stored.at(head).isLowSurrogate()) {
        previewHead.chop(1);
    }
    QString previewTail = stored.right(tail);
    if (!previewTail.isEmpty() && previewTail.at(0).isLowSurrogate()
        && stored.size() > tail && stored.at(stored.size() - tail - 1).isHighSurrogate()) {
        previewTail.remove(0, 1);
    }
    result.preview = previewHead
        + u"\n\n… [tool output truncated: showing the first %1 and last %2 of %3 characters; %4 omitted] …\n"_s
              .arg(head).arg(tail).arg(stored.size()).arg(omitted)
        + u"Full output: tool_result id=" + id
        + u" (call read_tool_result with this id and optional offset/limit)\n\n"_s
        + previewTail;
    // The store is shared by every conversation. Evict oldest-first so a long
    // session cannot grow the cache without bound; the preview already says
    // how to page, and an evicted id reports "unknown or expired". The file
    // just written is always kept, even if timestamps tie.
    prune(200LL * 1024 * 1024, {path});
    return result;
}

int ToolResults::prune(qint64 maxBytes, const QStringList &keep)
{
    const QString dirPath = directory();
    QDir dir(dirPath);
    if (!dir.exists()) {
        return 0;
    }
    QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.txt")},
                                              QDir::Files, QDir::NoSort);
    qint64 total = 0;
    for (const QFileInfo &info : std::as_const(entries)) {
        total += info.size();
    }
    if (total <= maxBytes) {
        return 0;
    }
    // Sort explicitly oldest-first. QDir::Time leaves ties in unspecified
    // order, which can delete the file that was just written on filesystems
    // with coarse timestamps.
    std::sort(entries.begin(), entries.end(), [](const QFileInfo &a, const QFileInfo &b) {
        if (a.lastModified() != b.lastModified()) {
            return a.lastModified() < b.lastModified();
        }
        return a.fileName() < b.fileName();
    });
    QSet<QString> keepPaths;
    for (const QString &path : keep) {
        keepPaths.insert(QFileInfo(path).absoluteFilePath());
    }
    int removed = 0;
    for (const QFileInfo &info : std::as_const(entries)) {
        if (total <= maxBytes) {
            break;
        }
        if (keepPaths.contains(info.absoluteFilePath())) {
            continue;
        }
        const qint64 size = info.size();
        if (QFile::remove(info.absoluteFilePath())) {
            // The sidecar is only a character count for the paging path.
            QFile::remove(info.absoluteFilePath() + u".len"_s);
            total -= size;
            ++removed;
        }
    }
    return removed;
}

bool ToolResults::read(const QString &id, qint64 offset, int limit, QString *out,
                       qint64 *totalChars, QString *error)
{
    if (out) {
        out->clear();
    }
    if (totalChars) {
        *totalChars = 0;
    }
    if (!validId(id)) {
        if (error) {
            *error = u"Invalid tool_result id."_s;
        }
        return false;
    }
    if (offset < 0) {
        if (error) {
            *error = u"offset must be >= 0."_s;
        }
        return false;
    }
    if (limit < 1) {
        if (error) {
            *error = u"limit must be >= 1."_s;
        }
        return false;
    }

    const QString path = QDir(directory()).filePath(id + u".txt"_s);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = u"Unknown or expired tool_result id."_s;
        }
        return false;
    }
    // Read at most 64 MiB; the store already caps at 32 MiB.
    constexpr qint64 kMaxRead = 64 * 1024 * 1024;
    if (file.size() > kMaxRead) {
        if (error) {
            *error = u"Stored tool result is too large to read."_s;
        }
        return false;
    }

    // The total is written next to the result at store time. Older stores do
    // not have it; in that case the stream below counts while it reads.
    qint64 knownTotal = -1;
    QFile meta(path + u".len"_s);
    if (meta.open(QIODevice::ReadOnly)) {
        bool ok = false;
        const qint64 value = meta.readAll().trimmed().toLongLong(&ok);
        if (ok && value >= 0) {
            knownTotal = value;
        }
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    QString page;
    qint64 total = 0;
    qint64 skipped = 0;
    while (!stream.atEnd()) {
        QString chunk = stream.read(64 * 1024);
        if (chunk.isEmpty() && stream.atEnd()) {
            break;
        }
        if (knownTotal < 0) {
            total += chunk.size();
        }
        if (skipped < offset) {
            const qint64 drop = qMin<qint64>(chunk.size(), offset - skipped);
            chunk.remove(0, static_cast<int>(drop));
            skipped += drop;
        }
        if (!chunk.isEmpty() && page.size() < limit) {
            page += chunk.left(limit - page.size());
        }
        // With a valid sidecar the page is complete without walking the rest
        // of a possibly 32 MiB result.
        if (knownTotal >= 0 && page.size() >= limit) {
            break;
        }
    }
    if (knownTotal >= 0) {
        total = knownTotal;
    }
    if (totalChars) {
        *totalChars = total;
    }
    if (out) {
        *out = page;
    }
    return true;
}

} // namespace KateAi
