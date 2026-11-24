/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "middleware.h"

#include "types.h"

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

MiddlewareResult MiddlewareResult::next()
{
    return {};
}

MiddlewareResult MiddlewareResult::deny(const QString &reason, const QString &ruleId)
{
    MiddlewareResult result;
    result.flow = MiddlewareFlow::Deny;
    result.reason = reason;
    result.ruleId = ruleId;
    return result;
}

MiddlewareResult MiddlewareResult::stop(const QString &reason, const QString &ruleId)
{
    MiddlewareResult result;
    result.flow = MiddlewareFlow::Stop;
    result.reason = reason;
    result.ruleId = ruleId;
    return result;
}

MiddlewareResult MiddlewareResult::retry(const QString &reason, const QString &ruleId)
{
    MiddlewareResult result;
    result.flow = MiddlewareFlow::Retry;
    result.reason = reason;
    result.ruleId = ruleId;
    return result;
}

MiddlewareResult TurnMiddleware::onTurnStart(TurnContext &)
{
    return MiddlewareResult::next();
}

MiddlewareResult TurnMiddleware::onStepStart(TurnContext &, int)
{
    return MiddlewareResult::next();
}

MiddlewareResult TurnMiddleware::onPreTool(TurnContext &, const ToolCall &, const QJsonObject &)
{
    return MiddlewareResult::next();
}

MiddlewareResult TurnMiddleware::onPostTool(TurnContext &, const ToolCall &, const ToolResult &)
{
    return MiddlewareResult::next();
}

MiddlewareResult TurnMiddleware::onStop(TurnContext &)
{
    return MiddlewareResult::next();
}

void TurnMiddleware::onTurnEnd(TurnContext &)
{
}

void MiddlewareChain::add(std::unique_ptr<TurnMiddleware> middleware)
{
    if (middleware) {
        m_middleware.push_back(std::move(middleware));
    }
}

void MiddlewareChain::clear()
{
    m_middleware.clear();
}

QStringList MiddlewareChain::ids() const
{
    QStringList out;
    for (const auto &item : m_middleware) {
        out.append(item->id());
    }
    return out;
}

bool MiddlewareChain::isEmpty() const
{
    return m_middleware.empty();
}

MiddlewareResult MiddlewareChain::runTurnStart(TurnContext &context)
{
    for (const auto &item : m_middleware) {
        const MiddlewareResult result = item->onTurnStart(context);
        if (!result.continues()) {
            return result;
        }
    }
    return MiddlewareResult::next();
}

MiddlewareResult MiddlewareChain::runStepStart(TurnContext &context, int stepNumber)
{
    ++context.modelRequests;
    for (const auto &item : m_middleware) {
        const MiddlewareResult result = item->onStepStart(context, stepNumber);
        if (!result.continues()) {
            return result;
        }
    }
    return MiddlewareResult::next();
}

MiddlewareResult MiddlewareChain::runPreTool(TurnContext &context, const ToolCall &call,
                                             const QJsonObject &arguments)
{
    for (const auto &item : m_middleware) {
        const MiddlewareResult result = item->onPreTool(context, call, arguments);
        if (!result.continues()) {
            return result;
        }
    }
    return MiddlewareResult::next();
}

MiddlewareResult MiddlewareChain::runPostTool(TurnContext &context, const ToolCall &call,
                                              const ToolResult &result)
{
    MiddlewareResult combined = MiddlewareResult::next();
    for (auto it = m_middleware.rbegin(); it != m_middleware.rend(); ++it) {
        const MiddlewareResult current = (*it)->onPostTool(context, call, result);
        if (combined.continues() && !current.continues()) {
            combined = current;
        }
    }
    return combined;
}

MiddlewareResult MiddlewareChain::runStop(TurnContext &context)
{
    for (const auto &item : m_middleware) {
        const MiddlewareResult result = item->onStop(context);
        if (!result.continues()) {
            return result;
        }
    }
    return MiddlewareResult::next();
}

void MiddlewareChain::runTurnEnd(TurnContext &context)
{
    for (auto it = m_middleware.rbegin(); it != m_middleware.rend(); ++it) {
        (*it)->onTurnEnd(context);
    }
}

BudgetMiddleware::BudgetMiddleware(int maxToolCalls, int maxModelRequests)
    : m_maxToolCalls(maxToolCalls)
    , m_maxModelRequests(maxModelRequests)
{
}

QString BudgetMiddleware::id() const
{
    return u"budget"_s;
}

MiddlewareResult BudgetMiddleware::onStepStart(TurnContext &context, int stepNumber)
{
    Q_UNUSED(context)
    if (m_maxModelRequests > 0 && stepNumber > m_maxModelRequests) {
        return MiddlewareResult::stop(
            u"Model-request budget exhausted (%1 requests)."_s.arg(m_maxModelRequests), id());
    }
    return MiddlewareResult::next();
}

MiddlewareResult BudgetMiddleware::onPreTool(TurnContext &context, const ToolCall &,
                                             const QJsonObject &)
{
    if (m_maxToolCalls > 0 && context.toolCalls >= m_maxToolCalls) {
        return MiddlewareResult::deny(
            u"Tool-call budget exhausted (%1 calls)."_s.arg(m_maxToolCalls), id());
    }
    ++context.toolCalls;
    return MiddlewareResult::next();
}

} // namespace KateAi
