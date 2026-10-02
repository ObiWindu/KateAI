/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QProcess>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTimer;

namespace KateAi
{

// Prefix every tool exposed by an MCP server carries in the model's tool set.
QString mcpToolPrefix();
bool isMcpToolName(const QString &toolName);
// Splits "mcp__server__tool" into its parts. Returns false for other names.
bool splitMcpToolName(const QString &qualifiedName, QString *server, QString *tool);
// Replaces "__" so a server or tool name cannot forge a different identity.
QString sanitizeMcpSegment(const QString &segment);

enum class McpTransport {
    Stdio,
    Http,
};

enum class McpServerState {
    Disabled,
    Starting,
    Ready,
    Failed,
    Stopped,
};

QString mcpTransportId(McpTransport transport);
McpTransport mcpTransportFromId(const QString &id);
QString mcpServerStateLabel(McpServerState state);

struct McpServerConfig {
    QString name;
    McpTransport transport = McpTransport::Stdio;
    // stdio
    QString command;
    QStringList args;
    QString cwd;
    QMap<QString, QString> env;
    // http
    QString url;
    QMap<QString, QString> headers;
    bool enabled = true;
    // Tools this server may call without prompting.
    QStringList alwaysAllow;
    int timeoutMs = 60000;

    bool isValid(QString *error = nullptr) const;
    QString transportId() const
    {
        return mcpTransportId(transport);
    }
    static McpTransport transportFromId(const QString &id);

    QJsonObject toJson() const;
    // Accepts the common mcp.json shape, where "type" or "transport" picks the
    // transport and either "command"/"args" or "url" describes the endpoint.
    static McpServerConfig fromJson(const QJsonObject &object, const QString &fallbackName = QString());

    // {"mcpServers": {"name": {...}}} - the shape every MCP client reads.
    static QJsonObject serversToJson(const QList<McpServerConfig> &servers);
    static QList<McpServerConfig> serversFromJson(const QJsonObject &root);
};

struct McpTool {
    QString server;
    QString name;
    QString description;
    QJsonObject inputSchema;
    bool readOnly = false;
    bool destructive = false;

    QString qualifiedName() const;
    // Converts the tool into an OpenAI-style function definition.
    QJsonObject toToolDefinition() const;
};

// One connection to one MCP server, over stdio or Streamable HTTP.
class McpClient : public QObject
{
    Q_OBJECT

public:
    explicit McpClient(McpServerConfig config, QObject *parent = nullptr);
    ~McpClient() override;

    void start();
    void stop();

    const McpServerConfig &config() const
    {
        return m_config;
    }
    QString name() const
    {
        return m_config.name;
    }
    McpServerState state() const
    {
        return m_state;
    }
    QString statusText() const
    {
        return m_status;
    }
    QList<McpTool> tools() const
    {
        return m_tools;
    }
    bool isReady() const
    {
        return m_state == McpServerState::Ready;
    }
    const McpTool *tool(const QString &toolName) const;
    bool isAutoApproved(const QString &toolName) const;

    // Issues tools/call. The outcome arrives asynchronously via toolResult(),
    // tagged with callToken so a stale reply can be ignored after an abort.
    bool callTool(const QString &callToken, const QString &toolName, const QJsonObject &arguments);

Q_SIGNALS:
    void stateChanged();
    void toolsChanged();
    void toolResult(const QString &callToken, bool ok, const QString &output, const QString &error);
    void logMessage(const QString &message);

private:
    struct Pending {
        int id = 0;
        QString method;
        QString callToken;
        QPointer<QTimer> timeout;
        QPointer<QNetworkReply> reply;
    };

    void setState(McpServerState state, const QString &status);
    int sendRequest(const QString &method, const QJsonObject &params);
    void sendRequestForId(const QString &method, const QJsonObject &params, int id);
    void sendNotification(const QString &method, const QJsonObject &params);
    void writeMessage(const QJsonObject &message);
    void handleMessage(const QJsonObject &message);
    void onStdioReadyRead();
    void onStdioFailed(QProcess::ProcessError error);
    void onStdioFinished(int exitCode);
    void onHttpFinished(QNetworkReply *reply, int id);
    void failAll(const QString &error);
    QList<McpTool> parseToolsList(const QJsonObject &params) const;

    McpServerConfig m_config;
    McpServerState m_state = McpServerState::Stopped;
    QString m_status;
    QList<McpTool> m_tools;

    QProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;

    QNetworkAccessManager *m_nam = nullptr;
    QString m_sessionId;

    int m_nextId = 1;
    QHash<int, Pending> m_pending;
    QPointer<QTimer> m_startupTimer;
    bool m_shuttingDown = false;
};

// Owns every configured server and presents their tools as one flat set.
class McpManager : public QObject
{
    Q_OBJECT

public:
    explicit McpManager(QObject *parent = nullptr);
    ~McpManager() override;

    void setWorkspace(const QString &workspace);
    QString workspace() const
    {
        return m_workspace;
    }

    QList<McpServerConfig> servers() const
    {
        return m_configs;
    }
    // Applies a new set, restarting only the servers whose config changed.
    void setServers(const QList<McpServerConfig> &servers);
    // Re-reads .kateai/mcp.json and applies it.
    void reloadFromDisk();
    // Writes the current set back to .kateai/mcp.json.
    bool saveToDisk(QString *error = nullptr);

    void setEnabled(bool enabled);
    bool isEnabled() const
    {
        return m_enabled;
    }
    void setAutoConnect(bool autoConnect)
    {
        m_autoConnect = autoConnect;
    }
    void setTimeoutMs(int timeoutMs)
    {
        m_timeoutMs = timeoutMs;
    }

    // Connects every enabled server that is not connected yet.
    void connectAll();
    void disconnectAll();

    QList<McpTool> tools() const;
    QJsonArray toolDefinitions() const;
    QStringList readyServers() const;
    QStringList toolNames() const;
    bool hasTool(const QString &qualifiedName) const;
    // True when the name is an MCP tool this manager can currently run.
    bool isAvailable(const QString &toolName) const;
    // The tool with this qualified name, if any. Returned by value because the
        // list is rebuilt per call and a pointer into it would not outlive us.
        std::optional<McpTool> tool(const QString &qualifiedName) const;
    McpServerConfig serverConfig(const QString &serverName) const;
    // Union of every server's alwaysAllow list.
    QStringList autoApprovedTools() const;
    // Tools whose server advertises readOnlyHint.
    QStringList readOnlyTools() const;
    QString statusSummary() const;

    bool callTool(const QString &callToken, const QString &qualifiedName, const QJsonObject &arguments);

Q_SIGNALS:
    void serversChanged();
    void toolsChanged();
    void serverStatusChanged(const QString &serverName);
    void toolResult(const QString &callToken, bool ok, const QString &output, const QString &error);
    void logMessage(const QString &message);

private:
    void rebuildClients();
    void handleClientResult(const QString &serverName, const QString &callToken, bool ok, const QString &output, const QString &error);
    void refreshDerived();

    QString m_workspace;
    QList<McpServerConfig> m_configs;
    QHash<QString, McpClient *> m_clients;
    bool m_enabled = true;
    bool m_autoConnect = true;
    int m_timeoutMs = 60000;
    QStringList m_autoApproved;
    QStringList m_readOnlyTools;
};

namespace McpConfigStore
{
// <workspace>/.kateai/mcp.json
QString configPath(const QString &workspace);
QList<McpServerConfig> load(const QString &workspace);
bool save(const QString &workspace, const QList<McpServerConfig> &servers, QString *error = nullptr);
} // namespace McpConfigStore

} // namespace KateAi