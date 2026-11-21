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
#include <QStandardPaths>
#include <QUuid>

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
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(u"tool-results"_s);
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
    if (file.write(stored.toUtf8()) < 0) {
        file.close();
        QFile::remove(path);
        result.preview = text.left(head + tail);
        return result;
    }
    file.close();

    result.id = id;
    result.totalChars = stored.size();
    const qsizetype omitted = stored.size() - head - tail;
    result.preview = text.left(head)
        + u"\n\n… [tool output truncated: showing the first %1 and last %2 of %3 characters; %4 omitted] …\n"_s
              .arg(head).arg(tail).arg(stored.size()).arg(omitted)
        + u"Full output: tool_result id=" + id
        + u" (call read_tool_result with this id and optional offset/limit)\n\n"_s
        + text.right(tail);
    return result;
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
    const QString text = QString::fromUtf8(file.readAll());
    if (totalChars) {
        *totalChars = text.size();
    }
    const qint64 start = qMin<qint64>(offset, text.size());
    const qint64 count = qMin<qint64>(limit, text.size() - start);
    if (out) {
        *out = text.mid(static_cast<int>(start), static_cast<int>(count));
    }
    return true;
}

} // namespace KateAi
