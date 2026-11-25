/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "events.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace KateAi
{

/**
 * One timed unit in a turn trace.
 *
 * Spans are derived from the durable event log, never from a separate log, so
 * the inspector, the replay tool and an exported trace always describe the
 * same turn. They contain sizes, statuses and token counts; message bodies and
 * tool output stay in the event log and are redacted there.
 */
struct Span {
    QString traceId;
    QString spanId;
    QString parentSpanId;
    QString name;
    QString kind = QStringLiteral("internal");
    qint64 startMs = 0;
    qint64 endMs = 0;
    bool open = false;
    QJsonObject attributes;

    qint64 durationMs() const
    {
        return endMs > startMs ? endMs - startMs : 0;
    }

    QJsonObject toJson() const;

    // OTLP identifiers are 16-byte trace ids and 8-byte span ids, hex encoded.
    QString otlpTraceId() const;
    QString otlpSpanId() const;
    QString otlpParentSpanId() const;
};

class Telemetry
{
public:
    // Builds a deterministic span tree from events. A missing end (a crashed
    // turn, a tool that never produced a result) leaves the span open and
    // clamped to the last observed timestamp. With `captureContent`, message
    // text and tool arguments/output are included (truncated); the default is
    // sizes and statuses only.
    static QList<Span> fromEvents(const QList<HarnessEvent> &events,
                                  bool captureContent = false);

    static QJsonArray toJsonArray(const QList<Span> &spans);
    static QJsonObject toJsonObject(const QList<Span> &spans, const QString &traceId = QString());

    static bool exportJsonl(const QList<Span> &spans, const QString &path,
                            QString *error = nullptr);

    // Human-readable waterfall used by :kateai trace and the inspector.
    static QString report(const QList<Span> &spans);
};

} // namespace KateAi
