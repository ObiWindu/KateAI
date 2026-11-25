/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/cost.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

class TestHarnessCost : public QObject
{
    Q_OBJECT

private:
    ModelProfile pricedProfile() const
    {
        ModelProfile profile;
        profile.id = u"test-model"_s;
        profile.inputPrice = 3.0;
        profile.outputPrice = 15.0;
        profile.cacheReadPrice = 0.3;
        profile.cacheWritePrice = 3.75;
        return profile;
    }

private Q_SLOTS:
    void pricesInputOutputAndCache()
    {
        const CostBreakdown cost = CostBreakdown::compute(
            pricedProfile(), CostUsage{1000000, 200000, 500000, 100000});
        QVERIFY(cost.priced);
        QVERIFY(qAbs(cost.inputUsd - 3.0) < 1e-9);
        QVERIFY(qAbs(cost.outputUsd - 3.0) < 1e-9);
        QVERIFY(qAbs(cost.cacheReadUsd - 0.15) < 1e-9);
        QVERIFY(qAbs(cost.cacheWriteUsd - 0.375) < 1e-9);
        QVERIFY(qAbs(cost.totalUsd() - 6.525) < 1e-9);
    }

    void unknownPriceIsNotGuessed()
    {
        ModelProfile profile;
        profile.id = u"unknown"_s;
        const CostBreakdown cost = CostBreakdown::compute(profile, CostUsage{1000, 1000, 0, 0});
        QVERIFY(!cost.priced);
        QCOMPARE(cost.totalUsd(), 0.0);
        QCOMPARE(cost.usage.inputTokens, 1000);
    }

    void ledgerAggregatesAndSorts()
    {
        CostLedger ledger;
        ledger.setBudgetUsd(1.0);
        ledger.record(u"executor"_s, u"exec"_s, pricedProfile(), CostUsage{100000, 10000, 0, 0});
        ledger.record(u"planner"_s, u"plan"_s, pricedProfile(), CostUsage{100000, 10000, 0, 0});
        ledger.record(u"executor"_s, u"exec"_s, pricedProfile(), CostUsage{100000, 10000, 0, 0});

        const QList<CostLedger::Entry> entries = ledger.entries();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).key, u"executor"_s);
        QCOMPARE(entries.at(0).calls, 2);
        QCOMPARE(entries.at(1).key, u"planner"_s);
        QCOMPARE(ledger.total().usage.inputTokens, 300000);
        QVERIFY(ledger.overBudget());

        const QJsonObject json = ledger.toJson();
        QCOMPARE(json.value(u"entries"_s).toArray().size(), 2);
        QVERIFY(json.value(u"total"_s).toObject().value(u"overBudget"_s).toBool());
    }

    void subagentUsageIsAttributed()
    {
        CostLedger ledger;
        ledger.recordSubagent(u"task-1"_s, u"review"_s, pricedProfile(),
                              CostUsage{200000, 10000, 0, 0});
        ledger.recordSubagent(u"task-1"_s, u"review"_s, pricedProfile(),
                              CostUsage{0, 10000, 0, 0});
        const QJsonObject json = ledger.toJson();
        const QJsonArray subagents = json.value(u"subagents"_s).toArray();
        QCOMPARE(subagents.size(), 1);
        QCOMPARE(subagents.first().toObject().value(u"calls"_s).toInt(), 2);
        QCOMPARE(subagents.first().toObject().value(u"usage"_s).toObject()
                     .value(u"inputTokens"_s).toInt(), 200000);
    }

    void formatIsLocaleIndependent()
    {
        QCOMPARE(CostBreakdown::formatUsd(0.0), u"$0.0000"_s);
        QCOMPARE(CostBreakdown::formatUsd(0.00001), u"<$0.0001"_s);
        QCOMPARE(CostBreakdown::formatUsd(1.23456), u"$1.2346"_s);
    }
};

QTEST_GUILESS_MAIN(TestHarnessCost)

#include "test_harness_cost.moc"
