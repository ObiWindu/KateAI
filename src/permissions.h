/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "sandbox.h"
#include "types.h"

namespace KateAi
{

class PermissionPolicy
{
public:
    PermissionPolicy() = default;
    explicit PermissionPolicy(PermissionMode mode);

    PermissionMode mode() const
    {
        return m_mode;
    }
    void setMode(PermissionMode mode)
    {
        m_mode = mode;
    }

    void grantSession(const QString &toolName);
    void revokeSession();
    bool sessionGranted(const QString &toolName) const;

    // Tools that never prompt, whatever the permission mode is. This is the
    // auto-approve list from settings plus each MCP server's alwaysAllow.
    void setAutoApproveTools(const QSet<QString> &tools)
    {
        m_autoApprove = tools;
    }
    void autoApproveTool(const QString &toolName)
    {
        m_autoApprove.insert(toolName);
    }
    bool isAutoApproved(const QString &toolName) const
    {
        return m_autoApprove.contains(toolName);
    }
    // Tools an MCP server marked with readOnlyHint.
    void setReadOnlyTools(const QSet<QString> &tools)
    {
        m_extraReadTools = tools;
    }

    ToolRisk riskFor(const QString &toolName) const;
    bool isReadTool(const QString &toolName) const;

    enum class Verdict {
        Allow,
        Ask,
        Deny,
    };

    Verdict evaluate(const QString &toolName, const QJsonObject &arguments, const Sandbox &sandbox, QString *reason) const;

private:
    PermissionMode m_mode = PermissionMode::Ask;
    QStringList m_sessionGrants;
    QSet<QString> m_autoApprove;
    QSet<QString> m_extraReadTools;
};

} // namespace KateAi
