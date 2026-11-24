/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/harnessflags.h"
#include "harness/toolresults.h"
#include "agentloop.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

class TestHarnessFoundation : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void flagsDefaults()
    {
        HarnessFlags flags;
        QVERIFY(flags.stablePromptPrefix);
        QVERIFY(flags.resultBudget);
        QVERIFY(flags.contextReport);
        QVERIFY(flags.eventLog);
        QVERIFY(flags.middleware);
        QVERIFY(flags.verificationGate);
        QVERIFY(flags.policyV2);
        QVERIFY(flags.hooks);
        QVERIFY(flags.promptV2);
        QVERIFY(flags.contextV2);
        QVERIFY(flags.memoryV2);
        QVERIFY(flags.toolSearch);
        // 0.9 features are on at the 0.9 gate; telemetry remains opt-in.
        QVERIFY(flags.skills);
        QVERIFY(flags.agentsV2);
        QVERIFY(flags.inspector);
        QVERIFY(!flags.telemetry);
    }

    void flagsOverrides()
    {
        HarnessFlags flags;
        flags.applyOverrides(u"+middleware,-stablePromptPrefix, toolSearch ,unknown"_s);
        QVERIFY(flags.middleware);
        QVERIFY(flags.toolSearch);
        QVERIFY(!flags.stablePromptPrefix);
        // Unknown names are ignored.
        QVERIFY(!flags.enabled(u"unknown"_s));
        QVERIFY(flags.enabledNames().contains(u"middleware"_s));
    }

    void flagsEnvironment()
    {
        qputenv("KATEAI_HARNESS_FLAGS", "+eventLog,-resultBudget");
        const HarnessFlags flags = HarnessFlags::load();
        QVERIFY(flags.eventLog);
        QVERIFY(!flags.resultBudget);
        qunsetenv("KATEAI_HARNESS_FLAGS");
    }

    void toolResultsRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ToolResults::setDirectoryForTests(dir.path());

        QString text;
        for (int i = 0; i < 5000; ++i) {
            text += u"line %1: the quick brown fox jumps over the lazy dog\n"_s.arg(i);
        }

        const StoredToolResult stored = ToolResults::store(text, 100, 80);
        QVERIFY(!stored.id.isEmpty());
        QCOMPARE(stored.totalChars, text.size());
        QVERIFY(stored.preview.size() < text.size());
        QVERIFY(stored.preview.contains(u"read_tool_result"_s));
        QVERIFY(stored.preview.contains(stored.id));

        QString page;
        qint64 total = 0;
        QString error;
        QVERIFY2(ToolResults::read(stored.id, 0, 120, &page, &total, &error), qPrintable(error));
        QCOMPARE(total, text.size());
        QCOMPARE(page, text.left(120));

        QVERIFY(ToolResults::read(stored.id, 500, 40, &page, &total, &error));
        QCOMPARE(page, text.mid(500, 40));

        // Offsets past the end are clamped, not an error.
        QVERIFY(ToolResults::read(stored.id, text.size() + 100, 10, &page, &total, &error));
        QVERIFY(page.isEmpty());

        ToolResults::setDirectoryForTests(QString());
    }

    void toolResultsRejectInvalidId()
    {
        QString page;
        qint64 total = 0;
        QString error;
        QVERIFY(!ToolResults::read(u"../../etc/passwd"_s, 0, 10, &page, &total, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!ToolResults::read(u"short"_s, 0, 10, &page, &total, &error));
    }

    void toolResultsSmallTextHasNoFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ToolResults::setDirectoryForTests(dir.path());
        const StoredToolResult stored = ToolResults::store(u"tiny"_s);
        QVERIFY(stored.id.isEmpty());
        QCOMPARE(stored.preview, u"tiny"_s);
        ToolResults::setDirectoryForTests(QString());
    }

    void reportsAreAvailableOutsideATurn()
    {
        AgentLoop loop;
        const QString context = loop.contextReport();
        QVERIFY(context.contains(u"Context window"_s));
        QVERIFY(context.contains(u"Messages"_s));

        const QString usage = loop.usageReport();
        QVERIFY(usage.contains(u"Provider"_s));
        QVERIFY(usage.contains(u"Model requests"_s));
    }
};

QTEST_GUILESS_MAIN(TestHarnessFoundation)
#include "test_harness_foundation.moc"
