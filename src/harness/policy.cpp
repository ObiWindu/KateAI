/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "policy.h"

#include "sandbox.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString resolveForMatch(const QString &workspace, const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    QFileInfo info(path);
    if (info.isRelative()) {
        info = QFileInfo(QDir(workspace).absoluteFilePath(path));
    }
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

QStringList pathValues(const QJsonObject &arguments)
{
    QStringList values;
    const QString path = arguments.value(u"path"_s).toString();
    if (!path.isEmpty()) {
        values.append(path);
    }
    for (const QString &key : {u"paths"_s, u"files"_s}) {
        for (const QJsonValue &value : arguments.value(key).toArray()) {
            const QString item = value.toString();
            if (!item.isEmpty()) {
                values.append(item);
            }
        }
    }
    return values;
}

QStringList commandPathTokens(const QString &command)
{
    QStringList tokens;
    static const QRegularExpression tokenPattern(uR"((^|[\s=:])([A-Za-z0-9_./~-]*[/.][A-Za-z0-9_./~-]+))"_s);
    auto it = tokenPattern.globalMatch(command);
    while (it.hasNext()) {
        const QString token = it.next().captured(2);
        if (!token.startsWith(u"-"_s)) {
            tokens.append(token);
        }
    }
    return tokens;
}

bool wildcardMatch(const QString &pattern, const QString &value)
{
    if (!pattern.contains(u'*')) {
        return pattern == value;
    }
    QString regex = QRegularExpression::escape(pattern);
    regex.replace(u"\\*"_s, u".*"_s);
    return QRegularExpression(u"^%1$"_s.arg(regex),
                              QRegularExpression::CaseInsensitiveOption)
        .match(value)
        .hasMatch();
}

QString riskFor(const QString &toolName)
{
    const QString canonical = canonicalToolName(toolName);
    if (canonical == u"bash"_s || canonical == u"new_task"_s
        || canonical.startsWith(u"mcp__"_s) || canonical == u"terminal_run"_s
        || canonical == u"debug_session"_s || canonical == u"debug_step"_s
        || canonical == u"debug_set_breakpoint"_s || canonical == u"debug_console"_s) {
        return u"execute"_s;
    }
    if (canonical == u"read_file"_s || canonical == u"read_tool_result"_s
        || canonical == u"list_dir"_s || canonical == u"grep"_s || canonical == u"glob"_s
        || canonical == u"query_project_graph"_s || canonical == u"web_search"_s
        || canonical == u"web_fetch"_s || canonical == u"lsp_diagnostics"_s
        || canonical.startsWith(u"lsp_"_s) || canonical.startsWith(u"debug_status"_s)
        || canonical.startsWith(u"debug_stack"_s) || canonical.startsWith(u"debug_locals"_s)
        || canonical.startsWith(u"debug_breakpoints"_s) || canonical.startsWith(u"debug_eval"_s)
        || canonical == u"question"_s || canonical == u"think"_s || canonical == u"todo_read"_s
        || canonical == u"write_plan_file"_s || canonical == u"suggest"_s) {
        return u"read"_s;
    }
    return u"write"_s;
}

bool pathMatches(const QString &pattern, const QString &workspace, const QString &rawPath)
{
    if (pattern.isEmpty()) {
        return true;
    }
    const QString absolute = resolveForMatch(workspace, rawPath);
    QString relative = QDir(workspace).relativeFilePath(absolute);
    if (relative.startsWith(u".."_s)) {
        relative = absolute;
    }
    return Sandbox::globMatch(pattern, absolute) || Sandbox::globMatch(pattern, relative)
        || Sandbox::globMatch(pattern, rawPath) || pattern == absolute || pattern == relative;
}

