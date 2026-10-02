/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "permissions.h"

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

PermissionPolicy::PermissionPolicy(PermissionMode mode)
    : m_mode(mode)
{
}

void PermissionPolicy::grantSession(const QString &toolName)
{
    if (!m_sessionGrants.contains(toolName)) {
        m_sessionGrants.append(toolName);
    }
}

void PermissionPolicy::revokeSession()
{
    m_sessionGrants.clear();
}

bool PermissionPolicy::sessionGranted(const QString &toolName) const
{
    return m_sessionGrants.contains(toolName);
}

bool PermissionPolicy::isReadTool(const QString &toolName) const
{
    if (m_extraReadTools.contains(toolName)) {
        return true;
    }
    // Web tools are read-only: they cannot touch the workspace, so they are
    // safe to run without an edit checkpoint or an edit lock.
    return toolName == u"read_file"_s || toolName == u"list_dir"_s || toolName == u"grep"_s
        || toolName == u"glob"_s || toolName == u"query_project_graph"_s
        || toolName == u"web_search"_s || toolName == u"web_fetch"_s;
}

ToolRisk PermissionPolicy::riskFor(const QString &toolName) const
{
    if (toolName == u"bash"_s || toolName == u"new_task"_s) {
        return ToolRisk::Execute;
    }
    if (isReadTool(toolName)) {
        return ToolRisk::Read;
    }
    // An MCP tool can do anything the server offers, so it is treated as an
    // execute-risk call unless the server marked it read-only.
    if (toolName.startsWith(u"mcp__"_s)) {
        return ToolRisk::Execute;
    }
    return ToolRisk::Write;
}

PermissionPolicy::Verdict PermissionPolicy::evaluate(const QString &toolName, const QJsonObject &arguments, const Sandbox &sandbox, QString *reason) const
{
    if (toolName == u"bash"_s) {
        const QString command = arguments.value(u"command"_s).toString();
        if (sandbox.isAlwaysDeniedCommand(command)) {
            if (reason) {
                *reason = u"Command is blocked by the safety policy."_s;
            }
            return Verdict::Deny;
        }
        // A read-only command must not become a way around the deny globs that
        // read_file and friends enforce, so this is a hard deny too.
        if (sandbox.commandTouchesDeniedPath(command)) {
            if (reason) {
                *reason = u"Command touches a path blocked by the deny rules."_s;
            }
            return Verdict::Deny;
        }
    }

    if (m_mode == PermissionMode::AlwaysApprove) {
        return Verdict::Allow;
    }

    // Delegation is consent to spend, not a mutation of the workspace. Every
    // tool the sub-agent itself runs is still judged by this policy, so
    // allowing the spawn does not let it edit unattended.
    if (toolName == u"new_task"_s) {
        return Verdict::Allow;
    }

    // Explicit auto-approval beats everything except the hard-deny list.
    if (m_autoApprove.contains(toolName)) {
        return Verdict::Allow;
    }

    if (sessionGranted(toolName)) {
        if (toolName == u"bash"_s && sandbox.isDangerousCommand(arguments.value(u"command"_s).toString())) {
            return Verdict::Ask;
        }
        return Verdict::Allow;
    }

    if (isReadTool(toolName)) {
        return Verdict::Allow;
    }

    if (toolName == u"bash"_s && sandbox.isReadOnlyCommand(arguments.value(u"command"_s).toString())) {
        return Verdict::Allow;
    }

    if (m_mode == PermissionMode::AcceptEdits && riskFor(toolName) == ToolRisk::Write) {
        return Verdict::Allow;
    }

    if (reason) {
        *reason = u"This action needs your approval."_s;
    }
    return Verdict::Ask;
}

} // namespace KateAi
