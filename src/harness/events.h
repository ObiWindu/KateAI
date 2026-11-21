/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace KateAi
{

/**
 * Durable events of a conversation.
 *
 * The event log is the append-only source of truth. The message projection is
 * rebuilt from it; the legacy SessionData payload remains the fast-load path
 * for one more release so a downgrade keeps working.
 */
namespace EventTypes
{
inline const QString TurnStart = QStringLiteral("turn/start");
inline const QString TurnEnd = QStringLiteral("turn/end");
inline const QString StepStart = QStringLiteral("step/start");
inline const QString RequestHeaders = QStringLiteral("request/headers");
inline const QString AssistantMessage = QStringLiteral("assistant/message");
inline const QString ToolCall = QStringLiteral("tool/call");
inline const QString ToolResult = QStringLiteral("tool/result");
inline const QString PermissionDecision = QStringLiteral("permission/decision");
inline const QString PolicyDecision = QStringLiteral("policy/decision");
inline const QString HookDecision = QStringLiteral("hook/decision");
inline const QString Checkpoint = QStringLiteral("checkpoint");
inline const QString Compaction = QStringLiteral("compaction");
inline const QString Notice = QStringLiteral("notice");
inline const QString Verification = QStringLiteral("verification");
inline const QString Message = QStringLiteral("message");
} // namespace EventTypes

struct HarnessEvent {
    QString conversationId;
    qint64 seq = 0;
    qint64 timestampMs = 0;
    QString turnId;
    QString stepId;
    QString type;
    QJsonObject payload;
    bool imported = false;
};

class Events
{
public:
    static HarnessEvent make(const QString &type, const QJsonObject &payload = {},
                             const QString &turnId = QString(), const QString &stepId = QString());
    static HarnessEvent make(const QString &type, qint64 timestampMs, const QJsonObject &payload,
                             const QString &turnId = QString(), const QString &stepId = QString());

    // Removes credentials and credential-shaped values before an event is
    // written. Key matching is on the JSON key name; value matching catches
    // bearer tokens, provider key prefixes and secret environment assignments.
    static QJsonObject redact(const QJsonObject &payload);
    static QString redactText(const QString &text);

    static QByteArray encode(const QJsonObject &payload);
    static QJsonObject decode(const QByteArray &data);
};

/**
 * Turns events into the message rows the transcript renders.
 *
 * Keeping this a pure function makes replay tests deterministic and lets the
 * event store rebuild its projection after a crash.
 */
class EventProjection
{
public:
    static void apply(const HarnessEvent &event, QList<ChatMessage> *messages);
    static QList<ChatMessage> build(const QList<HarnessEvent> &events);
};

} // namespace KateAi
