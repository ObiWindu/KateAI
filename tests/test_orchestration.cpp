/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "orchestration.h"
#include "types.h"

#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestOrchestration : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void providerCapability()
    {
        QVERIFY(providerSupportsOrchestration(Provider::OpenAI));
        QVERIFY(providerSupportsOrchestration(Provider::DeepSeek));
        QVERIFY(!providerSupportsOrchestration(Provider::Acp));
        QVERIFY(!providerSupportsOrchestration(Provider::Telegram));
    }

    void routeValidation()
    {
        Settings settings;
        settings.openaiApiKey = u"sk-test"_s;
        ModelRoute route;
        route.provider = Provider::OpenAI;
        route.model = u"gpt-test"_s;
        QString error;
        QVERIFY2(modelRouteUsable(settings, route, &error), qPrintable(error));

        route.model.clear();
        QVERIFY(!modelRouteUsable(settings, route, &error));
        QVERIFY(error.contains(u"model"_s, Qt::CaseInsensitive));

        route.model = u"gpt-test"_s;
        route.provider = Provider::DeepSeek;
        QVERIFY(!modelRouteUsable(settings, route, &error));
        QVERIFY(error.contains(u"API key"_s, Qt::CaseInsensitive));

        route.provider = Provider::Acp;
        QVERIFY(!modelRouteUsable(settings, route, &error));
    }

    void routeOverridesOnlyItsProvider()
    {
        Settings settings;
        settings.provider = Provider::DeepSeek;
        settings.deepseekModel = u"deepseek-chat"_s;
        ModelRoute route;
        route.provider = Provider::OpenAI;
        route.model = u"gpt-5"_s;
        route.effort = u"high"_s;
        const Settings routed = settingsForRoute(settings, route);
        QCOMPARE(routed.provider, Provider::OpenAI);
        QCOMPARE(routed.openaiModel, u"gpt-5"_s);
        QCOMPARE(routed.deepseekModel, u"deepseek-chat"_s);
        QCOMPARE(routed.reasoningEffort, u"high"_s);
    }

    void planParsing()
    {
        QJsonObject args{
            {u"summary"_s, u"Fix the parser"_s},
            {u"needs_changes"_s, true},
            {u"steps"_s, QJsonArray{
                 QJsonObject{{u"description"_s, u"Read the parser"_s},
                             {u"files"_s, QJsonArray{u"src/parser.cpp"_s}}},
                 u"Add a regression test"_s,
             }},
            {u"acceptance"_s, QJsonArray{u"ctest passes"_s}},
            {u"replan_if"_s, QJsonArray{u"A second verification fails"_s}},
        };
        OrchestrationPlan plan;
        QString error;
        QVERIFY2(parseSubmitPlan(args, &plan, &error), qPrintable(error));
        QCOMPARE(plan.steps.size(), 2);
        QCOMPARE(plan.acceptance.size(), 1);
        QVERIFY(!plan.hash.isEmpty());
        QVERIFY(plan.needsChanges);

        args[u"needs_changes"_s] = false;
        args.remove(u"steps"_s);
        args[u"direct_answer"_s] = u"No change needed"_s;
        QVERIFY2(parseSubmitPlan(args, &plan, &error), qPrintable(error));
        QVERIFY(!plan.needsChanges);
        QCOMPARE(plan.directAnswer, u"No change needed"_s);

        QJsonObject bad{{u"summary"_s, u"empty"_s}, {u"needs_changes"_s, true}};
        QVERIFY(!parseSubmitPlan(bad, &plan, &error));
    }

    void reviewParsing()
    {
        OrchestrationReview review;
        QString error;
        QVERIFY(parseSubmitReview(QJsonObject{{u"approved"_s, true},
                                               {u"summary"_s, u"looks good"_s}},
                                  &review, &error));
        QVERIFY(review.approved);

        QVERIFY(!parseSubmitReview(QJsonObject{{u"approved"_s, false},
                                                {u"summary"_s, u"no fixes given"_s}},
                                   &review, &error));
        QVERIFY(parseSubmitReview(QJsonObject{{u"approved"_s, false},
                                               {u"fixes"_s, QJsonArray{u"Add a test"_s}}},
                                  &review, &error));
        QCOMPARE(review.fixes.size(), 1);
    }

    void reviewPolicy()
    {
        OrchestrationSettings settings;
        settings.reviewMode = u"auto"_s;
        QVERIFY(!shouldReviewOrchestratedTurn(settings, false, 1));
        QVERIFY(shouldReviewOrchestratedTurn(settings, true, 1));
        QVERIFY(shouldReviewOrchestratedTurn(settings, false, 3));
        settings.reviewMode = u"off"_s;
        QVERIFY(!shouldReviewOrchestratedTurn(settings, true, 10));
        settings.reviewMode = u"always"_s;
        QVERIFY(shouldReviewOrchestratedTurn(settings, false, 0));
    }

    void toolDefinitionsAreRoleScoped()
    {
        const QJsonArray planner = orchestrationToolDefinitions(OrchestrationRole::Planner);
        QCOMPARE(planner.size(), 1);
        QCOMPARE(planner.first().toObject().value(u"function"_s).toObject()
                     .value(u"name"_s).toString(), submitPlanToolName());
        const QJsonArray executor = orchestrationToolDefinitions(OrchestrationRole::Executor);
        QCOMPARE(executor.first().toObject().value(u"function"_s).toObject()
                     .value(u"name"_s).toString(), requestReplanToolName());
        const QJsonArray reviewer = orchestrationToolDefinitions(OrchestrationRole::Reviewer);
        QCOMPARE(reviewer.first().toObject().value(u"function"_s).toObject()
                     .value(u"name"_s).toString(), submitReviewToolName());
        QVERIFY(orchestrationToolDefinitions(OrchestrationRole::Single).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestOrchestration)
#include "test_orchestration.moc"
