/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace KateAi
{

// The model behind the current step. Single is the ordinary one-model loop;
// Planner and Reviewer are read-only, Executor owns the mode's normal tools.
enum class OrchestrationRole {
    Single,
    Planner,
    Executor,
    Reviewer,
};

QString orchestrationRoleId(OrchestrationRole role);
QString orchestrationRoleLabel(OrchestrationRole role);
// "planner"/"reviewer" are read-only roles.
bool orchestrationRoleIsReadOnly(OrchestrationRole role);

// Providers usable for either side of an orchestration pair. ACP agents and
// Telegram are excluded because they do not speak the API-key completion
// transports and cannot be routed through Settings.
bool providerSupportsOrchestration(Provider provider);
// A route is usable when the provider is API-key-backed, its key is present,
// and a model is selected. Fills `error` with a user-facing reason otherwise.
bool modelRouteUsable(const Settings &settings, const ModelRoute &route, QString *error = nullptr);
// A copy of `settings` with provider/model/effort replaced by the route.
Settings settingsForRoute(const Settings &settings, const ModelRoute &route);
// Providers that may be offered in the orchestration selectors.
QList<Provider> orchestrationProviders(const Settings &settings);

// A plan submitted by the planner through the internal submit_plan tool.
struct OrchestrationPlan {
    QString summary;
    QStringList steps;
    QStringList files;
    QStringList acceptance;
    QStringList replanIf;
    bool needsChanges = true;
    QString directAnswer;
    QString hash;
    int version = 0;
};

struct OrchestrationReview {
    bool approved = false;
    QString summary;
    QStringList fixes;
    QString askUser;
};

// Internal controller tools. They are advertised only to their role and never
// reach ToolRunner, so they cannot touch the workspace.
QString submitPlanToolName();
QString requestReplanToolName();
QString submitReviewToolName();
bool isOrchestrationTool(const QString &name);

QJsonObject submitPlanDefinition();
QJsonObject requestReplanDefinition();
QJsonObject submitReviewDefinition();
QJsonArray orchestrationToolDefinitions(OrchestrationRole role);

// Parsers validate shape and cap sizes; they do not invent fields.
bool parseSubmitPlan(const QJsonObject &arguments, OrchestrationPlan *plan, QString *error);
bool parseSubmitReview(const QJsonObject &arguments, OrchestrationReview *review, QString *error);
// request_replan is intentionally small: the loop only needs a reason and
// evidence, and the planner inspects the rest of the conversation itself.
bool parseRequestReplan(const QJsonObject &arguments, QString *reason, QString *error);

QString planHash(const OrchestrationPlan &plan);
QString renderPlanDirective(const OrchestrationPlan &plan);
QString renderReviewDirective(const OrchestrationPlan &plan, const QString &changedFiles,
                              const QString &verification, const QString &executorSummary,
                              const QString &fixes = QString());
QString renderHandoffDirective(const QString &reason, const QString &evidence,
                               const QString &changedFiles, const OrchestrationPlan &plan);

// The role-specific prose appended to the normal system prompt. The planner and
// reviewer must never be asked to edit or run commands.
QString orchestrationRolePrompt(OrchestrationRole role);

// True when a completed executor turn should be sent to the reviewer. `auto`
// reviews work that changed files or has a multi-step plan.
bool shouldReviewOrchestratedTurn(const OrchestrationSettings &settings, bool changedFiles,
                                  int planSteps);

} // namespace KateAi
