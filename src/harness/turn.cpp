/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "turn.h"

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QString turnPhaseId(TurnPhase phase)
{
    switch (phase) {
    case TurnPhase::Idle:
        return u"idle"_s;
    case TurnPhase::Starting:
        return u"starting"_s;
    case TurnPhase::ModelWait:
        return u"model-wait"_s;
    case TurnPhase::ToolWait:
        return u"tool-wait"_s;
    case TurnPhase::Verifying:
        return u"verifying"_s;
    case TurnPhase::Stopping:
        return u"stopping"_s;
    case TurnPhase::Completed:
        return u"completed"_s;
    case TurnPhase::Failed:
        return u"failed"_s;
    case TurnPhase::Interrupted:
        return u"interrupted"_s;
    }
    return u"idle"_s;
}

bool isTerminalTurnPhase(TurnPhase phase)
{
    return phase == TurnPhase::Completed || phase == TurnPhase::Failed
        || phase == TurnPhase::Interrupted;
}

void TurnContext::begin(const QString &id, const QString &conversation, const QString &root,
                        const QString &text, const QString &mode)
{
    turnId = id;
    conversationId = conversation;
    workspace = root;
    userText = text;
    modeId = mode;
    phase = TurnPhase::Starting;
    modelRequests = 0;
    toolCalls = 0;
    verificationAttempts = 0;
    stopHookAttempts = 0;
    mutationOccurred = false;
    verificationPassed = false;
    changedPaths.clear();
}

void TurnContext::noteMutation(const QString &path)
{
    mutationOccurred = true;
    if (!path.isEmpty()) {
        changedPaths.insert(path);
    }
}

void TurnContext::resetAttempts()
{
    verificationAttempts = 0;
    stopHookAttempts = 0;
    verificationPassed = false;
}

bool TurnController::canTransition(TurnPhase from, TurnPhase to)
{
    if (from == to) {
        return true;
    }
    if (isTerminalTurnPhase(from)) {
        return false;
    }
    switch (from) {
    case TurnPhase::Idle:
        return to == TurnPhase::Starting;
    case TurnPhase::Starting:
        return to == TurnPhase::ModelWait || to == TurnPhase::Failed || to == TurnPhase::Interrupted;
    case TurnPhase::ModelWait:
        return to == TurnPhase::ToolWait || to == TurnPhase::Verifying || to == TurnPhase::Stopping
            || to == TurnPhase::ModelWait || to == TurnPhase::Failed || to == TurnPhase::Interrupted;
    case TurnPhase::ToolWait:
        return to == TurnPhase::ModelWait || to == TurnPhase::Verifying
            || to == TurnPhase::Stopping || to == TurnPhase::Failed || to == TurnPhase::Interrupted;
    case TurnPhase::Verifying:
        return to == TurnPhase::ModelWait || to == TurnPhase::Stopping || to == TurnPhase::Failed
            || to == TurnPhase::Interrupted;
    case TurnPhase::Stopping:
        return to == TurnPhase::Completed || to == TurnPhase::Failed || to == TurnPhase::Interrupted;
    case TurnPhase::Completed:
    case TurnPhase::Failed:
    case TurnPhase::Interrupted:
        return false;
    }
    return false;
}

void TurnController::begin(TurnContext *context)
{
    if (!context) {
        return;
    }
    context->phase = TurnPhase::Starting;
}

bool TurnController::transition(TurnContext *context, TurnPhase next, QString *error)
{
    if (!context) {
        if (error) {
            *error = u"No turn context."_s;
        }
        return false;
    }
    if (!canTransition(context->phase, next)) {
        if (error) {
            *error = u"Cannot move a turn from %1 to %2."_s
                         .arg(turnPhaseId(context->phase), turnPhaseId(next));
        }
        return false;
    }
    context->phase = next;
    return true;
}

void TurnController::complete(TurnContext *context)
{
    if (context) {
        context->phase = TurnPhase::Completed;
    }
}

void TurnController::fail(TurnContext *context)
{
    if (context) {
        context->phase = TurnPhase::Failed;
    }
}

void TurnController::interrupt(TurnContext *context)
{
    if (context) {
        context->phase = TurnPhase::Interrupted;
    }
}

} // namespace KateAi
