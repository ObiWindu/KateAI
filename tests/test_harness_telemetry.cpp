/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/events.h"
#include "harness/otlp.h"
#include "harness/telemetry.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

class TestHarnessTelemetry : public QObject
{
    Q_OBJECT

private:
    static HarnessEvent event(const QString &type, qint64 timestamp, const QJsonObject &payload,
                              const QString &turn = u"turn-1"_s,
                              const QString &step = u"step-1"_s)
    {
        HarnessEvent value = Events::make(type, timestamp, payload, turn, step);
        value.conversationId = u"conv-1"_s;
        return value;
    }

private Q_SLOTS:
    void buildsTurnStepAndToolSpans()
    {
        QList<HarnessEvent> events;
        events.append(event(EventTypes::TurnStart, 1000,
                            QJsonObject{{u"permissionMode"_s, u"ask"_s}}));
        events.append(event(EventTypes::StepStart, 1010, QJsonObject{{u"step"_s, 1}}));
        events.append(event(EventTypes::RequestHeaders, 1020,
                            QJsonObject{{u"estimatedTokens"_s, 1200}}));
        events.append(event(EventTypes::ToolCall, 1030,
                            QJsonObject{{u"toolCallId"_s, u"c1"_s},
                                        {u"name"_s, u"read_file"_s},
                                        {u"arguments"_s, u"{\"path\":\"a.cpp\"}"_s}}));
        events.append(event(EventTypes::ToolResult, 1060,
                            QJsonObject{{u"toolCallId"_s, u"c1"_s},
                                        {u"name"_s, u"read_file"_s},
                                        {u"ok"_s, true},
                                        {u"output"_s, u"contents"_s}}));
        events.append(event(EventTypes::AssistantMessage, 1070,
                            QJsonObject{{u"text"_s, u"Done."_s}}));
        events.append(event(EventTypes::TurnEnd, 1100,
                            QJsonObject{{u"status"_s, u"completed"_s},
                                        {u"modelRequests"_s, 1},
                                        {u"toolCalls"_s, 1}}));

        const QList<Span> spans = Telemetry::fromEvents(events);
        QVERIFY(spans.size() >= 5);
        QCOMPARE(spans.first().name, u"turn"_s);
        QCOMPARE(spans.first().durationMs(), 100);
        QVERIFY(!spans.first().open);

        int tool = -1;
        for (int i = 0; i < spans.size(); ++i) {
            if (spans.at(i).name == u"tool/read_file"_s) {
                tool = i;
                break;
            }
        }
        QVERIFY(tool >= 0);
        QCOMPARE(spans.at(tool).durationMs(), 30);
        QCOMPARE(spans.at(tool).kind, u"execute_tool"_s);
        QCOMPARE(spans.at(tool).attributes.value(u"ok"_s).toBool(), true);

        const QJsonObject json = Telemetry::toJsonObject(spans, u"conv-1/turn-1"_s);
        QCOMPARE(json.value(u"spans"_s).toArray().size(), spans.size());
        QCOMPARE(json.value(u"traceId"_s).toString(), u"conv-1/turn-1"_s);
    }

    void unmatchedCallsStayOpenAndAreClamped()
    {
        QList<HarnessEvent> events;
        events.append(event(EventTypes::TurnStart, 1000, {}));
        events.append(event(EventTypes::ToolCall, 1010,
                            QJsonObject{{u"toolCallId"_s, u"missing"_s},
                                        {u"name"_s, u"bash"_s}}));

        const QList<Span> spans = Telemetry::fromEvents(events);
        QVERIFY(!spans.isEmpty());
        QVERIFY(spans.first().endMs >= spans.first().startMs);
    }

