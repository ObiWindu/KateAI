/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "audit.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QStringList>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QString AuditLog::s_pathOverride;
QMutex AuditLog::s_mutex;

QString AuditLog::defaultPath()
{
    if (!s_pathOverride.isEmpty()) {
        return s_pathOverride;
    }
    // A caller that isolated the session database (a test, a sandboxed host)
    // gets its audit trail beside it instead of in the user's real data dir.
    const QByteArray sessionDb = qgetenv("KATEAI_SESSION_DB");
    if (!sessionDb.isEmpty()) {
        return QFileInfo(QString::fromLocal8Bit(sessionDb)).absolutePath()
            + u"/audit.jsonl"_s;
    }
    QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data.isEmpty()) {
        data = QDir::home().filePath(u".local/share"_s);
    }
    return data + u"/kateai/audit.jsonl"_s;
}

void AuditLog::setPathForTests(const QString &path)
{
    QMutexLocker locker(&s_mutex);
    s_pathOverride = path;
}

bool AuditLog::record(const QString &type, const QJsonObject &payload)
{
    QMutexLocker locker(&s_mutex);
    const QString path = defaultPath();
    if (path.isEmpty()) {
        return false;
    }
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return false;
    }

    QJsonObject entry = Events::redact(payload);
    entry.insert(u"type"_s, type);
    entry.insert(u"ts"_s, QDateTime::currentMSecsSinceEpoch());
    const QByteArray line = QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
    return file.write(line) == line.size();
}

bool AuditLog::recordPermission(const QString &turnId, const QString &toolName,
                                const QString &decision, const QString &ruleId,
                                const QString &reason)
{
    QJsonObject payload;
    payload.insert(u"turnId"_s, turnId);
    payload.insert(u"tool"_s, toolName);
    payload.insert(u"decision"_s, decision);
    payload.insert(u"ruleId"_s, ruleId);
    payload.insert(u"reason"_s, reason);
    return record(u"permission"_s, payload);
}

QStringList AuditLog::recentLines(int maxLines)
{
    QMutexLocker locker(&s_mutex);
    QFile file(defaultPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QStringList lines = QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts);
    if (maxLines <= 0 || lines.size() <= maxLines) {
        return lines;
    }
    return lines.mid(lines.size() - maxLines);
}

} // namespace KateAi
