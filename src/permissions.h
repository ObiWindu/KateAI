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
};

} // namespace KateAi
