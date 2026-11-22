/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "replay.h"

#include "eventstore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

ReplayReport Replay::analyze(const QList<HarnessEvent> &events)
{
    ReplayReport report;
    QSet<QString> callIds;
    QSet<QString> resultIds;
    QString currentStep;
    QString currentTurn;
    qint64 lastSeq = 0;

    for (const HarnessEvent &event : events) {
        if (event.seq > 0 && event.seq <= lastSeq) {
            report.errors.append(u"Event sequence is not strictly increasing at %1."_s.arg(event.seq));
        }
        lastSeq = qMax(lastSeq, event.seq);

        if (event.type == EventTypes::StepStart) {
            ReplayStep step;
            step.stepId = event.stepId;
            step.turnId = event.turnId;
            report.steps.append(step);
            currentStep = event.stepId;
            currentTurn = event.turnId;
            continue;
        }

        if (event.type == EventTypes::AssistantMessage) {
            const QString text = event.payload.value(u"text"_s).toString();
            if (!text.isEmpty()) {
                if (!report.finalAssistant.isEmpty()) {
                    report.finalAssistant += u'\n';
                }
                report.finalAssistant += text;
            }
            ReplayStep *step = nullptr;
            if (!currentStep.isEmpty()) {
                for (auto it = report.steps.rbegin(); it != report.steps.rend(); ++it) {
                    if (it->stepId == currentStep) {
                        step = &(*it);
                        break;
                    }
                }
            }
            if (!step) {
                ReplayStep synthetic;
                synthetic.stepId = event.stepId;
                synthetic.turnId = event.turnId;
                report.steps.append(synthetic);
                step = &report.steps.last();
            }
            step->assistantText += text;
            for (const QJsonValue &value : event.payload.value(u"toolCalls"_s).toArray()) {
                const QJsonObject call = value.toObject();
                const QString id = call.value(u"id"_s).toString();
                const QString name = call.value(u"name"_s).toString();
                if (id.isEmpty()) {
                    report.errors.append(u"An assistant tool call has no id."_s);
                    continue;
                }
                if (callIds.contains(id)) {
                    report.errors.append(u"Tool call id %1 is declared twice."_s.arg(id));
                    continue;
                }
                callIds.insert(id);
                step->toolCallIds.append(id);
                step->toolNames.append(name);
                ++report.toolCalls;
            }
            continue;
        }

        if (event.type == EventTypes::ToolResult) {
            const QString id = event.payload.value(u"toolCallId"_s).toString();
            if (id.isEmpty()) {
                report.errors.append(u"A tool result has no tool-call id."_s);
                continue;
            }
            if (resultIds.contains(id)) {
                report.errors.append(u"Tool call %1 has more than one result."_s.arg(id));
                continue;
            }
            resultIds.insert(id);
            ++report.toolResults;
            if (!callIds.contains(id)) {
                report.errors.append(u"Tool result %1 has no matching call."_s.arg(id));
            }
        }
    }

    for (const QString &id : std::as_const(callIds)) {
        if (!resultIds.contains(id)) {
            report.errors.append(u"Tool call %1 never produced a result."_s.arg(id));
        }
    }
    report.ok = report.errors.isEmpty();
    return report;
}

ReplayReport Replay::analyze(const EventStore &store, const QString &conversationId,
                             QString *error)
{
    const QList<HarnessEvent> events = store.read(conversationId, 0, -1, error);
    return analyze(events);
}

bool Replay::exportJsonl(const QList<HarnessEvent> &events, const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    for (const HarnessEvent &event : events) {
        QJsonObject object;
        object.insert(u"conversationId"_s, event.conversationId);
        object.insert(u"seq"_s, event.seq);
        object.insert(u"ts"_s, event.timestampMs);
        object.insert(u"turnId"_s, event.turnId);
        object.insert(u"stepId"_s, event.stepId);
        object.insert(u"type"_s, event.type);
        object.insert(u"payload"_s, Events::redact(event.payload));
        object.insert(u"imported"_s, event.imported);
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
    }
    return true;
}

} // namespace KateAi
