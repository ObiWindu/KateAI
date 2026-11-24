/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "orchestration.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <KLocalizedString>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace
{

QJsonObject functionSchema(const QString &name, const QString &description,
                           const QJsonObject &properties, const QJsonArray &required)
{
    QJsonObject parameters;
    parameters.insert(u"type"_s, u"object"_s);
    parameters.insert(u"properties"_s, properties);
    parameters.insert(u"required"_s, required);

    QJsonObject function;
    function.insert(u"name"_s, name);
    function.insert(u"description"_s, description);
    function.insert(u"parameters"_s, parameters);

    QJsonObject tool;
    tool.insert(u"type"_s, u"function"_s);
    tool.insert(u"function"_s, function);
    return tool;
}

QStringList stringList(const QJsonValue &value, int limit)
{
    QStringList out;
    if (value.isString()) {
        const QString single = value.toString().trimmed();
        if (!single.isEmpty()) {
            out.append(single);
        }
        return out;
    }
    if (!value.isArray()) {
        return out;
    }
    for (const QJsonValue &entry : value.toArray()) {
        QString text;
        if (entry.isString()) {
            text = entry.toString().trimmed();
        } else if (entry.isObject()) {
            text = entry.toObject().value(u"description"_s).toString().trimmed();
            if (text.isEmpty()) {
                text = entry.toObject().value(u"text"_s).toString().trimmed();
            }
        }
        if (!text.isEmpty()) {
            out.append(text);
            if (out.size() >= limit) {
                break;
            }
        }
    }
    return out;
}

QStringList stepList(const QJsonValue &value, int limit)
{
    QStringList out;
    if (!value.isArray()) {
        return out;
    }
    for (const QJsonValue &entry : value.toArray()) {
        QString text;
        QStringList files;
        if (entry.isString()) {
            text = entry.toString().trimmed();
        } else if (entry.isObject()) {
            const QJsonObject object = entry.toObject();
            text = object.value(u"description"_s).toString().trimmed();
            if (text.isEmpty()) {
                text = object.value(u"text"_s).toString().trimmed();
            }
            files = stringList(object.value(u"files"_s), 12);
        }
        if (text.isEmpty()) {
            continue;
        }
        QString rendered = text;
        if (!files.isEmpty()) {
            rendered += u" (files: "_s + files.join(u", "_s) + u')';
        }
        out.append(rendered);
        if (out.size() >= limit) {
            break;
        }
    }
    return out;
}

QString planSignature(const OrchestrationPlan &plan)
{
    QStringList lines;
    lines.append(plan.needsChanges ? u"changes"_s : u"no-changes"_s);
    lines.append(plan.summary.trimmed());
    lines.append(plan.directAnswer.trimmed());
    lines.append(plan.steps.join(u'\n'));
    lines.append(plan.acceptance.join(u'\n'));
    lines.append(plan.replanIf.join(u'\n'));
    return lines.join(u"\n---\n"_s);
}

} // namespace

QString orchestrationRoleId(OrchestrationRole role)
{
    switch (role) {
    case OrchestrationRole::Planner:
        return u"planner"_s;
    case OrchestrationRole::Executor:
        return u"executor"_s;
    case OrchestrationRole::Reviewer:
        return u"reviewer"_s;
    case OrchestrationRole::Single:
        break;
    }
    return QString();
}

QString orchestrationRoleLabel(OrchestrationRole role)
{
    switch (role) {
    case OrchestrationRole::Planner:
        return i18n("Planner");
    case OrchestrationRole::Executor:
        return i18n("Executor");
    case OrchestrationRole::Reviewer:
        return i18n("Planner review");
    case OrchestrationRole::Single:
        break;
    }
    return QString();
}

bool orchestrationRoleIsReadOnly(OrchestrationRole role)
{
    return role == OrchestrationRole::Planner || role == OrchestrationRole::Reviewer;
}

bool providerSupportsOrchestration(Provider provider)
{
    return provider != Provider::Acp && provider != Provider::Telegram;
}

