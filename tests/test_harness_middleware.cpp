/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/middleware.h"
#include "harness/turn.h"
#include "types.h"

#include <QJsonObject>
#include <QStringList>
#include <QTest>

#include <memory>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

namespace
{

class RecordingMiddleware : public TurnMiddleware
{
public:
    RecordingMiddleware(QString name, QStringList *log, MiddlewareFlow flow = MiddlewareFlow::Continue)
        : m_name(std::move(name))
        , m_log(log)
        , m_flow(flow)
    {
    }

    QString id() const override
    {
        return m_name;
    }

    MiddlewareResult onTurnStart(TurnContext &) override
    {
        m_log->append(m_name + u":start"_s);
        return m_flow == MiddlewareFlow::Deny ? MiddlewareResult::deny(u"denied"_s, m_name)
                                              : MiddlewareResult::next();
    }

    MiddlewareResult onPreTool(TurnContext &context, const ToolCall &, const QJsonObject &) override
    {
        m_log->append(m_name + u":pre"_s);
        ++context.toolCalls;
        return m_flow == MiddlewareFlow::Stop ? MiddlewareResult::stop(u"stopped"_s, m_name)
                                              : MiddlewareResult::next();
    }

    MiddlewareResult onPostTool(TurnContext &, const ToolCall &, const ToolResult &) override
    {
        m_log->append(m_name + u":post"_s);
        return MiddlewareResult::next();
    }

    void onTurnEnd(TurnContext &) override
    {
        m_log->append(m_name + u":end"_s);
    }

private:
    QString m_name;
    QStringList *m_log;
    MiddlewareFlow m_flow;
};

} // namespace

class TestHarnessMiddleware : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void turnTransitionsAreEnforced()
    {
        TurnController controller;
        TurnContext context;
        context.begin(u"turn"_s, u"conv"_s, u"/tmp"_s, u"fix"_s, u"code"_s);
        QCOMPARE(context.phase, TurnPhase::Starting);

        QVERIFY(controller.transition(&context, TurnPhase::ModelWait));
        QVERIFY(controller.transition(&context, TurnPhase::ToolWait));
        QVERIFY(controller.transition(&context, TurnPhase::Verifying));
        QVERIFY(controller.transition(&context, TurnPhase::ModelWait));
        QVERIFY(controller.transition(&context, TurnPhase::Stopping));
        controller.complete(&context);
        QCOMPARE(context.phase, TurnPhase::Completed);

        QString error;
        QVERIFY(!controller.transition(&context, TurnPhase::ModelWait, &error));
        QVERIFY(error.contains(u"Cannot move a turn"_s));
        QVERIFY(isTerminalTurnPhase(context.phase));
    }

    void chainRunsPreInOrderAndPostInReverse()
    {
        QStringList log;
        MiddlewareChain chain;
        chain.add(std::make_unique<RecordingMiddleware>(u"one"_s, &log));
        chain.add(std::make_unique<RecordingMiddleware>(u"two"_s, &log));
        QCOMPARE(chain.ids(), QStringList({u"one"_s, u"two"_s}));

        TurnContext context;
        context.begin(u"turn"_s, u"conv"_s, u"/tmp"_s, u"fix"_s, u"code"_s);
        QVERIFY(chain.runTurnStart(context).continues());

        ToolCall call;
        call.name = u"read_file"_s;
        QVERIFY(chain.runPreTool(context, call, {}).continues());
        QVERIFY(chain.runPostTool(context, call, {}).continues());
        chain.runTurnEnd(context);

        QCOMPARE(log, QStringList({u"one:start"_s, u"two:start"_s, u"one:pre"_s, u"two:pre"_s,
                                   u"two:post"_s, u"one:post"_s, u"two:end"_s, u"one:end"_s}));
    }

    void firstStopShortCircuits()
    {
        QStringList log;
        MiddlewareChain chain;
        chain.add(std::make_unique<RecordingMiddleware>(u"one"_s, &log));
        chain.add(std::make_unique<RecordingMiddleware>(u"two"_s, &log, MiddlewareFlow::Stop));
        chain.add(std::make_unique<RecordingMiddleware>(u"three"_s, &log));

        TurnContext context;
        context.begin(u"turn"_s, u"conv"_s, u"/tmp"_s, u"fix"_s, u"code"_s);
        const MiddlewareResult result = chain.runPreTool(context, ToolCall(), {});
        QCOMPARE(result.flow, MiddlewareFlow::Stop);
        QCOMPARE(result.ruleId, u"two"_s);
        QVERIFY(!log.contains(u"three:pre"_s));
    }

    void budgetStopsAtTheLimit()
    {
        TurnContext context;
        context.begin(u"turn"_s, u"conv"_s, u"/tmp"_s, u"fix"_s, u"code"_s);
        BudgetMiddleware budget(2, 1);
        ToolCall call;
        call.name = u"read_file"_s;

        QVERIFY(budget.onStepStart(context, 1).continues());
        const MiddlewareResult secondStep = budget.onStepStart(context, 2);
        QCOMPARE(secondStep.flow, MiddlewareFlow::Stop);

        QVERIFY(budget.onPreTool(context, call, {}).continues());
        QVERIFY(budget.onPreTool(context, call, {}).continues());
        QCOMPARE(budget.onPreTool(context, call, {}).flow, MiddlewareFlow::Deny);
    }
};

QTEST_GUILESS_MAIN(TestHarnessMiddleware)
#include "test_harness_middleware.moc"