bool ruleMatches(const PolicyRule &rule, const QString &toolName, const QJsonObject &arguments,
                 const Sandbox &sandbox, const QString &workspace)
{
    Q_UNUSED(sandbox)
    const QString canonical = canonicalToolName(toolName);
    if (!rule.match.tool.isEmpty() && !wildcardMatch(rule.match.tool, canonical)
        && !wildcardMatch(rule.match.tool, toolName)) {
        return false;
    }

    if (!rule.match.mcpServer.isEmpty() || !rule.match.mcpTool.isEmpty()) {
        if (!canonical.startsWith(u"mcp__"_s)) {
            return false;
        }
        const QStringList parts = canonical.split(u"__"_s);
        const QString server = parts.size() > 1 ? parts.at(1) : QString();
        const QString mcpTool = parts.size() > 2 ? parts.mid(2).join(u"__"_s) : QString();
        if (!rule.match.mcpServer.isEmpty() && !wildcardMatch(rule.match.mcpServer, server)) {
            return false;
        }
        if (!rule.match.mcpTool.isEmpty() && !wildcardMatch(rule.match.mcpTool, mcpTool)) {
            return false;
        }
    }

    if (!rule.match.risk.isEmpty() && rule.match.risk.compare(riskFor(canonical),
                                                              Qt::CaseInsensitive) != 0) {
        return false;
    }

    if (!rule.match.path.isEmpty()) {
        bool matched = false;
        const QStringList paths = pathValues(arguments);
        for (const QString &path : paths) {
            if (pathMatches(rule.match.path, workspace, path)) {
                matched = true;
                break;
            }
        }
        if (!matched && canonical == u"bash"_s) {
            for (const QString &token : commandPathTokens(arguments.value(u"command"_s).toString())) {
                if (pathMatches(rule.match.path, workspace, token)) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            return false;
        }
    }

    if (!rule.match.commandPrefixes.isEmpty() || !rule.match.commandRegex.isEmpty()) {
        if (canonical != u"bash"_s) {
            return false;
        }
        const QString command = arguments.value(u"command"_s).toString().simplified();
        bool matched = rule.match.commandPrefixes.isEmpty();
        for (const QString &prefix : rule.match.commandPrefixes) {
            const QString normalized = prefix.simplified();
            if (command == normalized || command.startsWith(normalized + u' ')) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
        if (!rule.match.commandRegex.isEmpty()) {
            const QRegularExpression regex(rule.match.commandRegex);
            if (!regex.isValid() || !regex.match(command).hasMatch()) {
                return false;
            }
        }
    }

    return true;
}

bool isUnsafeCommand(const QString &command)
{
    const QString cmd = command.simplified();
    static const QRegularExpression critical(
        uR"((?i)(^|[;&|]\s*)(rm\s+(-[a-z]*[rf][a-z]*\s+)+(/|/\*|~|\$HOME)(\s|$)|mkfs(\.[a-z0-9]+)?\s|dd\s+[^;&|]*of=/dev/|:\s*\(\s*\)\s*\{|shutdown\b|reboot\b|halt\b|poweroff\b))"_s);
    if (critical.match(cmd).hasMatch()) {
        return true;
    }
    // Force-pushing a protected branch defeats the review that the branch
    // protection is meant to provide.
    static const QRegularExpression forcePush(
        uR"(\bgit\s+push\b[^;&|]*(--force|-f)\b[^;&|]*\b(main|master|release/[A-Za-z0-9._-]+)\b)"_s);
    return forcePush.match(cmd).hasMatch();
}

bool isHardlinePath(const QString &workspace, const QString &rawPath)
{
    if (rawPath.isEmpty()) {
        return false;
    }
    const QString absolute = resolveForMatch(workspace, rawPath);
    const QString base = QFileInfo(absolute).fileName();
    if (base == u".env"_s || base.startsWith(u".env."_s)) {
        return true;
    }
    if (base == u"id_rsa"_s || base == u"id_ed25519"_s || base == u"id_ecdsa"_s) {
        return true;
    }
    static const QStringList critical = {
        u"/etc/shadow"_s,
        u"/etc/gshadow"_s,
        u"/etc/sudoers"_s,
        u"/.ssh/"_s,
        u"/.gnupg/"_s,
        u"/.aws/credentials"_s,
        u"/.kube/config"_s,
        u"/.docker/config.json"_s,
    };
    for (const QString &part : critical) {
        if (absolute.endsWith(part) || absolute.contains(part)) {
            return true;
        }
    }
    return false;
}

PolicyVerdict verdictFromString(const QString &value)
{
    const QString lower = value.trimmed().toLower();
    if (lower == u"allow"_s) {
        return PolicyVerdict::Allow;
    }
    if (lower == u"ask"_s) {
        return PolicyVerdict::Ask;
    }
    if (lower == u"deny"_s) {
        return PolicyVerdict::Deny;
    }
    if (lower == u"review"_s) {
        return PolicyVerdict::Review;
    }
    return PolicyVerdict::NoMatch;
}

QString scopeForFile(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.startsWith(u"/etc/"_s)) {
        return u"managed"_s;
    }
    if (clean.contains(u"/.kateai/"_s)) {
        return u"project"_s;
    }
    return u"user"_s;
}

void parseRules(const QString &file, const QString &scope, QList<PolicyRule> *rules,
                QList<PolicyWarning> *warnings)
{
    const QJsonDocument document = QJsonDocument::fromJson(readFile(file).toUtf8());
    if (!document.isObject() && !document.isArray()) {
        warnings->append({file, u"The policy file is not valid JSON."_s});
        return;
    }
    QJsonArray array;
    if (document.isArray()) {
        array = document.array();
    } else {
        array = document.object().value(u"rules"_s).toArray();
    }
    int index = 0;
    for (const QJsonValue &value : array) {
        ++index;
        if (!value.isObject()) {
            warnings->append({file, u"Rule %1 is not an object."_s.arg(index)});
            continue;
        }
        const QJsonObject object = value.toObject();
        PolicyRule rule;
        rule.id = object.value(u"id"_s).toString(
            u"%1-%2"_s.arg(scope).arg(index));
        rule.scope = scope;
        rule.sourceFile = file;
        rule.reason = object.value(u"reason"_s).toString();
        rule.verdict = verdictFromString(object.value(u"decision"_s).toString());
        if (rule.verdict == PolicyVerdict::NoMatch) {
            warnings->append({file,
                              u"Rule %1 has an unknown decision; it is ignored."_s.arg(rule.id)});
            continue;
        }
        const QJsonObject match = object.value(u"match"_s).toObject();
        rule.match.tool = match.value(u"tool"_s).toString();
        rule.match.path = match.value(u"path"_s).toString();
        rule.match.commandRegex = match.value(u"commandRegex"_s).toString();
        rule.match.mcpServer = match.value(u"mcpServer"_s).toString();
        rule.match.mcpTool = match.value(u"mcpTool"_s).toString();
        rule.match.risk = match.value(u"risk"_s).toString();
        const QJsonValue prefix = match.value(u"commandPrefix"_s);
        if (prefix.isString()) {
            rule.match.commandPrefixes.append(prefix.toString());
        } else if (prefix.isArray()) {
            for (const QJsonValue &item : prefix.toArray()) {
                if (item.isString()) {
                    rule.match.commandPrefixes.append(item.toString());
                }
            }
        }
        if (!rule.match.commandRegex.isEmpty()
            && !QRegularExpression(rule.match.commandRegex).isValid()) {
            warnings->append({file,
                              u"Rule %1 has an invalid commandRegex; it is ignored."_s.arg(rule.id)});
            continue;
        }
        if (rule.reason.isEmpty()) {
            rule.reason = rule.verdict == PolicyVerdict::Deny
                ? u"Blocked by policy %1."_s.arg(rule.id)
                : u"Policy %1 matched."_s.arg(rule.id);
        }
        rules->append(rule);
    }
}

} // namespace