bool modelRouteUsable(const Settings &settings, const ModelRoute &route, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (!providerSupportsOrchestration(route.provider)) {
        return fail(i18n("%1 cannot be used for orchestration; choose an API-key provider.",
                         providerLabel(route.provider)));
    }
    if (route.model.trimmed().isEmpty()) {
        return fail(i18n("No model is selected for %1.", providerLabel(route.provider)));
    }
    Settings copy = settingsForRoute(settings, route);
    if (apiKeyFor(copy).trimmed().isEmpty()) {
        return fail(i18n("%1 has no API key configured.", providerLabel(route.provider)));
    }
    if (error) {
        error->clear();
    }
    return true;
}

Settings settingsForRoute(const Settings &settings, const ModelRoute &route)
{
    Settings copy = settings;
    copy.provider = route.provider;
    const QString value = route.model.trimmed();
    if (!value.isEmpty()) {
        switch (route.provider) {
        case Provider::OpenAI:
            copy.openaiModel = value;
            break;
        case Provider::OpenRouter:
            copy.openrouterModel = value;
            break;
        case Provider::DeepSeek:
            copy.deepseekModel = value;
            break;
        case Provider::OpenAICompatible:
            copy.openaiCompatibleModel = value;
            break;
        case Provider::ClaudeCompatible:
            copy.claudeCompatibleModel = value;
            break;
        case Provider::Kilo:
            copy.kiloModel = value;
            break;
        case Provider::OpenCode:
            copy.opencodeModel = value;
            break;
        case Provider::Acp:
            copy.acpModel = value;
            break;
        case Provider::Grok:
        case Provider::Telegram:
        default:
            copy.grokModel = value;
            break;
        }
    }
    if (!route.effort.trimmed().isEmpty()) {
        copy.reasoningEffort = route.effort.trimmed();
    }
    return copy;
}

QList<Provider> orchestrationProviders(const Settings &settings)
{
    Q_UNUSED(settings)
    return {Provider::Grok, Provider::OpenAI, Provider::OpenRouter, Provider::DeepSeek,
            Provider::OpenAICompatible, Provider::ClaudeCompatible, Provider::Kilo,
            Provider::OpenCode};
}

QString submitPlanToolName()
{
    return u"submit_plan"_s;
}

QString requestReplanToolName()
{
    return u"request_replan"_s;
}

QString submitReviewToolName()
{
    return u"submit_review"_s;
}

bool isOrchestrationTool(const QString &name)
{
    return name == submitPlanToolName() || name == requestReplanToolName()
        || name == submitReviewToolName();
}

QJsonObject submitPlanDefinition()
{
    const QJsonObject stepSchema{
        {u"type"_s, u"object"_s},
        {u"properties"_s,
         QJsonObject{
             {u"description"_s, QJsonObject{{u"type"_s, u"string"_s}}},
             {u"files"_s, QJsonObject{{u"type"_s, u"array"_s},
                                      {u"items"_s, QJsonObject{{u"type"_s, u"string"_s}}},
                                      {u"description"_s, u"Files or symbols touched by this step."_s}}},
             {u"verification"_s, QJsonObject{{u"type"_s, u"string"_s}}},
         }},
        {u"required"_s, QJsonArray{u"description"_s}},
    };
    return functionSchema(
        submitPlanToolName(),
        u"Submit the implementation plan to the executor. Use needs_changes=false "
        "and direct_answer when the request needs no code change. Call this alone."_s,
        QJsonObject{
            {u"summary"_s, QJsonObject{{u"type"_s, u"string"_s},
                                       {u"description"_s, u"One-paragraph objective and approach."_s}}},
            {u"needs_changes"_s, QJsonObject{{u"type"_s, u"boolean"_s},
                                             {u"description"_s, u"False for a direct answer."_s}}},
            {u"steps"_s, QJsonObject{{u"type"_s, u"array"_s},
                                     {u"items"_s, stepSchema},
                                     {u"description"_s, u"Ordered, verifiable implementation steps."_s}}},
            {u"acceptance"_s, QJsonObject{{u"type"_s, u"array"_s},
                                          {u"items"_s, QJsonObject{{u"type"_s, u"string"_s}}},
                                          {u"description"_s, u"How completion is verified."_s}}},
            {u"replan_if"_s, QJsonObject{{u"type"_s, u"array"_s},
                                         {u"items"_s, QJsonObject{{u"type"_s, u"string"_s}}},
                                         {u"description"_s, u"Conditions that should return control to the planner."_s}}},
            {u"direct_answer"_s, QJsonObject{{u"type"_s, u"string"_s},
                                             {u"description"_s, u"Answer shown to the user when needs_changes is false."_s}}},
        },
        QJsonArray{u"summary"_s, u"needs_changes"_s});
}

