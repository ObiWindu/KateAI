/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "permissions.h"
#include "mcp.h"

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

PermissionPolicy::PermissionPolicy(PermissionMode mode)
    : m_mode(mode)
{
}

void PermissionPolicy::grantSession(const QString &toolName)
{
    const QString canonical = canonicalToolName(toolName);
    if (!m_sessionGrants.contains(toolName) && !m_sessionGrants.contains(canonical)) {
        m_sessionGrants.append(canonical);
    }
}

void PermissionPolicy::revokeSession()
{
    m_sessionGrants.clear();
}

bool PermissionPolicy::sessionGranted(const QString &toolName) const
{
    const QString canonical = canonicalToolName(toolName);
    return m_sessionGrants.contains(toolName) || m_sessionGrants.contains(canonical);
}

bool PermissionPolicy::isReadTool(const QString &toolName) const
{
    const QString canonical = canonicalToolName(toolName);
    if (m_extraReadTools.contains(toolName) || m_extraReadTools.contains(canonical)) {
        return true;
    }
    // Web tools are read-only: they cannot touch the workspace, so they are
    // safe to run without an edit checkpoint or an edit lock.
    // write_plan_file joins them: it writes only a Kate-chosen temp file, so
    // it never mutates the project and must not prompt just to take notes.
    // suggest joins them for the same reason: it only posts a card for the
    // user to accept or dismiss.
    // question joins them for the same reason again: it neither reads the
    // workspace nor changes it -- it asks the user something.
    const QString c = canonical;
    return c == u"read_file"_s || c == u"list_dir"_s || c == u"grep"_s
        || c == u"glob"_s || c == u"query_project_graph"_s
        || c == u"web_search"_s || c == u"web_fetch"_s
        || c == u"write_plan_file"_s || c == u"suggest"_s || c == u"question"_s
        || toolName == u"read"_s || toolName == u"list"_s || toolName == u"search"_s;
}

ToolRisk PermissionPolicy::riskFor(const QString &toolName) const
{
    const QString canonical = canonicalToolName(toolName);
    if (canonical == u"bash"_s || canonical == u"new_task"_s || toolName == u"execute"_s) {
        return ToolRisk::Execute;
    }
    // An MCP tool can do anything the server offers, whatever its hints say,
    // so it is always treated as an execute-risk call.
    if (toolName.startsWith(u"mcp__"_s)) {
        return ToolRisk::Execute;
    }
    if (isReadTool(toolName)) {
        return ToolRisk::Read;
    }
    return ToolRisk::Write;
}

PermissionPolicy::Verdict PermissionPolicy::evaluate(const QString &toolName, const QJsonObject &arguments, const Sandbox &sandbox, QString *reason) const
{
    PermissionRequest request;
    request.toolName = toolName;
    request.risk = riskFor(canonicalToolName(toolName));
    request.mutatesFiles = false;
    return evaluate(request, arguments, sandbox, reason);
}

PermissionPolicy::Verdict PermissionPolicy::evaluate(const PermissionRequest &request, const QJsonObject &arguments,
                                                     const Sandbox &sandbox, QString *reason) const
{
    const QString toolName = canonicalToolName(request.toolName);
    // Execute-risk or file-mutating calls are never read tools, whatever the
    // name says: an unknown mutating tool literally named "read" must not
    // auto-allow through the read-tool fast path below.
    const bool mutating = request.mutatesFiles || request.risk == ToolRisk::Execute;
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

    // Delegation still spawns a sub-agent whose own tools are judged by this
    // policy, but in Ask mode the spawn itself prompts: it spends budget and
    // can fan out into unattended work. Other modes allow it directly.
    if (toolName == u"new_task"_s) {
        if (m_mode == PermissionMode::Ask) {
            if (reason) {
                *reason = u"Spawning a sub-agent needs your approval."_s;
            }
            return Verdict::Ask;
        }
        return Verdict::Allow;
    }

    // Explicit auto-approval beats everything except the hard-deny list.
    // Checked by canonical and raw name so an alias cannot bypass the list.
    if (m_autoApprove.contains(toolName) || m_autoApprove.contains(request.toolName)) {
        return Verdict::Allow;
    }

    if (sessionGranted(toolName)) {
        if (toolName == u"bash"_s && sandbox.isDangerousCommand(arguments.value(u"command"_s).toString())) {
            return Verdict::Ask;
        }
        return Verdict::Allow;
    }

    // A server's own readOnlyHint is never trusted to skip the prompt in Ask
    // mode; in AcceptEdits it follows the mode's read-tool rule (the mode
    // contract), while the tool's risk stays Execute.
    if (!mutating && isReadTool(toolName) && !isMcpToolName(toolName) && !isMcpToolName(request.toolName)) {
        return Verdict::Allow;
    }
    if (m_mode == PermissionMode::AcceptEdits && isReadTool(toolName)) {
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
