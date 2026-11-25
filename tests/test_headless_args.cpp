/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "headless/args.h"

#include <QTest>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

class TestHeadlessArgs : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesRunDefaults()
    {
        HeadlessOptions options;
        QString error;
        QVERIFY2(HeadlessArgs::parse({u"kateai-headless"_s, u"run"_s}, &options, &error),
                 qPrintable(error));
        QCOMPARE(options.mode, u"run"_s);
        QCOMPARE(options.permission, u"auto"_s);
        QCOMPARE(options.sandbox, u"workspace"_s);
        QVERIFY(options.verify);
        QVERIFY(options.persist);
        QVERIFY(!options.jsonl);
        QVERIFY(!options.quiet);
    }

    void parsesInlineAndSeparateValues()
    {
        HeadlessOptions options;
        QString error;
        const QStringList args{u"kateai-headless"_s,
                               u"run"_s,
                               u"--prompt=fix it"_s,
                               u"--workspace"_s,
                               u"/tmp/work"_s,
                               u"--provider=openai-compatible"_s,
                               u"--max-turns"_s,
                               u"4"_s,
                               u"--max-cost=0.25"_s,
                               u"--no-verify"_s,
                               u"--jsonl"_s};
        QVERIFY2(HeadlessArgs::parse(args, &options, &error), qPrintable(error));
        QCOMPARE(options.prompt, u"fix it"_s);
        QCOMPARE(options.workspace, u"/tmp/work"_s);
        QCOMPARE(options.provider, u"openai-compatible"_s);
        QCOMPARE(options.maxTurns, 4);
        QCOMPARE(options.maxCostUsd, 0.25);
        QVERIFY(!options.verify);
        QVERIFY(options.jsonl);
    }

    void rejectsUnknownOption()
    {
        HeadlessOptions options;
        QString error;
        QVERIFY(!HeadlessArgs::parse({u"kateai-headless"_s, u"--make-it-fast"_s}, &options, &error));
        QVERIFY(error.contains(u"--make-it-fast"_s));
    }

    void rejectsInvalidPermission()
    {
        HeadlessOptions options;
        QString error;
        QVERIFY(!HeadlessArgs::parse(
            {u"kateai-headless"_s, u"--permission"_s, u"yolo"_s}, &options, &error));
        QVERIFY(error.contains(u"permission"_s));
    }

    void rejectsInvalidNumbers()
    {
        HeadlessOptions options;
        QString error;
        QVERIFY(!HeadlessArgs::parse({u"kateai-headless"_s, u"--max-turns"_s, u"-2"_s},
                                     &options, &error));
        QString error2;
        QVERIFY(!HeadlessArgs::parse({u"kateai-headless"_s, u"--max-cost"_s, u"free"_s},
                                     &options, &error2));
    }

    void parsesReplayAndAcp()
    {
        HeadlessOptions options;
        QString error;
        QVERIFY(HeadlessArgs::parse(
            {u"kateai-headless"_s, u"replay"_s, u"--conversation"_s, u"conv-1"_s},
            &options, &error));
        QCOMPARE(options.mode, u"replay"_s);
        QCOMPARE(options.conversationId, u"conv-1"_s);

        HeadlessOptions acp;
        QVERIFY(HeadlessArgs::parse({u"kateai-headless"_s, u"acp"_s, u"--stdio"_s}, &acp, &error));
        QCOMPARE(acp.mode, u"acp"_s);
        QVERIFY(acp.stdio);
    }

    void providerMustBeKnown()
    {
        HeadlessOptions options;
        QString error;
        QVERIFY(!HeadlessArgs::parse(
            {u"kateai-headless"_s, u"--provider"_s, u"not-a-provider"_s}, &options, &error));
        QVERIFY(error.contains(u"Unknown provider"_s));
    }
};

QTEST_GUILESS_MAIN(TestHeadlessArgs)

#include "test_headless_args.moc"