QJsonObject requestReplanDefinition()
{
    return functionSchema(
        requestReplanToolName(),
        u"Hand control back to the planner when the plan is wrong, a verification "
        "keeps failing, or an interface behaves differently than expected. Call this alone."_s,
        QJsonObject{
            {u"reason"_s, QJsonObject{{u"type"_s, u"string"_s},
                                      {u"description"_s, u"verification_failed | plan_gap | unexpected_interface | design_decision"_s}}},
            {u"what_failed"_s, QJsonObject{{u"type"_s, u"string"_s},
                                           {u"description"_s, u"Concrete evidence the planner can act on."_s}}},
            {u"attempted"_s, QJsonObject{{u"type"_s, u"array"_s},
                                         {u"items"_s, QJsonObject{{u"type"_s, u"string"_s}}},
                                         {u"description"_s, u"Approaches already tried."_s}}},
            {u"question"_s, QJsonObject{{u"type"_s, u"string"_s},
                                        {u"description"_s, u"The decision the planner must settle, if any."_s}}},
        },
        QJsonArray{u"reason"_s, u"what_failed"_s});
}

QJsonObject submitReviewDefinition()
{
    return functionSchema(
        submitReviewToolName(),
        u"Approve the executor's result or return concrete fixes. Call this alone."_s,
        QJsonObject{
            {u"approved"_s, QJsonObject{{u"type"_s, u"boolean"_s}}},
            {u"summary"_s, QJsonObject{{u"type"_s, u"string"_s}}},
            {u"fixes"_s, QJsonObject{{u"type"_s, u"array"_s},
                                     {u"items"_s, QJsonObject{{u"type"_s, u"string"_s}}},
                                     {u"description"_s, u"Concrete corrections for the executor."_s}}},
            {u"ask_user"_s, QJsonObject{{u"type"_s, u"string"_s},
                                        {u"description"_s, u"Question to put to the user instead of approving."_s}}},
        },
        QJsonArray{u"approved"_s});
}

QJsonArray orchestrationToolDefinitions(OrchestrationRole role)
{
    QJsonArray tools;
    switch (role) {
    case OrchestrationRole::Planner:
        tools.append(submitPlanDefinition());
        break;
    case OrchestrationRole::Executor:
        tools.append(requestReplanDefinition());
        break;
    case OrchestrationRole::Reviewer:
        tools.append(submitReviewDefinition());
        break;
    case OrchestrationRole::Single:
        break;
    }
    return tools;
}