    void otlpPayloadUsesProtocolShape()
    {
        QList<HarnessEvent> events;
        events.append(event(EventTypes::TurnStart, 1000, {}));
        events.append(event(EventTypes::TurnEnd, 1050,
                            QJsonObject{{u"status"_s, u"completed"_s}}));
        const QList<Span> spans = Telemetry::fromEvents(events);
        const QJsonObject payload = OtlpExporter::payload(spans, u"kateai"_s, u"1.0.0"_s);

        const QJsonArray resourceSpans = payload.value(u"resourceSpans"_s).toArray();
        QCOMPARE(resourceSpans.size(), 1);
        const QJsonObject resource = resourceSpans.first().toObject();
        const QJsonArray spansArray = resource.value(u"scopeSpans"_s).toArray()
                                          .first().toObject().value(u"spans"_s).toArray();
        QCOMPARE(spansArray.size(), spans.size());
        const QJsonObject first = spansArray.first().toObject();
        QCOMPARE(first.value(u"traceId"_s).toString().size(), 32);
        QCOMPARE(first.value(u"spanId"_s).toString().size(), 16);
        QVERIFY(first.contains(u"startTimeUnixNano"_s));

        OtlpExporter exporter;
        QVERIFY(!exporter.isConfigured());
        QCOMPARE(exporter.exportSpans(spans), 0);
    }

    void exportsJsonl()
    {
        QList<HarnessEvent> events;
        events.append(event(EventTypes::TurnStart, 1000, {}));
        events.append(event(EventTypes::TurnEnd, 1010, {}));
        const QList<Span> spans = Telemetry::fromEvents(events);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(u"trace.jsonl"_s);
        QString error;
        QVERIFY2(Telemetry::exportJsonl(spans, path, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = file.readAll().split('\n');
        QVERIFY(lines.size() >= 2);
        QVERIFY(lines.first().startsWith('{'));
    }

    void missingEndLeavesLastTimestamp()
    {
        QList<HarnessEvent> events;
        events.append(event(EventTypes::TurnStart, 1000, {}));
        const QList<Span> spans = Telemetry::fromEvents(events);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans.first().endMs, 1000);
        QVERIFY(!spans.first().open);
    }

    void contentCaptureIsExplicit()
    {
        QList<HarnessEvent> events;
        events.append(event(EventTypes::ToolCall, 1000,
                            QJsonObject{{u"toolCallId"_s, u"c1"_s},
                                        {u"name"_s, u"read_file"_s},
                                        {u"arguments"_s, u"{\"path\":\"secret.txt\"}"_s}}));
        const QList<Span> without = Telemetry::fromEvents(events);
        QVERIFY(!without.first().attributes.contains(u"arguments"_s));
        QVERIFY(without.first().attributes.contains(u"argumentChars"_s));

        const QList<Span> with = Telemetry::fromEvents(events, true);
        QVERIFY(with.first().attributes.value(u"arguments"_s).toString()
                    .contains(u"secret.txt"_s));
    }

    void longLogStaysLinear()
    {
        QList<HarnessEvent> events;
        events.reserve(4001);
        events.append(event(EventTypes::TurnStart, 1000, {}));
        for (int i = 0; i < 2000; ++i) {
            const qint64 start = 1001 + i * 4;
            events.append(event(EventTypes::StepStart, start,
                                QJsonObject{{u"step"_s, i}}));
            events.append(event(EventTypes::ToolCall, start + 1,
                                QJsonObject{{u"toolCallId"_s, u"c%1"_s.arg(i)},
                                            {u"name"_s, u"read_file"_s}}));
            events.append(event(EventTypes::ToolResult, start + 2,
                                QJsonObject{{u"toolCallId"_s, u"c%1"_s.arg(i)},
                                            {u"ok"_s, true}}));
        }
        events.append(event(EventTypes::TurnEnd, 100000, {}));

        QElapsedTimer timer;
        timer.start();
        const QList<Span> spans = Telemetry::fromEvents(events);
        const qint64 elapsed = timer.elapsed();
        QVERIFY(spans.size() >= 4000);
        QVERIFY2(elapsed < 5000, qPrintable(u"%1 ms"_s.arg(elapsed)));
    }
};

QTEST_GUILESS_MAIN(TestHarnessTelemetry)

#include "test_harness_telemetry.moc"
