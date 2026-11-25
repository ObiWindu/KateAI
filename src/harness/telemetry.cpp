/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "telemetry.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

QString hexId(const QString &seed, int bytes)
{
    const QByteArray hash =
        QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.left(bytes).toHex());
}

void insertIfPresent(QJsonObject *object, const QString &key, const QJsonObject &source,
                     const QString &sourceKey = QString())
{
    const QJsonValue value = source.value(sourceKey.isEmpty() ? key : sourceKey);
    if (!value.isUndefined() && !value.isNull()) {
        object->insert(key, value);
    }
}

QJsonObject boundedAttributes(const QString &type, const QJsonObject &payload,
                              bool captureContent)
{
    QJsonObject attributes;
    insertIfPresent(&attributes, u"role"_s, payload);
    insertIfPresent(&attributes, u"provider"_s, payload);
    insertIfPresent(&attributes, u"model"_s, payload);
    insertIfPresent(&attributes, u"mode"_s, payload);
    insertIfPresent(&attributes, u"permissionMode"_s, payload);
    insertIfPresent(&attributes, u"sandbox"_s, payload);
    insertIfPresent(&attributes, u"status"_s, payload);
    insertIfPresent(&attributes, u"detail"_s, payload);
    insertIfPresent(&attributes, u"decision"_s, payload);
    insertIfPresent(&attributes, u"ruleId"_s, payload);
    insertIfPresent(&attributes, u"reason"_s, payload);
    insertIfPresent(&attributes, u"tool"_s, payload);
    insertIfPresent(&attributes, u"name"_s, payload);
    insertIfPresent(&attributes, u"toolCallId"_s, payload);
    insertIfPresent(&attributes, u"ok"_s, payload);
    insertIfPresent(&attributes, u"cancelled"_s, payload);
    insertIfPresent(&attributes, u"messages"_s, payload);
    insertIfPresent(&attributes, u"estimatedTokens"_s, payload);
    insertIfPresent(&attributes, u"inputTokens"_s, payload);
    insertIfPresent(&attributes, u"outputTokens"_s, payload);
    insertIfPresent(&attributes, u"cachedInputTokens"_s, payload);
    insertIfPresent(&attributes, u"modelRequests"_s, payload);
    insertIfPresent(&attributes, u"toolCalls"_s, payload);
    insertIfPresent(&attributes, u"step"_s, payload);
    insertIfPresent(&attributes, u"beforeTokens"_s, payload);
    insertIfPresent(&attributes, u"afterTokens"_s, payload);
    insertIfPresent(&attributes, u"attempts"_s, payload);
    insertIfPresent(&attributes, u"taskId"_s, payload);
    insertIfPresent(&attributes, u"agentId"_s, payload);
    insertIfPresent(&attributes, u"agentName"_s, payload);
    insertIfPresent(&attributes, u"isolation"_s, payload);
    if (type == EventTypes::AssistantMessage) {
        attributes.insert(u"textChars"_s, payload.value(u"text"_s).toString().size());
        attributes.insert(u"toolCallCount"_s, payload.value(u"toolCalls"_s).toArray().size());
    } else if (type == EventTypes::ToolCall) {
        attributes.insert(u"argumentChars"_s,
                          payload.value(u"arguments"_s).toString().size());
    } else if (type == EventTypes::ToolResult) {
        attributes.insert(u"outputChars"_s, payload.value(u"output"_s).toString().size());
    } else if (type == EventTypes::TurnEnd) {
        attributes.insert(u"changedPathCount"_s,
                          payload.value(u"changedPaths"_s).toArray().size());
    } else if (type == EventTypes::Verification) {
        attributes.insert(u"commandCount"_s,
                          payload.value(u"commands"_s).toArray().size());
    }
    if (captureContent) {
        if (type == EventTypes::AssistantMessage) {
            attributes.insert(u"text"_s, payload.value(u"text"_s).toString().left(2000));
        } else if (type == EventTypes::ToolCall) {
            attributes.insert(u"arguments"_s,
                              payload.value(u"arguments"_s).toString().left(2000));
        } else if (type == EventTypes::ToolResult) {
            attributes.insert(u"output"_s, payload.value(u"output"_s).toString().left(2000));
        } else if (type == EventTypes::TurnStart) {
            attributes.insert(u"userText"_s, payload.value(u"userText"_s).toString().left(2000));
        }
    }
    return attributes;
}

} // namespace

