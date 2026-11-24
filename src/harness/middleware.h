/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "turn.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

namespace KateAi
{

struct ToolCall;
struct ToolResult;

enum class MiddlewareFlow {
    Continue,
    Deny,
    Stop,
    Retry,
};

struct MiddlewareResult {
    MiddlewareFlow flow = MiddlewareFlow::Continue;
    QString reason;
    QString ruleId;

    static MiddlewareResult next();
    static MiddlewareResult deny(const QString &reason, const QString &ruleId = QString());
    static MiddlewareResult stop(const QString &reason, const QString &ruleId = QString());
    static MiddlewareResult retry(const QString &reason, const QString &ruleId = QString());
    bool continues() const
    {
        return flow == MiddlewareFlow::Continue;
    }
};

/**
 * One policy in the turn pipeline.
 *
 * Pre-events run in registration order and the first Deny/Stop/Retry wins.
 * Post-events run in reverse order so setup/cleanup pairs nest. Middleware
 * must not touch the UI; the loop translates a result into cards and events.
 */
class TurnMiddleware
{
public:
    virtual ~TurnMiddleware() = default;
    virtual QString id() const = 0;

    virtual MiddlewareResult onTurnStart(TurnContext &context);
    virtual MiddlewareResult onStepStart(TurnContext &context, int stepNumber);
    virtual MiddlewareResult onPreTool(TurnContext &context, const ToolCall &call,
                                       const QJsonObject &arguments);
    virtual MiddlewareResult onPostTool(TurnContext &context, const ToolCall &call,
                                        const ToolResult &result);
    virtual MiddlewareResult onStop(TurnContext &context);
    virtual void onTurnEnd(TurnContext &context);
};

class MiddlewareChain
{
public:
    void add(std::unique_ptr<TurnMiddleware> middleware);
    void clear();
    QStringList ids() const;
    bool isEmpty() const;

    MiddlewareResult runTurnStart(TurnContext &context);
    MiddlewareResult runStepStart(TurnContext &context, int stepNumber);
    MiddlewareResult runPreTool(TurnContext &context, const ToolCall &call,
                                const QJsonObject &arguments);
    MiddlewareResult runPostTool(TurnContext &context, const ToolCall &call,
                                 const ToolResult &result);
    MiddlewareResult runStop(TurnContext &context);
    void runTurnEnd(TurnContext &context);

private:
    std::vector<std::unique_ptr<TurnMiddleware>> m_middleware;
};

/**
 * Stops a turn after a configurable number of tool calls or model requests.
 * The loop still owns the legacy counters; this middleware makes the budget
 * testable in isolation and available to hosts that do not use AgentLoop.
 */
class BudgetMiddleware : public TurnMiddleware
{
public:
    BudgetMiddleware(int maxToolCalls = 0, int maxModelRequests = 0);
    QString id() const override;
    MiddlewareResult onStepStart(TurnContext &context, int stepNumber) override;
    MiddlewareResult onPreTool(TurnContext &context, const ToolCall &call,
                               const QJsonObject &arguments) override;

private:
    int m_maxToolCalls = 0;
    int m_maxModelRequests = 0;
};

} // namespace KateAi
