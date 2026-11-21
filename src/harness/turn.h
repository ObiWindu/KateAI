/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace KateAi
{

/**
 * Explicit state vocabulary for one turn.
 *
 * The legacy AgentLoop state enum still drives execution; this controller is
 * the stable vocabulary events, middleware and tests share. Terminal states
 * are never left.
 */
enum class TurnPhase {
    Idle,
    Starting,
    ModelWait,
    ToolWait,
    Verifying,
    Stopping,
    Completed,
    Failed,
    Interrupted,
};

QString turnPhaseId(TurnPhase phase);
bool isTerminalTurnPhase(TurnPhase phase);

struct TurnContext {
    QString conversationId;
    QString turnId;
    QString workspace;
    QString userText;
    QString modeId;
    TurnPhase phase = TurnPhase::Idle;
    int modelRequests = 0;
    int toolCalls = 0;
    int verificationAttempts = 0;
    int stopHookAttempts = 0;
    bool mutationOccurred = false;
    bool verificationPassed = false;
    QSet<QString> changedPaths;

    void begin(const QString &id, const QString &conversation, const QString &root,
               const QString &text, const QString &mode);
    void noteMutation(const QString &path);
    void resetAttempts();
};

class TurnController
{
public:
    void begin(TurnContext *context);
    bool transition(TurnContext *context, TurnPhase next, QString *error = nullptr);
    void complete(TurnContext *context);
    void fail(TurnContext *context);
    void interrupt(TurnContext *context);
    static bool canTransition(TurnPhase from, TurnPhase to);
};

} // namespace KateAi
