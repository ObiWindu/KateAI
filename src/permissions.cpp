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
    return toolName == u"read_file"_s || toolName == u"list_dir"_s || toolName == u"grep"_s || toolName == u"glob"_s;
}

ToolRisk PermissionPolicy::riskFor(const QString &toolName) const
{
    if (toolName == u"bash"_s) {
        return ToolRisk::Execute;
    }
    if (isReadTool(toolName)) {
        return ToolRisk::Read;
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
    }

    if (m_mode == PermissionMode::AlwaysApprove) {
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