bool parseSubmitPlan(const QJsonObject &arguments, OrchestrationPlan *plan, QString *error)
{
    if (!plan) {
        return false;
    }
    auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    OrchestrationPlan parsed;
    parsed.summary = arguments.value(u"summary"_s).toString().trimmed();
    parsed.needsChanges = arguments.value(u"needs_changes"_s).toBool(true);
    parsed.steps = stepList(arguments.value(u"steps"_s), 30);
    parsed.files = stringList(arguments.value(u"files"_s), 50);
    parsed.acceptance = stringList(arguments.value(u"acceptance"_s), 20);
    parsed.replanIf = stringList(arguments.value(u"replan_if"_s), 20);
    parsed.directAnswer = arguments.value(u"direct_answer"_s).toString().trimmed();
    if (parsed.summary.isEmpty()) {
        parsed.summary = parsed.steps.isEmpty() ? parsed.directAnswer : parsed.steps.first();
    }
    if (parsed.needsChanges && parsed.steps.isEmpty()) {
        return fail(u"submit_plan needs at least one step when needs_changes is true."_s);
    }
    if (!parsed.needsChanges && parsed.directAnswer.isEmpty()) {
        parsed.directAnswer = parsed.summary;
    }
    if (!parsed.needsChanges && parsed.directAnswer.isEmpty()) {
        return fail(u"submit_plan needs direct_answer when needs_changes is false."_s);
    }
    parsed.hash = planHash(parsed);
    *plan = parsed;
    if (error) {
        error->clear();
    }
    return true;
}

bool parseSubmitReview(const QJsonObject &arguments, OrchestrationReview *review, QString *error)
{
    if (!review) {
        return false;
    }
    OrchestrationReview parsed;
    parsed.approved = arguments.value(u"approved"_s).toBool(false);
    parsed.summary = arguments.value(u"summary"_s).toString().trimmed();
    parsed.fixes = stringList(arguments.value(u"fixes"_s), 20);
    parsed.askUser = arguments.value(u"ask_user"_s).toString().trimmed();
    if (!parsed.approved && parsed.fixes.isEmpty() && parsed.askUser.isEmpty()) {
        if (error) {
            *error = i18n("Rejected review needs at least one fix or ask_user question.");
        }
        return false;
    }
    *review = parsed;
    if (error) {
        error->clear();
    }
    return true;
}

bool parseRequestReplan(const QJsonObject &arguments, QString *reason, QString *error)
{
    if (!reason) {
        return false;
    }
    const QString category = arguments.value(u"reason"_s).toString().trimmed();
    const QString detail = arguments.value(u"what_failed"_s).toString().trimmed();
    if (category.isEmpty() && detail.isEmpty()) {
        if (error) {
            *error = i18n("request_replan needs a reason or what_failed evidence.");
        }
        return false;
    }
    *reason = category.isEmpty() ? detail : category + u": "_s + detail;
    if (error) {
        error->clear();
    }
    return true;
}

QString planHash(const OrchestrationPlan &plan)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(planSignature(plan).toUtf8(), QCryptographicHash::Sha1)
            .toHex()
            .left(20));
}

QString renderPlanDirective(const OrchestrationPlan &plan)
{
    QStringList lines;
    lines.append(u"<orchestration_plan version=\"%1\">"_s.arg(plan.version));
    lines.append(i18n("Objective: %1", plan.summary));
    if (!plan.steps.isEmpty()) {
        lines.append(i18n("Steps:"));
        int index = 1;
        for (const QString &step : plan.steps) {
            lines.append(QString::number(index++) + u". "_s + step);
        }
    }
    if (!plan.acceptance.isEmpty()) {
        lines.append(i18n("Acceptance checks:"));
        for (const QString &item : plan.acceptance) {
            lines.append(u"- "_s + item);
        }
    }
    if (!plan.replanIf.isEmpty()) {
        lines.append(i18n("Return control to the planner if:"));
        for (const QString &item : plan.replanIf) {
            lines.append(u"- "_s + item);
        }
    }
    lines.append(u"</orchestration_plan>"_s);
    return lines.join(u'\n');
}