QString policyVerdictId(PolicyVerdict verdict)
{
    switch (verdict) {
    case PolicyVerdict::Allow:
        return u"allow"_s;
    case PolicyVerdict::Ask:
        return u"ask"_s;
    case PolicyVerdict::Deny:
        return u"deny"_s;
    case PolicyVerdict::Review:
        return u"review"_s;
    case PolicyVerdict::NoMatch:
        return u"no-match"_s;
    }
    return u"no-match"_s;
}

QString PolicyEngine::userPolicyPath()
{
    QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (config.isEmpty()) {
        config = QDir::home().filePath(u".config"_s);
    }
    return config + u"/kateai/policy.json"_s;
}

QString PolicyEngine::managedPolicyPath()
{
    return u"/etc/kateai/policy.json"_s;
}

void PolicyEngine::clear()
{
    m_rules.clear();
    m_files.clear();
    m_warnings.clear();
}

bool PolicyEngine::load(const QString &workspace, QString *error)
{
    QStringList files;
    files.append(managedPolicyPath());
    if (!workspace.isEmpty()) {
        files.append(QDir(workspace).filePath(u".kateai/policy.json"_s));
    }
    files.append(userPolicyPath());
    return loadFiles(files, error);
}

bool PolicyEngine::loadFiles(const QStringList &files, QString *error)
{
    clear();
    for (const QString &file : files) {
        if (!QFileInfo::exists(file)) {
            continue;
        }
        parseRules(file, scopeForFile(file), &m_rules, &m_warnings);
        m_files.append(file);
    }
    // Managed rules are the most restrictive, then project, then user. The
    // evaluation itself is deny-first across every scope, so this ordering is
    // only for reporting and for the first matching allow rule.
    std::stable_sort(m_rules.begin(), m_rules.end(), [](const PolicyRule &left, const PolicyRule &right) {
        auto rank = [](const QString &scope) {
            if (scope == u"managed"_s) {
                return 0;
            }
            if (scope == u"project"_s) {
                return 1;
            }
            if (scope == u"session"_s) {
                return 2;
            }
            return 3;
        };
        return rank(left.scope) < rank(right.scope);
    });
    if (error) {
        error->clear();
    }
    return true;
}

