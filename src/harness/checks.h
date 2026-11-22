/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "middleware.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace KateAi
{

class Sandbox;

struct CheckCommand {
    QString command;
    // Empty means "always relevant". A non-empty list is a set of path globs
    // the check applies to.
    QStringList paths;
    int timeoutMs = 120000;

    QJsonObject toJson() const;
};

struct CheckPlan {
    QList<CheckCommand> commands;
    QStringList sources;
    QString note;

    bool isEmpty() const
    {
        return commands.isEmpty();
    }
};

struct CheckResult {
    QString command;
    bool ok = false;
    bool timedOut = false;
    int exitCode = -1;
    QString output;
    qint64 durationMs = 0;

    QString summary() const;
};

class CheckDiscovery
{
public:
    // The discovery order is explicit config, instruction-file commands,
    // build-system detection, then `git diff --check`.
    static CheckPlan discover(const QString &workspace, const QStringList &changedPaths = {});
    static QStringList parseInstructionCommands(const QString &text);
    // Exposed for tests and the inspector.
    static bool pathRelevant(const CheckCommand &command, const QStringList &changedPaths);
};

class CheckRunner
{
public:
    // Runs at most three commands and stops once the total budget is spent.
    // Commands use the caller's sandbox wrapper and scrubbed environment.
    static QList<CheckResult> run(const QString &workspace, const CheckPlan &plan,
                                  const Sandbox *sandbox, int totalTimeoutMs = 600000);
    static QString evidence(const QList<CheckResult> &results);
};

/**
 * The 0.7 verification gate. A turn that mutated files cannot complete until
 * the plan passes. A failure is returned as Retry with the output attached so
 * the loop can feed it back to the model.
 */
class VerificationMiddleware : public TurnMiddleware
{
public:
    VerificationMiddleware(QString workspace, const Sandbox *sandbox, int maxAttempts = 2);

    QString id() const override;
    MiddlewareResult onStop(TurnContext &context) override;

    void setPlan(const CheckPlan &plan);
    CheckPlan plan() const
    {
        return m_plan;
    }
    QList<CheckResult> lastResults() const
    {
        return m_results;
    }

private:
    QString m_workspace;
    const Sandbox *m_sandbox = nullptr;
    int m_maxAttempts = 2;
    CheckPlan m_plan;
    QList<CheckResult> m_results;
};

} // namespace KateAi