QJsonObject Span::toJson() const
{
    QJsonObject object;
    object.insert(u"traceId"_s, traceId);
    object.insert(u"spanId"_s, spanId);
    object.insert(u"parentSpanId"_s, parentSpanId);
    object.insert(u"name"_s, name);
    object.insert(u"kind"_s, kind);
    object.insert(u"startMs"_s, startMs);
    object.insert(u"endMs"_s, endMs);
    object.insert(u"durationMs"_s, durationMs());
    object.insert(u"open"_s, open);
    object.insert(u"attributes"_s, attributes);
    return object;
}

QString Span::otlpTraceId() const
{
    return traceId.isEmpty() ? hexId(name, 16) : hexId(traceId, 16);
}

QString Span::otlpSpanId() const
{
    return spanId.isEmpty() ? hexId(name, 8) : hexId(spanId, 8);
}

QString Span::otlpParentSpanId() const
{
    return parentSpanId.isEmpty() ? QString() : hexId(parentSpanId, 8);
}

QList<Span> Telemetry::fromEvents(const QList<HarnessEvent> &events, bool captureContent)
{
    QList<Span> spans;
    QString traceId;
    int turnIndex = -1;
    int stepIndex = -1;
    QHash<QString, int> openTools;
    QHash<QString, int> openSubagents;
    qint64 lastTimestamp = 0;
    int syntheticSeq = 0;

    auto spanIdAt = [&spans](int index) {
        return index >= 0 && index < spans.size() ? spans.at(index).spanId : QString();
    };

    auto closeSpan = [&spans](int index, qint64 timestamp, const QJsonObject &extra = {}) {
        if (index < 0 || index >= spans.size()) {
            return;
        }
        Span &span = spans[index];
        if (!span.open) {
            return;
        }
        span.endMs = qMax(timestamp, span.startMs);
        span.open = false;
        for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
            span.attributes.insert(it.key(), it.value());
        }
    };

    auto closeOpen = [&](QHash<QString, int> *open, qint64 timestamp) {
        for (auto it = open->constBegin(); it != open->constEnd(); ++it) {
            closeSpan(it.value(), timestamp, QJsonObject{{u"status"_s, u"unfinished"_s}});
        }
        open->clear();
    };

    auto addSpan = [&](const QString &name, const QString &kind, const QString &parent,
                       qint64 start, const QJsonObject &attributes, bool open) {
        Span span;
        span.traceId = traceId.isEmpty() ? u"trace"_s : traceId;
        ++syntheticSeq;
        span.spanId = u"s%1"_s.arg(syntheticSeq);
        span.parentSpanId = parent;
        span.name = name;
        span.kind = kind;
        span.startMs = start;
        span.endMs = start;
        span.open = open;
        span.attributes = attributes;
        spans.append(span);
        return spans.size() - 1;
    };

    for (const HarnessEvent &event : events) {
        const qint64 timestamp = event.timestampMs > 0 ? event.timestampMs : lastTimestamp;
        lastTimestamp = qMax(lastTimestamp, timestamp);
        if (traceId.isEmpty() && !event.conversationId.isEmpty()) {
            traceId = event.conversationId;
            if (!event.turnId.isEmpty()) {
                traceId += u"/"_s + event.turnId;
            }
        }
        const QJsonObject attributes =
            boundedAttributes(event.type, event.payload, captureContent);

        if (event.type == EventTypes::TurnStart) {
            closeOpen(&openTools, timestamp);
            closeOpen(&openSubagents, timestamp);
            closeSpan(stepIndex, timestamp);
            closeSpan(turnIndex, timestamp);
            turnIndex = -1;
            stepIndex = -1;
            QString thisTrace = event.conversationId;
            if (thisTrace.isEmpty()) {
                thisTrace = QStringLiteral("trace");
            }
            if (!event.turnId.isEmpty()) {
                thisTrace += QStringLiteral("/") + event.turnId;
            }
            if (traceId.isEmpty()) {
                traceId = thisTrace;
            }
            turnIndex = addSpan(u"turn"_s, u"invoke_agent"_s, QString(), timestamp, attributes,
                                true);
            continue;
        }

        if (event.type == EventTypes::StepStart) {
            closeSpan(stepIndex, timestamp);
            stepIndex = -1;
            stepIndex = addSpan(u"step"_s, u"internal"_s, spanIdAt(turnIndex), timestamp,
                                attributes, true);
            continue;
        }

        if (event.type == EventTypes::TurnEnd) {
            closeOpen(&openTools, timestamp);
            closeOpen(&openSubagents, timestamp);
            closeSpan(stepIndex, timestamp);
            closeSpan(turnIndex, timestamp, attributes);
            turnIndex = -1;
            stepIndex = -1;
            continue;
        }

        if (event.type == EventTypes::ToolCall) {
            const QString toolId = event.payload.value(u"toolCallId"_s).toString();
            const QString name = event.payload.value(u"name"_s).toString();
            const QString spanName = name.isEmpty() ? u"tool"_s : u"tool/"_s + name;
            const int index = addSpan(spanName, u"execute_tool"_s, spanIdAt(stepIndex), timestamp,
                                      attributes, true);
            if (!toolId.isEmpty()) {
                openTools.insert(toolId, index);
            }
            continue;
        }

        if (event.type == EventTypes::ToolResult) {
            const QString toolId = event.payload.value(u"toolCallId"_s).toString();
            if (openTools.contains(toolId)) {
                closeSpan(openTools.take(toolId), timestamp, attributes);
            } else {
                addSpan(u"tool/result"_s, u"execute_tool"_s, spanIdAt(stepIndex), timestamp,
                        attributes, false);
                spans.last().endMs = timestamp;
            }
            continue;
        }

        if (event.type == EventTypes::SubagentStart) {
            const QString taskId = event.payload.value(u"taskId"_s).toString();
            const int index = addSpan(u"subagent"_s, u"invoke_agent"_s, spanIdAt(turnIndex),
                                      timestamp, attributes, true);
            if (!taskId.isEmpty()) {
                openSubagents.insert(taskId, index);
            }
            continue;
        }

        if (event.type == EventTypes::SubagentEnd) {
            const QString taskId = event.payload.value(u"taskId"_s).toString();
            if (openSubagents.contains(taskId)) {
                closeSpan(openSubagents.take(taskId), timestamp, attributes);
            } else {
                addSpan(u"subagent"_s, u"invoke_agent"_s, spanIdAt(turnIndex), timestamp,
                        attributes, false);
                spans.last().endMs = timestamp;
            }
            continue;
        }

        if (event.type == EventTypes::AssistantMessage) {
            const int index = addSpan(u"chat"_s, u"chat"_s, spanIdAt(stepIndex), timestamp,
                                      attributes, false);
            spans[index].endMs = timestamp;
            continue;
        }

        if (event.type == EventTypes::RequestHeaders) {
            const int index = addSpan(u"request"_s, u"chat"_s, spanIdAt(stepIndex), timestamp,
                                      attributes, false);
            spans[index].endMs = timestamp;
            continue;
        }

        QString name;
        QString kind = u"internal"_s;
        if (event.type == EventTypes::PermissionDecision) {
            name = u"permission"_s;
        } else if (event.type == EventTypes::PolicyDecision) {
            name = u"policy"_s;
        } else if (event.type == EventTypes::HookDecision) {
            name = u"hook"_s;
        } else if (event.type == EventTypes::Verification) {
            name = u"verification"_s;
            kind = u"execute_tool"_s;
        } else if (event.type == EventTypes::Compaction) {
            name = u"compaction"_s;
        } else if (event.type == EventTypes::Checkpoint) {
            name = u"checkpoint"_s;
        } else if (event.type == EventTypes::Notice) {
            name = u"notice"_s;
        }
        if (!name.isEmpty()) {
            const int parent = stepIndex >= 0 ? stepIndex : turnIndex;
            const int index = addSpan(name, kind, spanIdAt(parent), timestamp, attributes, false);
            spans[index].endMs = timestamp;
        }
    }

    const qint64 end = lastTimestamp;
    closeOpen(&openTools, end);
    closeOpen(&openSubagents, end);
    for (Span &span : spans) {
        if (span.open) {
            span.endMs = qMax(end, span.startMs);
            span.open = false;
        }
        if (span.traceId == u"trace"_s && !traceId.isEmpty()) {
            span.traceId = traceId;
        }
    }
    return spans;
}