QString renderReviewDirective(const OrchestrationPlan &plan, const QString &changedFiles,
                              const QString &verification, const QString &executorSummary,
                              const QString &fixes)
{
    QString text = u"<orchestration_review plan_version=\"%1\">\n"_s.arg(plan.version);
    text += i18n("Plan summary: %1\n", plan.summary);
    if (!plan.acceptance.isEmpty()) {
        text += i18n("Acceptance checks:\n");
        for (const QString &item : plan.acceptance) {
            text += u"- "_s + item + u'\n';
        }
    }
    if (!changedFiles.trimmed().isEmpty()) {
        text += i18n("Changed files: %1\n", changedFiles.trimmed());
    }
    if (!verification.trimmed().isEmpty()) {
        text += i18n("Verification evidence:\n%1\n", verification.trimmed());
    }
    if (!executorSummary.trimmed().isEmpty()) {
        text += i18n("Executor summary:\n%1\n", executorSummary.trimmed());
    }
    if (!fixes.trimmed().isEmpty()) {
        text += i18n("Fixes the executor must apply:\n%1\n", fixes.trimmed());
    }
    text += u"</orchestration_review>"_s;
    return text;
}

QString renderHandoffDirective(const QString &reason, const QString &evidence,
                               const QString &changedFiles, const OrchestrationPlan &plan)
{
    QString text = u"<orchestration_handoff plan_version=\"%1\">\n"_s.arg(plan.version);
    text += i18n("The executor returned control: %1\n", reason.trimmed());
    if (!evidence.trimmed().isEmpty()) {
        text += i18n("Evidence:\n%1\n", evidence.trimmed());
    }
    if (!changedFiles.trimmed().isEmpty()) {
        text += i18n("Changed files: %1\n", changedFiles.trimmed());
    }
    if (!plan.steps.isEmpty()) {
        text += i18n("Current plan steps:\n");
        for (const QString &step : plan.steps) {
            text += u"- "_s + step + u'\n';
        }
    }
    text += u"</orchestration_handoff>"_s;
    return text;
}

QString orchestrationRolePrompt(OrchestrationRole role)
{
    switch (role) {
    case OrchestrationRole::Planner:
        return u"ORCHESTRATION ROLE: PLANNER\n\n"
               "You are the planning model in a two-model session. You may inspect the workspace with read-only "
               "tools. You cannot edit files, run commands, change state, or delegate.\n\n"
               "Resolve the user's intent and inspect only what the plan needs. If the request is a question or "
               "needs no code change, answer it directly with submit_plan(needs_changes=false, direct_answer=...). "
               "Otherwise produce a concrete plan: ordered steps, exact files/symbols, acceptance checks, and the "
               "conditions that should return control to you.\n\n"
               "Do not implement the plan. Do not leave design decisions to the executor. Prefer 5-15 verifiable "
               "steps. Call submit_plan exactly once when ready; do not submit it alongside other tool calls.\n"_s;
    case OrchestrationRole::Executor:
        return u"ORCHESTRATION ROLE: EXECUTOR\n\n"
               "A planner model owns the design. The current plan is in <orchestration_plan>. Follow it. You may "
               "search, edit, run commands, build, debug and verify. Keep the planner's decisions; do not redesign "
               "the solution.\n\n"
               "If a plan step is impossible, a verification failure repeats, an interface behaves differently "
               "than the plan assumed, or a design choice is missing, call request_replan with the evidence. "
               "Otherwise finish with a summary of what changed and how it was verified. Update the plan checklist "
               "with todo_write as steps complete. Do not claim success without evidence.\n"_s;
    case OrchestrationRole::Reviewer:
        return u"ORCHESTRATION ROLE: PLANNER REVIEW\n\n"
               "Review the executor's actual result against the plan and its acceptance checks. Inspect files or "
               "evidence with read-only tools. Approve only when the checks pass and verification evidence exists. "
               "Otherwise return concrete fixes, or ask the user when a decision is required. Call submit_review.\n"_s;
    case OrchestrationRole::Single:
        break;
    }
    return QString();
}

bool shouldReviewOrchestratedTurn(const OrchestrationSettings &settings, bool changedFiles,
                                  int planSteps)
{
    if (settings.reviewMode.compare(u"off"_s, Qt::CaseInsensitive) == 0) {
        return false;
    }
    if (settings.reviewMode.compare(u"always"_s, Qt::CaseInsensitive) == 0) {
        return true;
    }
    return changedFiles || planSteps >= 3;
}

} // namespace KateAi
