/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "events.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace KateAi
{

class EventStore;

struct ReplayStep {
    QString stepId;
    QString turnId;
    QString assistantText;
    QList<QString> toolCallIds;
    QList<QString> toolNames;
};

struct ReplayReport {
    bool ok = false;
    QStringList errors;
    QList<ReplayStep> steps;
    int toolCalls = 0;
    int toolResults = 0;
    QString finalAssistant;
};

/**
 * Reconstructs a recorded turn from the event log without calling a model.
 *
 * Replay is the acceptance test for the substrate: every tool call must have
 * exactly one result, ids must pair, and the sequence must be monotonic.
 */
class Replay
{
public:
    static ReplayReport analyze(const QList<HarnessEvent> &events);
    static ReplayReport analyze(const EventStore &store, const QString &conversationId,
                                QString *error = nullptr);

    // JSONL export is a debugging artifact; the database remains primary.
    static bool exportJsonl(const QList<HarnessEvent> &events, const QString &path,
                            QString *error = nullptr);
};

} // namespace KateAi