bool PolicyEngine::hardline(const QString &toolName, const QJsonObject &arguments,
                            const Sandbox &sandbox, QString *reason)
{
    const QString canonical = canonicalToolName(toolName);
    if (canonical == u"bash"_s) {
        const QString command = arguments.value(u"command"_s).toString();
        if (sandbox.isAlwaysDeniedCommand(command) || isUnsafeCommand(command)) {
            if (reason) {
                *reason = u"This command is on the immutable hardline list."_s;
            }
            return true;
        }
    }
    const QString workspace = sandbox.workspaceRoot();
    for (const QString &path : pathValues(arguments)) {
        if (isHardlinePath(workspace, path)) {
            if (reason) {
                *reason = u"This path is on the immutable hardline list."_s;
            }
            return true;
        }
    }
    if (canonical == u"bash"_s) {
        for (const QString &token : commandPathTokens(arguments.value(u"command"_s).toString())) {
            if (isHardlinePath(workspace, token)) {
                if (reason) {
                    *reason = u"The command touches a path on the immutable hardline list."_s;
                }
                return true;
            }
        }
    }
    return false;
}

PolicyDecision PolicyEngine::evaluate(const QString &toolName, const QJsonObject &arguments,
                                      const Sandbox &sandbox, const QString &workspace) const
{
    PolicyDecision decision;
    QString hardlineReason;
    if (hardline(toolName, arguments, sandbox, &hardlineReason)) {
        decision.verdict = PolicyVerdict::Deny;
        decision.ruleId = u"hardline"_s;
        decision.reason = hardlineReason;
        return decision;
    }

    // Deny-first: a deny in any layer beats an allow in any layer.
    for (const PolicyRule &rule : m_rules) {
        if (rule.verdict == PolicyVerdict::Deny
            && ruleMatches(rule, toolName, arguments, sandbox, workspace)) {
            decision.verdict = PolicyVerdict::Deny;
            decision.ruleId = rule.id;
            decision.reason = rule.reason;
            return decision;
        }
    }

    // Review is an ask that a reviewer model may answer; until a reviewer is
    // configured it must not silently allow the call.
    for (const PolicyRule &rule : m_rules) {
        if ((rule.verdict == PolicyVerdict::Ask || rule.verdict == PolicyVerdict::Review)
            && ruleMatches(rule, toolName, arguments, sandbox, workspace)) {
            decision.verdict = rule.verdict;
            decision.ruleId = rule.id;
            decision.reason = rule.reason;
            return decision;
        }
    }

    for (const PolicyRule &rule : m_rules) {
        if (rule.verdict == PolicyVerdict::Allow
            && ruleMatches(rule, toolName, arguments, sandbox, workspace)) {
            decision.verdict = PolicyVerdict::Allow;
            decision.ruleId = rule.id;
            decision.reason = rule.reason;
            return decision;
        }
    }
    return decision;
}

} // namespace KateAi
