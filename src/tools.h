#pragma once

#include "documentbridge.h"
#include "permissions.h"
#include "sandbox.h"
#include "types.h"

#include <QObject>

namespace KateAi
{

class ToolRunner : public QObject
{
    Q_OBJECT

public:
    ToolRunner(Sandbox sandbox, DocumentBridge *bridge, QObject *parent = nullptr);

    void setSandbox(Sandbox sandbox)
    {
        m_sandbox = std::move(sandbox);
    }
    void setTimeoutMs(int timeoutMs)
    {
        m_timeoutMs = timeoutMs;
    }

    ToolResult run(const ToolCall &call);
    PermissionRequest describe(const ToolCall &call) const;

Q_SIGNALS:
    void bashFinished(const ToolResult &result);

private:
    ToolResult readFile(const QJsonObject &args) const;
    ToolResult writeFile(const QJsonObject &args);
    ToolResult editFile(const QJsonObject &args);
    ToolResult listDir(const QJsonObject &args) const;
    ToolResult grep(const QJsonObject &args) const;
    ToolResult glob(const QJsonObject &args) const;
    ToolResult bash(const QJsonObject &args);

    Sandbox m_sandbox;
    DocumentBridge *m_bridge = nullptr;
    int m_timeoutMs = 60000;
};

QString unifiedDiff(const QString &path, const QString &before, const QString &after);

} // namespace KateAi
