/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "events.h"

#include <QJsonObject>
#include <QMutex>
#include <QString>

namespace KateAi
{

/**
 * Append-only JSONL audit trail for permission and policy decisions.
 *
 * The event log keeps the same decision in the conversation; the audit file is
 * the durable, cross-conversation record a user or administrator can inspect
 * without opening a transcript. Credentials are redacted on the way in.
 */
class AuditLog
{
public:
    static QString defaultPath();
    static void setPathForTests(const QString &path);

    static bool record(const QString &type, const QJsonObject &payload);
    static bool recordPermission(const QString &turnId, const QString &toolName,
                                 const QString &decision, const QString &ruleId,
                                 const QString &reason);
    static QStringList recentLines(int maxLines);

private:
    static QString s_pathOverride;
    static QMutex s_mutex;
};

} // namespace KateAi