QJsonArray Telemetry::toJsonArray(const QList<Span> &spans)
{
    QJsonArray array;
    for (const Span &span : spans) {
        array.append(span.toJson());
    }
    return array;
}

QJsonObject Telemetry::toJsonObject(const QList<Span> &spans, const QString &traceId)
{
    QString resolved = traceId;
    if (resolved.isEmpty() && !spans.isEmpty()) {
        resolved = spans.first().traceId;
    }
    QJsonObject object;
    object.insert(u"traceId"_s, resolved);
    object.insert(u"spans"_s, toJsonArray(spans));
    return object;
}

bool Telemetry::exportJsonl(const QList<Span> &spans, const QString &path, QString *error)
{
    if (path.isEmpty()) {
        if (error) {
            *error = u"No trace path was given."_s;
        }
        return false;
    }
    const QFileInfo info(path);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        if (error) {
            *error = u"Cannot create the trace directory %1."_s.arg(info.absolutePath());
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = u"Cannot write %1: %2"_s.arg(path, file.errorString());
        }
        return false;
    }
    for (const Span &span : spans) {
        file.write(QJsonDocument(span.toJson()).toJson(QJsonDocument::Compact));
        file.write("\n");
    }
    if (!file.commit()) {
        if (error) {
            *error = u"Cannot commit %1: %2"_s.arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

QString Telemetry::report(const QList<Span> &spans)
{
    if (spans.isEmpty()) {
        return u"No trace spans recorded.\n"_s;
    }
    QString text;
    text += u"Trace %1, %2 spans\n"_s.arg(spans.first().traceId).arg(spans.size());
    for (const Span &span : spans) {
        int depth = 0;
        QString parent = span.parentSpanId;
        while (!parent.isEmpty() && depth < 8) {
            ++depth;
            QString next;
            for (const Span &candidate : spans) {
                if (candidate.spanId == parent) {
                    next = candidate.parentSpanId;
                    break;
                }
            }
            parent = next;
        }
        text += u"%1%2 [%3] %4 ms%5\n"_s
                    .arg(QString(depth * 2, u' '))
                    .arg(span.name, span.kind)
                    .arg(span.durationMs())
                    .arg(span.open ? u" (unfinished)"_s : QString());
        for (auto it = span.attributes.constBegin(); it != span.attributes.constEnd(); ++it) {
            if (it.value().isString() || it.value().isBool() || it.value().isDouble()) {
                text += u"%1  %2=%3\n"_s
                            .arg(QString(depth * 2 + 2, u' '), it.key(),
                                 it.value().toVariant().toString());
            }
        }
    }
    return text;
}

} // namespace KateAi
