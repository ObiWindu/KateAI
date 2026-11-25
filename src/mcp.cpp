/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "mcp.h"
#include "credentialstore.h"
#include "processenv.h"

#include <KLocalizedString>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <limits>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

static const QString McpProtocolVersion = QStringLiteral("2024-11-05");
static const QString JsonRpcVersion = QStringLiteral("2.0");

// ---------------------------------------------------------------------------
// Names and small value types
// ---------------------------------------------------------------------------

QString mcpToolPrefix()
{
    return u"mcp__"_s;
}

QString sanitizeMcpSegment(const QString &segment)
{
    QString out = segment;
    while (out.contains(u"__"_s)) {
        out.replace(u"__"_s, u"_"_s);
    }
    return out;
}

// A JSON value used as a command argument or as an environment value.
// QJsonValue::toString() answers an empty string for a number or a boolean,
// so such an argument was silently dropped and the server was started with a
// different command line than the configuration described.
QString jsonValueAsText(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    if (value.isNull() || value.isUndefined()) {
        return QString();
    }
    return QString::fromUtf8(QJsonDocument::fromVariant(value.toVariant()).toJson(QJsonDocument::Compact));
}

// QJsonValue::toBool(true) treats the string "false" as true. Workspace
// configuration is hand-editable, so a quoted boolean must be honoured.
static bool mcpBoolValue(const QJsonValue &value, bool fallback)
{
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isDouble()) {
        return value.toInt() != 0;
    }
    if (value.isString()) {
        const QString text = value.toString().trimmed().toLower();
        if (text == u"true"_s || text == u"1"_s || text == u"yes"_s || text == u"on"_s) {
            return true;
        }
        if (text == u"false"_s || text == u"0"_s || text == u"no"_s || text == u"off"_s) {
            return false;
        }
    }
    return fallback;
}

bool isMcpToolName(const QString &toolName)
{
    return toolName.startsWith(mcpToolPrefix()) && toolName.size() > mcpToolPrefix().size();
}

bool splitMcpToolName(const QString &qualifiedName, QString *server, QString *tool)
{
    const QString prefix = mcpToolPrefix();
    if (!qualifiedName.startsWith(prefix)) {
        return false;
    }
    const QString rest = qualifiedName.mid(prefix.size());
    const int separator = rest.indexOf(u"__"_s);
    if (separator <= 0 || separator + 2 >= rest.size()) {
        return false;
    }
    if (server) {
        *server = rest.left(separator);
    }
    if (tool) {
        *tool = rest.mid(separator + 2);
    }
    return true;
}

QString mcpTransportId(McpTransport transport)
{
    return transport == McpTransport::Http ? u"http"_s : u"stdio"_s;
}

McpTransport mcpTransportFromId(const QString &id)
{
    const QString lower = id.toLower();
    return (lower == u"http"_s || lower == u"sse"_s || lower == u"streamable-http"_s || lower == u"streamable_http"_s)
        ? McpTransport::Http
        : McpTransport::Stdio;
}

McpTransport McpServerConfig::transportFromId(const QString &id)
{
    return mcpTransportFromId(id);
}

QString mcpServerStateLabel(McpServerState state)
{
    switch (state) {
    case McpServerState::Disabled:
        return u"Disabled"_s;
    case McpServerState::Starting:
        return u"Starting"_s;
    case McpServerState::Ready:
        return u"Ready"_s;
    case McpServerState::Failed:
        return u"Failed"_s;
    case McpServerState::Stopped:
        return u"Stopped"_s;
    }
    return u"Unknown"_s;
}

bool McpServerConfig::isValid(QString *error) const
{
    if (name.trimmed().isEmpty()) {
        if (error) {
            *error = u"The server name is empty."_s;
        }
        return false;
    }
    if (transport == McpTransport::Http) {
        if (url.trimmed().isEmpty()) {
            if (error) {
                *error = u"Server '%1' has no URL."_s.arg(name);
            }
            return false;
        }
        const QUrl parsed{url.trimmed()};
        if (!parsed.isValid() || parsed.scheme().isEmpty()) {
            if (error) {
                *error = u"Server '%1' has an invalid URL: %2"_s.arg(name, url);
            }
            return false;
        }
        return true;
    }
    if (command.trimmed().isEmpty()) {
        if (error) {
            *error = u"Server '%1' has no command."_s.arg(name);
        }
        return false;
    }
    return true;
}

QString McpServerConfig::commandLine() const
{
    if (transport == McpTransport::Http) {
        return url.trimmed();
    }
    // The program and its arguments, quoted so the boundaries stay readable.
    QStringList parts;
    if (!command.trimmed().isEmpty()) {
        parts.append(command.trimmed());
    }
    for (const QString &arg : args) {
        if (arg.contains(u' ') || arg.contains(u'\t') || arg.contains(u'\n') || arg.contains(u'"')) {
            QString escaped = arg;
            escaped.replace(u'"', u"\\\""_s);
            escaped.replace(u'\n', u"\\n"_s);
            parts.append(u"\"%1\""_s.arg(escaped));
        } else {
            parts.append(arg);
        }
    }
    return parts.join(u' ');
}

QJsonObject McpServerConfig::toJson() const
{
    QJsonObject obj;
    obj.insert(u"name"_s, name);
    obj.insert(u"type"_s, transportId());
    obj.insert(u"enabled"_s, enabled);
    obj.insert(u"timeoutMs"_s, timeoutMs);
    if (transport == McpTransport::Http) {
        obj.insert(u"url"_s, url);
        if (!headers.isEmpty()) {
            QJsonObject headerObj;
            for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
                headerObj.insert(it.key(), it.value());
            }
            obj.insert(u"headers"_s, headerObj);
        }
    } else {
        obj.insert(u"command"_s, command);
        if (!args.isEmpty()) {
            obj.insert(u"args"_s, QJsonArray::fromStringList(args));
        }
        if (!cwd.trimmed().isEmpty()) {
            obj.insert(u"cwd"_s, cwd);
        }
        if (!env.isEmpty()) {
            QJsonObject envObj;
            for (auto it = env.constBegin(); it != env.constEnd(); ++it) {
                envObj.insert(it.key(), it.value());
            }
            obj.insert(u"env"_s, envObj);
        }
    }
    if (!alwaysAllow.isEmpty()) {
        obj.insert(u"alwaysAllow"_s, QJsonArray::fromStringList(alwaysAllow));
    }
    return obj;
}

McpServerConfig McpServerConfig::fromJson(const QJsonObject &object, const QString &fallbackName)
{
    McpServerConfig config;
    config.name = object.value(u"name"_s).toString(fallbackName).trimmed();

    QString transportText = object.value(u"type"_s).toString();
    if (transportText.isEmpty()) {
        transportText = object.value(u"transport"_s).toString();
    }
    if (!transportText.isEmpty()) {
        config.transport = transportFromId(transportText);
    } else if (object.contains(u"url"_s) || object.contains(u"endpoint"_s)) {
        config.transport = McpTransport::Http;
    }

    config.command = object.value(u"command"_s).toString();
    const QJsonArray args = object.value(u"args"_s).toArray();
    for (const QJsonValue &value : args) {
        config.args.append(jsonValueAsText(value));
    }
    config.cwd = object.value(u"cwd"_s).toString();
    const QJsonObject envObj = object.value(u"env"_s).toObject();
    for (auto it = envObj.constBegin(); it != envObj.constEnd(); ++it) {
        config.env.insert(it.key(), jsonValueAsText(it.value()));
    }
    config.url = object.value(u"url"_s).toString();
    if (config.url.isEmpty()) {
        config.url = object.value(u"endpoint"_s).toString();
    }
    const QJsonObject headerObj = object.value(u"headers"_s).toObject();
    for (auto it = headerObj.constBegin(); it != headerObj.constEnd(); ++it) {
        config.headers.insert(it.key(), it.value().toString());
    }
    config.enabled = mcpBoolValue(object.value(u"enabled"_s), true);
    const QJsonArray allow = object.value(u"alwaysAllow"_s).toArray();
    for (const QJsonValue &value : allow) {
        config.alwaysAllow.append(value.toString());
    }
    config.timeoutMs = qBound(1000, object.value(u"timeoutMs"_s).toInt(60000), 30 * 60 * 1000);
    return config;
}

QJsonObject McpServerConfig::serversToJson(const QList<McpServerConfig> &servers)
{
    QJsonObject byName;
    for (const McpServerConfig &server : servers) {
        if (server.name.trimmed().isEmpty()) {
            continue;
        }
        byName.insert(server.name, server.toJson());
    }
    QJsonObject root;
    root.insert(u"mcpServers"_s, byName);
    return root;
}

QList<McpServerConfig> McpServerConfig::serversFromJson(const QJsonObject &root)
{
    QList<McpServerConfig> servers;
    QJsonObject byName = root.value(u"mcpServers"_s).toObject();
    if (byName.isEmpty()) {
        // Also accept a bare { "name": {...} } map.
        byName = root;
    }
    for (auto it = byName.constBegin(); it != byName.constEnd(); ++it) {
        const QJsonObject value = it.value().toObject();
        if (value.isEmpty()) {
            continue;
        }
        servers.append(McpServerConfig::fromJson(value, it.key()));
    }
    return servers;
}

QString McpTool::qualifiedName() const
{
    return mcpToolPrefix() + sanitizeMcpSegment(server) + u"__"_s + sanitizeMcpSegment(name);
}

QJsonObject McpTool::toToolDefinition() const
{
    QJsonObject function;
    function.insert(u"name"_s, qualifiedName());
    function.insert(u"description"_s, description);
    // Servers should return a JSON Schema; fall back to a permissive object so
    // a malformed schema cannot hide the tool from the model.
    QJsonObject schema = inputSchema;
    if (schema.isEmpty()) {
        schema.insert(u"type"_s, u"object"_s);
        schema.insert(u"properties"_s, QJsonObject());
    }
    if (!schema.contains(u"type"_s)) {
        schema.insert(u"type"_s, u"object"_s);
    }
    function.insert(u"parameters"_s, schema);
    QJsonObject tool;
    tool.insert(u"type"_s, u"function"_s);
    tool.insert(u"function"_s, function);
    return tool;
}

// ---------------------------------------------------------------------------
// McpClient
// ---------------------------------------------------------------------------

namespace
{

QString extractToolResultText(const QJsonObject &result)
{
    QStringList parts;
    const QJsonArray content = result.value(u"content"_s).toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(u"type"_s).toString();
        if (item.contains(u"text"_s)) {
            parts.append(item.value(u"text"_s).toString());
        } else if (type == u"image"_s) {
            parts.append(u"[image omitted]"_s);
        } else if (type == u"audio"_s) {
            parts.append(u"[audio omitted]"_s);
        } else if (type == u"resource"_s) {
            parts.append(u"[resource: "_s + item.value(u"resource"_s).toObject().value(u"uri"_s).toString() + u"]"_s);
        } else if (!item.isEmpty()) {
            parts.append(QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact)));
        }
    }
    if (!parts.isEmpty()) {
        return parts.join(u'\n');
    }
    if (result.contains(u"structuredContent"_s)) {
        return QString::fromUtf8(QJsonDocument(result.value(u"structuredContent"_s).toObject()).toJson(QJsonDocument::Indented));
    }
    return QString();
}

QJsonObject jsonRpcError(const QString &message, int code = -32603)
{
    QJsonObject error;
    error.insert(u"code"_s, code);
    error.insert(u"message"_s, message);
    QJsonObject response;
    response.insert(u"jsonrpc"_s, JsonRpcVersion);
    response.insert(u"error"_s, error);
    return response;
}

// Extracts JSON-RPC messages from a Streamable HTTP body, which is either a
// single JSON object or an SSE stream of "data:" lines.
QJsonArray parseHttpBody(const QByteArray &body, const QString &contentType)
{
    QJsonArray messages;
    if (body.trimmed().isEmpty()) {
        return messages;
    }
    if (!contentType.contains(u"text/event-stream"_s)) {
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            messages.append(document.object());
        }
        return messages;
    }
    QString currentEvent;
    QString data;
    auto dispatch = [&]() {
        if (data.trimmed().isEmpty() || data.trimmed() == u"[DONE]"_s) {
            data.clear();
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(data.trimmed().toUtf8());
        QJsonArray payloads;
        if (document.isObject()) {
            payloads.append(document.object());
        } else if (document.isArray()) {
            payloads = document.array();
        }
        for (const QJsonValue &value : std::as_const(payloads)) {
            QJsonObject obj = value.toObject();
            if (obj.isEmpty()) {
                continue;
            }
            if (!currentEvent.isEmpty()) {
                obj.insert(u"_event"_s, currentEvent);
            }
            messages.append(obj);
        }
        currentEvent.clear();
        data.clear();
    };
    for (const QByteArray &line : body.split('\n')) {
        const QString text = QString::fromUtf8(line).trimmed();
        if (text.isEmpty()) {
            dispatch();
            continue;
        }
        if (text.startsWith(u"event:"_s)) {
            currentEvent = text.mid(6).trimmed();
            continue;
        }
        if (!text.startsWith(u"data:"_s)) {
            continue;
        }
        if (!data.isEmpty()) {
            data += u'\n';
        }
        data += text.mid(5).trimmed();
    }
    dispatch();
    return messages;
}

} // namespace

McpClient::McpClient(McpServerConfig config, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
{
    if (!m_config.enabled) {
        setState(McpServerState::Disabled, u"Disabled in configuration"_s);
    }
}

McpClient::~McpClient()
{
    stop();
}

void McpClient::setState(McpServerState state, const QString &status)
{
    if (m_state == state && m_status == status) {
        return;
    }
    m_state = state;
    m_status = status;
    Q_EMIT stateChanged();
}

void McpClient::start()
{
    if (!m_config.enabled) {
        setState(McpServerState::Disabled, u"Disabled in configuration"_s);
        return;
    }
    QString error;
    if (!m_config.isValid(&error)) {
        setState(McpServerState::Failed, error);
        return;
    }

    m_shuttingDown = false;
    m_discardingStdoutFrame = false;
    setState(McpServerState::Starting, u"Connecting…"_s);

    // A retry/reconnect must not leave the previous watchdog parented to this
    // client and still counting down against the new handshake.
    if (m_startupTimer) {
        m_startupTimer->stop();
        m_startupTimer->deleteLater();
        m_startupTimer = nullptr;
    }

    m_startupTimer = new QTimer(this);
    m_startupTimer->setSingleShot(true);
    m_startupTimer->setInterval(30000);
    connect(m_startupTimer, &QTimer::timeout, this, [this] {
        if (m_state == McpServerState::Starting) {
            failAll(u"Timed out waiting for the server handshake."_s);
            setState(McpServerState::Failed, u"Handshake timed out"_s);
            // The server process is still running. Leaving it behind would
            // orphan it, and a later start() would read its output from a
            // different process.
            stopProcess();
        }
    });
    m_startupTimer->start();

    if (m_config.transport == McpTransport::Http) {
        // Restarting without deleting the old manager leaks it (QObject child
        // lives until McpClient dies) and leaves stale replies connected.
        if (m_nam) {
            m_nam->deleteLater();
            m_nam = nullptr;
        }
        m_nam = new QNetworkAccessManager(this);
    } else {
        // A previous run may still own a process: starting a new one next to
        // it would mix their output.
        stopProcess();
        m_process = new QProcess(this);
        m_process->setProcessChannelMode(QProcess::SeparateChannels);
        if (!m_config.cwd.trimmed().isEmpty() && QFileInfo(m_config.cwd).isDir()) {
            m_process->setWorkingDirectory(m_config.cwd);
        }
        // Third-party MCP servers get a scrubbed environment: ambient provider
        // keys are not needed to run the server, and leaking them to every
        // configured server would widen the blast radius of a compromised one.
        // Keys the user explicitly configured for this server are added back.
        QProcessEnvironment environment = sanitizedProcessEnvironment();
        for (auto it = m_config.env.constBegin(); it != m_config.env.constEnd(); ++it) {
            environment.insert(it.key(), it.value());
        }
        m_process->setProcessEnvironment(environment);

        connect(m_process, &QProcess::readyReadStandardOutput, this, &McpClient::onStdioReadyRead);
        connect(m_process, &QProcess::errorOccurred, this, &McpClient::onStdioFailed);
        connect(m_process, &QProcess::finished, this, &McpClient::onStdioFinished);

        m_process->start(m_config.command, m_config.args);
        if (!m_process->waitForStarted(10000)) {
            setState(McpServerState::Failed, u"Could not start '%1': %2"_s.arg(m_config.command, m_process->errorString()));
            return;
        }
        // Drain stderr so a chatty server cannot fill its pipe and block.
        connect(m_process, &QProcess::readyReadStandardError, this, [this] {
            const QString noise = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
            if (!noise.isEmpty()) {
                Q_EMIT logMessage(u"[%1] %2"_s.arg(m_config.name, noise));
            }
        });
    }

    QJsonObject params;
    params.insert(u"protocolVersion"_s, McpProtocolVersion);
    QJsonObject capabilities;
    capabilities.insert(u"tools"_s, QJsonObject());
    params.insert(u"capabilities"_s, capabilities);
    QJsonObject clientInfo;
    clientInfo.insert(u"name"_s, u"kateai"_s);
    clientInfo.insert(u"version"_s, u"1"_s);
    params.insert(u"clientInfo"_s, clientInfo);
    sendRequest(u"initialize"_s, params);
}

void McpClient::stopProcess()
{
    if (!m_process) {
        return;
    }
    if (m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->terminate();
        if (!m_process->waitForFinished(2000)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
    m_process->deleteLater();
    m_process = nullptr;
    m_stdoutBuffer.clear();
    m_discardingStdoutFrame = false;
}

void McpClient::stop()
{
    m_shuttingDown = true;
    if (m_startupTimer) {
        m_startupTimer->stop();
        m_startupTimer->deleteLater();
        m_startupTimer = nullptr;
    }
    failAll(u"Server disconnected."_s);
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        if (it.value().reply) {
            it.value().reply->abort();
        }
    }
    m_pending.clear();

    stopProcess();
    m_tools.clear();
    if (m_nam) {
        m_nam->deleteLater();
        m_nam = nullptr;
    }
    if (m_state != McpServerState::Disabled) {
        setState(McpServerState::Stopped, u"Disconnected"_s);
    }
}

const McpTool *McpClient::tool(const QString &toolName) const
{
    for (const McpTool &candidate : m_tools) {
        if (candidate.name == toolName) {
            return &candidate;
        }
    }
    return nullptr;
}

bool McpClient::isAutoApproved(const QString &toolName) const
{
    for (const QString &allowed : m_config.alwaysAllow) {
        // Accept "tool", "*" and "prefix/*" entries.
        if (allowed == u"*"_s || allowed == toolName) {
            return true;
        }
        if (allowed.endsWith(QLatin1Char('*')) && toolName.startsWith(allowed.left(allowed.size() - 1))) {
            return true;
        }
    }
    return false;
}

bool McpClient::callTool(const QString &callToken, const QString &toolName, const QJsonObject &arguments)
{
    if (!isReady()) {
        Q_EMIT toolResult(callToken, false, QString(), u"Server '%1' is not connected."_s.arg(m_config.name));
        return false;
    }
    if (!tool(toolName)) {
        Q_EMIT toolResult(callToken, false, QString(), u"Server '%1' does not expose a tool named '%2'."_s.arg(m_config.name, toolName));
        return false;
    }

    QJsonObject params;
    params.insert(u"name"_s, toolName);
    params.insert(u"arguments"_s, arguments);
    const int id = m_nextId++;
    Pending pending;
    pending.id = id;
    pending.method = u"tools/call"_s;
    pending.callToken = callToken;

    QTimer *timeout = new QTimer(this);
    timeout->setSingleShot(true);
    int budget = m_config.timeoutMs > 0 ? m_config.timeoutMs : 60000;
    // The manager-wide timeout caps the per-server one.
    if (m_timeoutMs > 0) {
        budget = qMin(budget, m_timeoutMs);
    }
    timeout->setInterval(budget);
    connect(timeout, &QTimer::timeout, this, [this, id, toolName, budget, timeout] {
        timeout->deleteLater();
        const auto it = m_pending.constFind(id);
        if (it == m_pending.constEnd()) {
            return;
        }
        const QString token = it.value().callToken;
        m_pending.remove(id);
        Q_EMIT toolResult(token, false, QString(), u"Tool '%1' timed out after %2 ms."_s.arg(toolName).arg(budget));
    });
    pending.timeout = timeout;
    m_pending.insert(id, pending);
    timeout->start();

    sendRequestForId(u"tools/call"_s, params, id);
    return true;
}

int McpClient::sendRequest(const QString &method, const QJsonObject &params)
{
    // int wraps after 2^31 requests and would collide with live pending ids.
    if (m_nextId <= 0 || m_nextId == std::numeric_limits<int>::max()) {
        m_nextId = 1;
        // If pending is non-empty here ids may still collide; drop only when
        // safe (no in-flight requests), otherwise keep counting to avoid
        // misrouting (practically unreachable).
        if (!m_pending.isEmpty()) {
            qWarning() << "KateAI: MCP request id near overflow with pending requests";
        }
    }
    const int id = m_nextId++;
    Pending pending;
    pending.id = id;
    pending.method = method;
    m_pending.insert(id, pending);
    // Handshake/path requests had no watchdog: a dead server hung them forever
    // (only tools/call had a timer). Arm the same budget here.
    if (method == u"initialize"_s || method == u"tools/list"_s) {
        int budget = m_config.timeoutMs > 0 ? m_config.timeoutMs : 60000;
        if (m_timeoutMs > 0) {
            budget = qMin(budget, m_timeoutMs);
        }
        QTimer *timeout = new QTimer(this);
        timeout->setSingleShot(true);
        timeout->setInterval(budget);
        connect(timeout, &QTimer::timeout, this, [this, id, method, timeout] {
            timeout->deleteLater();
            const auto it = m_pending.constFind(id);
            if (it == m_pending.constEnd()) {
                return;
            }
            m_pending.remove(id);
            if (method == u"initialize"_s) {
                failAll(u"Timed out waiting for the server handshake."_s);
                setState(McpServerState::Failed, u"Handshake timed out"_s);
                stopProcess();
            }
        });
        m_pending[id].timeout = timeout;
        timeout->start();
    }
    sendRequestForId(method, params, id);
    return id;
}

void McpClient::sendRequestForId(const QString &method, const QJsonObject &params, int id)
{
    QJsonObject message;
    message.insert(u"jsonrpc"_s, JsonRpcVersion);
    message.insert(u"id"_s, id);
    message.insert(u"method"_s, method);
    message.insert(u"params"_s, params);

    if (m_config.transport == McpTransport::Http) {
        if (!m_nam) {
            return;
        }
        QNetworkRequest request{QUrl(m_config.url)};
        request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("MCP-Protocol-Version", McpProtocolVersion.toUtf8());
        if (!m_sessionId.isEmpty()) {
            request.setRawHeader("mcp-session-id", m_sessionId.toUtf8());
        }
        for (auto it = m_config.headers.constBegin(); it != m_config.headers.constEnd(); ++it) {
            request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
        }

        // Enforce TLS certificate validation for HTTPS URLs
        QUrl url(m_config.url);
        if (url.scheme() == u"https"_s) {
            QSslConfiguration sslConfig = request.sslConfiguration();
            sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
            sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
            request.setSslConfiguration(sslConfig);
        }

        QNetworkReply *reply = m_nam->post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
        // Stop QNetworkReply from buffering more than the body cap; readAll()
        // still sees the first 16 MiB plus one byte, which is enough to detect
        // an oversized response.
        reply->setReadBufferSize(16 * 1024 * 1024 + 1);
        if (auto it = m_pending.find(id); it != m_pending.end()) {
            it.value().reply = reply;
        }
        connect(reply, &QNetworkReply::finished, this, [this, reply, id] {
            onHttpFinished(reply, id);
        });
        return;
    }

    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return;
    }
    writeMessage(message);
}

void McpClient::sendNotification(const QString &method, const QJsonObject &params)
{
    QJsonObject message;
    message.insert(u"jsonrpc"_s, JsonRpcVersion);
    message.insert(u"method"_s, method);
    message.insert(u"params"_s, params);
    writeMessage(message);
}

void McpClient::writeMessage(const QJsonObject &message)
{
    if (m_config.transport == McpTransport::Http) {
        // Notifications have no reply to wait for.
        if (!m_nam) {
            return;
        }
        QNetworkRequest request{QUrl(m_config.url)};
        request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("MCP-Protocol-Version", McpProtocolVersion.toUtf8());
        if (!m_sessionId.isEmpty()) {
            request.setRawHeader("mcp-session-id", m_sessionId.toUtf8());
        }
        for (auto it = m_config.headers.constBegin(); it != m_config.headers.constEnd(); ++it) {
            request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
        }

        // Enforce TLS certificate validation for HTTPS URLs
        QUrl url(m_config.url);
        if (url.scheme() == u"https"_s) {
            QSslConfiguration sslConfig = request.sslConfiguration();
            sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
            sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
            request.setSslConfiguration(sslConfig);
        }

        QNetworkReply *reply = m_nam->post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
        reply->setReadBufferSize(16 * 1024 * 1024 + 1);
        connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
        return;
    }
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return;
    }
    QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    payload.append('\n');
    m_process->write(payload);
}

void McpClient::onStdioReadyRead()
{
    if (!m_process) {
        return;
    }
    constexpr qsizetype kMaxFrameBytes = 16 * 1024 * 1024;
    const QByteArray output = m_process->readAllStandardOutput();
    for (qsizetype offset = 0; offset < output.size();) {
        const qsizetype chunkSize = qMin<qsizetype>(64 * 1024, output.size() - offset);
        const QByteArray chunk = output.mid(offset, chunkSize);
        offset += chunkSize;
        if (m_discardingStdoutFrame) {
            const qsizetype discarded = chunk.indexOf('\n');
            if (discarded < 0) {
                continue;
            }
            m_discardingStdoutFrame = false;
            m_stdoutBuffer.append(chunk.mid(discarded + 1));
        } else {
            m_stdoutBuffer.append(chunk);
        }
        int newline = m_stdoutBuffer.indexOf('\n');
        while (newline >= 0) {
            if (newline > kMaxFrameBytes) {
                m_stdoutBuffer.remove(0, newline + 1);
                newline = m_stdoutBuffer.indexOf('\n');
                continue;
            }
            const QByteArray line = m_stdoutBuffer.left(newline);
            m_stdoutBuffer.remove(0, newline + 1);
            const QByteArray trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                QJsonParseError parseError{};
                const QJsonDocument document = QJsonDocument::fromJson(trimmed, &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                    Q_EMIT logMessage(u"[%1] Ignoring non-JSON output: %2"_s.arg(m_config.name, QString::fromUtf8(trimmed.left(200))));
                } else {
                    handleMessage(document.object());
                }
            }
            newline = m_stdoutBuffer.indexOf('\n');
        }
        if (m_stdoutBuffer.size() > kMaxFrameBytes) {
            m_stdoutBuffer.clear();
            m_discardingStdoutFrame = true;
        }
    }
}

void McpClient::onStdioFailed(QProcess::ProcessError error)
{
    if (m_shuttingDown) {
        return;
    }
    if (error == QProcess::FailedToStart) {
        setState(McpServerState::Failed, u"Could not start '%1'."_s.arg(m_config.command));
    }
}

void McpClient::onStdioFinished(int exitCode)
{
    if (m_shuttingDown) {
        return;
    }
    failAll(u"The server process exited (code %1)."_s.arg(exitCode));
    setState(McpServerState::Failed, u"Process exited with code %1"_s.arg(exitCode));
}

void McpClient::onHttpFinished(QNetworkReply *reply, int id)
{
    reply->deleteLater();
    QByteArray body = reply->readAll();
    // Bound memory: malicious Streamable-HTTP server could otherwise OOM the
    // client (stdio path already has a 16 MiB frame cap).
    constexpr qint64 kMaxHttpBodyBytes = 16 * 1024 * 1024;
    if (body.size() > kMaxHttpBodyBytes) {
        body.truncate(kMaxHttpBodyBytes);
        qWarning() << "KateAI: MCP HTTP body truncated at 16 MiB from" << m_config.name;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    const QString session = QString::fromUtf8(reply->rawHeader("mcp-session-id"));
    if (!session.isEmpty()) {
        m_sessionId = session;
    }

    const auto it = m_pending.constFind(id);
    const QString token = it == m_pending.constEnd() ? QString() : it.value().callToken;
    const QString method = it == m_pending.constEnd() ? QString() : it.value().method;

    if (reply->error() != QNetworkReply::NoError) {
        // Qt reports an error for every HTTP status >= 400. The status code
        // says more than the generic error string, and the body often holds
        // the reason ("invalid token"), so both are passed on.
        const QString detail = status > 0
            ? i18n("HTTP %1: %2").arg(status).arg(reply->errorString())
            : reply->errorString();
        if (id > 0 && !token.isEmpty()) {
            m_pending.remove(id);
            Q_EMIT toolResult(token, false, QString(), u"Request failed: %1"_s.arg(detail));
            return;
        }
        if (id > 0) {
            // The request was already settled - its watchdog fired while the
            // reply was still in flight. The server itself is fine; one slow
            // call must not mark it as failed, or the manager tears it down and
            // restarts it.
            m_pending.remove(id);
            Q_EMIT logMessage(i18n("MCP request failed after it had already timed out: %1").arg(detail));
            return;
        }
        setState(McpServerState::Failed, u"Request failed: %1"_s.arg(detail));
        return;
    }

    const QJsonArray messages = parseHttpBody(body, contentType);
    if (messages.isEmpty()) {
        // 202 Accepted means "notification received, no reply expected".
        if (id > 0 && status == 202) {
            // A tools/call that answers with 202 and no body still has to be
            // settled; otherwise the agent would wait forever.
            if (!token.isEmpty()) {
                m_pending.remove(id);
                Q_EMIT toolResult(token, false, QString(), u"The server accepted the call but returned no result."_s);
            }
            return;
        }
        if (id > 0) {
            m_pending.remove(id);
            if (!token.isEmpty()) {
                Q_EMIT toolResult(token, false, QString(), u"Empty response from the server."_s);
            } else if (method == u"initialize"_s) {
                setState(McpServerState::Failed, u"No response to the handshake."_s);
            }
        }
        return;
    }

    for (const QJsonValue &value : messages) {
        handleMessage(value.toObject());
    }
}

void McpClient::refreshToolList()
{
    // The server announced that its tool list changed. Asking again is the
    // only way to learn the new list.
    if (m_state != McpServerState::Ready) {
        return;
    }
    QJsonObject empty;
    sendRequest(u"tools/list"_s, empty);
}

void McpClient::handleMessage(const QJsonObject &message)
{
    const QJsonValue idValue = message.value(u"id"_s);
    const bool hasId = !idValue.isUndefined() && !idValue.isNull();
    const bool hasMethod = message.contains(u"method"_s);

    if (hasMethod && !hasId) {
        // Server-initiated notification. Kate AI requests no sampling or
        // progress updates, so those are ignored. A changed tool list is
        // not: the tools the server added would stay invisible and the ones it
        // removed would stay callable.
        if (message.value(u"method"_s).toString() == u"notifications/tools/list_changed"_s) {
            refreshToolList();
        }
        return;
    }

    if (hasMethod && hasId) {
        // Server-to-client request. Answer so the server is not left waiting.
        QJsonObject response = jsonRpcError(u"Kate AI does not implement server-initiated requests."_s, -32601);
        response.insert(u"id"_s, idValue);
        writeMessage(response);
        return;
    }

    if (!hasId) {
        return;
    }

    const int id = idValue.toInt(-1);
    const auto it = m_pending.constFind(id);
    if (it == m_pending.constEnd()) {
        return;
    }
    const QString method = it.value().method;
    const QString token = it.value().callToken;
    QTimer *timeout = it.value().timeout.data();
    m_pending.remove(id);
    if (timeout) {
        timeout->stop();
        timeout->deleteLater();
    }

    if (message.contains(u"error"_s)) {
        const QJsonObject error = message.value(u"error"_s).toObject();
        const QString text = error.value(u"message"_s).toString(u"Unknown JSON-RPC error"_s);
        if (!token.isEmpty()) {
            Q_EMIT toolResult(token, false, QString(), text);
        } else if (method == u"initialize"_s) {
            failAll(text);
            setState(McpServerState::Failed, u"Handshake failed: %1"_s.arg(text));
        }
        return;
    }

    const QJsonObject result = message.value(u"result"_s).toObject();

    if (method == u"initialize"_s) {
        sendNotification(u"notifications/initialized"_s, QJsonObject());
        QJsonObject empty;
        sendRequest(u"tools/list"_s, empty);
        Q_EMIT logMessage(u"[%1] Connected using protocol %2"_s.arg(m_config.name, result.value(u"protocolVersion"_s).toString()));
        return;
    }

    if (method == u"tools/list"_s) {
        m_tools = parseToolsList(result);
        if (m_startupTimer) {
            m_startupTimer->stop();
        }
        setState(McpServerState::Ready, u"Connected · %1 tool(s)"_s.arg(m_tools.size()));
        Q_EMIT toolsChanged();
        return;
    }

    if (method == u"tools/call"_s) {
        const bool isError = mcpBoolValue(result.value(u"isError"_s), false);
        QString text = extractToolResultText(result);
        if (text.isEmpty()) {
            text = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
        }
        Q_EMIT toolResult(token, !isError, text, isError ? text : QString());
        return;
    }
}

void McpClient::failAll(const QString &error)
{
    const QList<Pending> pending = m_pending.values();
    m_pending.clear();
    for (const Pending &entry : pending) {
        if (entry.timeout) {
            entry.timeout->stop();
            entry.timeout->deleteLater();
        }
        if (entry.reply) {
            entry.reply->abort();
        }
        if (!entry.callToken.isEmpty()) {
            Q_EMIT toolResult(entry.callToken, false, QString(), error);
        }
    }
}

QList<McpTool> McpClient::parseToolsList(const QJsonObject &params) const
{
    QList<McpTool> tools;
    const QJsonArray array = params.value(u"tools"_s).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject item = value.toObject();
        const QString name = item.value(u"name"_s).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        McpTool entry;
        entry.server = m_config.name;
        entry.name = name;
        entry.description = item.value(u"description"_s).toString();
        entry.inputSchema = item.value(u"inputSchema"_s).toObject();
        const QJsonObject annotations = item.value(u"annotations"_s).toObject();
        entry.readOnly = mcpBoolValue(annotations.value(u"readOnlyHint"_s), false);
        entry.destructive = mcpBoolValue(annotations.value(u"destructiveHint"_s), false);
        // Prefix the server name so the model can tell where a tool lives.
        if (!m_config.name.isEmpty()) {
            entry.description = u"[%1] %2"_s.arg(m_config.name, entry.description);
        }
        tools.append(entry);
    }
    return tools;
}

// ---------------------------------------------------------------------------
// McpManager
// ---------------------------------------------------------------------------

McpManager::McpManager(QObject *parent)
    : QObject(parent)
{
}

McpManager::~McpManager()
{
    disconnectAll();
}

void McpManager::setWorkspace(const QString &workspace)
{
    if (m_workspace == workspace) {
        return;
    }
    m_workspace = workspace;
    reloadFromDisk();
}

void McpManager::reloadFromDisk()
{
    if (!m_loadWorkspaceConfig) {
        // The server list of this workspace was not confirmed by the user, so
        // it is not loaded. The servers that are already running stay.
        return;
    }
    const QList<McpServerConfig> loaded = McpConfigStore::load(m_workspace);
    setServers(loaded);
}

bool McpManager::saveToDisk(QString *error)
{
    return McpConfigStore::save(m_workspace, m_configs, error);
}

void McpManager::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    if (enabled) {
        connectAll();
    } else {
        disconnectAll();
    }
    Q_EMIT serversChanged();
}

void McpManager::setServers(const QList<McpServerConfig> &servers)
{
    // The server name is part of the qualified name its tools are exposed
    // under: "mcp__<server>__<tool>". A name with a double underscore is
    // collapsed in the qualified name, so the client map and the tool lookup
    // would disagree and every tool of that server would be unreachable
    // ("No MCP server named ..."). The name is normalised once, here, so both
    // sides use the same spelling.
    QList<McpServerConfig> normalised;
    normalised.reserve(servers.size());
    for (const McpServerConfig &config : servers) {
        McpServerConfig copy = config;
        copy.name = sanitizeMcpSegment(copy.name.trimmed());
        normalised.append(copy);
    }

    // Drop servers that vanished from the configuration.
    QStringList wanted;
    for (const McpServerConfig &config : normalised) {
        if (!config.name.trimmed().isEmpty()) {
            wanted.append(config.name);
        }
    }

    bool changed = false;
    const auto existing = m_configs;
    for (const McpServerConfig &config : existing) {
        if (!wanted.contains(config.name)) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        for (const McpServerConfig &config : normalised) {
            const auto it = std::find_if(existing.cbegin(), existing.cend(), [&config](const McpServerConfig &item) {
                return item.name == config.name;
            });
            if (it == existing.cend() || it->toJson() != config.toJson()) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) {
        return;
    }

    disconnectAll();
    qDeleteAll(m_clients);
    m_clients.clear();
    m_configs = normalised;
    rebuildClients();
    Q_EMIT serversChanged();
    if (m_enabled && m_autoConnect) {
        connectAll();
    }
}

void McpManager::rebuildClients()
{
    m_clients.clear();
    for (const McpServerConfig &config : m_configs) {
        if (config.name.trimmed().isEmpty()) {
            continue;
        }
        auto *client = new McpClient(config, this);
        client->setTimeoutMs(m_timeoutMs);
        connect(client, &McpClient::toolsChanged, this, [this, name = config.name] {
            refreshDerived();
            Q_EMIT toolsChanged();
            Q_EMIT serverStatusChanged(name);
        });
        connect(client, &McpClient::stateChanged, this, [this, name = config.name] {
            refreshDerived();
            Q_EMIT serverStatusChanged(name);
        });
        connect(client, &McpClient::toolResult, this, [this, name = config.name](const QString &token, bool ok, const QString &output, const QString &error) {
            handleClientResult(name, token, ok, output, error);
        });
        connect(client, &McpClient::logMessage, this, &McpManager::logMessage);
        m_clients.insert(config.name, client);
    }
    refreshDerived();
}

void McpManager::handleClientResult(const QString &serverName, const QString &callToken, bool ok, const QString &output, const QString &error)
{
    Q_UNUSED(serverName)
    Q_EMIT toolResult(callToken, ok, output, error);
}

void McpManager::refreshDerived()
{
    m_autoApproved.clear();
    m_readOnlyTools.clear();
    for (const McpTool &tool : tools()) {
        if (tool.readOnly) {
            m_readOnlyTools.append(tool.qualifiedName());
        }
        if (m_clients.value(tool.server) && m_clients.value(tool.server)->isAutoApproved(tool.name)) {
            m_autoApproved.append(tool.qualifiedName());
        }
    }
}

void McpManager::connectAll()
{
    if (!m_enabled) {
        return;
    }
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        McpClient *client = it.value();
        if (client->state() == McpServerState::Stopped || client->state() == McpServerState::Failed) {
            client->start();
        }
    }
}

void McpManager::disconnectAll()
{
    for (McpClient *client : m_clients) {
        client->stop();
    }
    m_autoApproved.clear();
    m_readOnlyTools.clear();
}

QList<McpTool> McpManager::tools() const
{
    QList<McpTool> all;
    if (!m_enabled) {
        return all;
    }
    for (McpClient *client : m_clients) {
        if (client->isReady()) {
            all.append(client->tools());
        }
    }
    return all;
}

QJsonArray McpManager::toolDefinitions() const
{
    QJsonArray definitions;
    for (const McpTool &tool : tools()) {
        definitions.append(tool.toToolDefinition());
    }
    return definitions;
}

QStringList McpManager::readyServers() const
{
    QStringList names;
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        if (it.value()->isReady()) {
            names.append(it.key());
        }
    }
    return names;
}

QStringList McpManager::toolNames() const
{
    QStringList names;
    for (const McpTool &tool : tools()) {
        names.append(tool.qualifiedName());
    }
    return names;
}

std::optional<McpTool> McpManager::tool(const QString &qualifiedName) const
{
    // tools() builds a fresh list per call, so the tool is returned by value:
    // a pointer into that temporary would dangle the moment we return.
    for (const McpTool &candidate : tools()) {
        if (candidate.qualifiedName() == qualifiedName) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool McpManager::hasTool(const QString &qualifiedName) const
{
    return tool(qualifiedName).has_value();
}

bool McpManager::isAvailable(const QString &toolName) const
{
    return isMcpToolName(toolName) && hasTool(toolName);
}

McpServerConfig McpManager::serverConfig(const QString &serverName) const
{
    const auto it = m_clients.constFind(serverName);
    if (it != m_clients.constEnd()) {
        return it.value()->config();
    }
    return {};
}

QStringList McpManager::autoApprovedTools() const
{
    return m_autoApproved;
}

QStringList McpManager::readOnlyTools() const
{
    return m_readOnlyTools;
}

QString McpManager::statusSummary() const
{
    const QStringList ready = readyServers();
    if (!m_enabled) {
        return u"MCP off"_s;
    }
    if (m_clients.isEmpty()) {
        return u"No MCP servers"_s;
    }
    if (!ready.isEmpty()) {
        return u"MCP: %1"_s.arg(ready.join(u", "_s));
    }
    return u"MCP: 0/%1 connected"_s.arg(m_clients.size());
}

bool McpManager::callTool(const QString &callToken, const QString &qualifiedName, const QJsonObject &arguments)
{
    QString serverName;
    QString toolName;
    if (!splitMcpToolName(qualifiedName, &serverName, &toolName)) {
        Q_EMIT toolResult(callToken, false, QString(), u"'%1' is not a valid MCP tool name."_s.arg(qualifiedName));
        return false;
    }
    // The advertised name is sanitized (a double underscore is collapsed), so
    // the split name can differ from the server's original tool name. Look up
    // the advertised tool and pass its original name through.
    const std::optional<McpTool> advertised = tool(qualifiedName);
    if (advertised) {
        serverName = advertised->server;
        toolName = advertised->name;
    }
    McpClient *client = m_clients.value(serverName);
    if (!client) {
        Q_EMIT toolResult(callToken, false, QString(), u"No MCP server named '%1'."_s.arg(serverName));
        return false;
    }
    return client->callTool(callToken, toolName, arguments);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

namespace McpConfigStore
{

namespace
{

constexpr auto kCredentialMarker = "${kateai-credential:";

QString credentialId(const QString &workspace, const QString &server, const QString &kind, const QString &key)
{
    const QString scope = QString::fromLatin1(QCryptographicHash::hash(workspace.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash((kind + u':' + server + u':' + key).toUtf8(), QCryptographicHash::Sha1).toHex().left(24));
    return u"mcp-%1-%2"_s.arg(scope, name);
}

bool isCredentialMarker(const QString &value)
{
    return value.startsWith(QLatin1String(kCredentialMarker)) && value.endsWith(u'}');
}

// Header and environment values are secrets (bearer tokens, API keys). The
// workspace file is frequently committed to git, so the value is kept in the
// credential store and only a marker is written to mcp.json.
bool protectValue(const QString &workspace, const QString &server, const QString &kind,
                  const QString &key, QString *value, QString *error)
{
    if (value->isEmpty() || isCredentialMarker(*value)) {
        return true;
    }
    const QString id = credentialId(workspace, server, kind, key);
    if (!CredentialStore::store(id, *value, error)) {
        return false;
    }
    *value = QString::fromLatin1(kCredentialMarker) + id + u'}';
    return true;
}

QString resolveValue(const QString &value)
{
    if (!isCredentialMarker(value)) {
        return value;
    }
    const QString id = value.mid(int(QLatin1String(kCredentialMarker).size()));
    const QString trimmed = id.left(id.size() - 1);
    bool ok = false;
    const QString secret = CredentialStore::load(trimmed, &ok);
    return ok ? secret : QString();
}

} // namespace

QString configPath(const QString &workspace)
{
    if (workspace.isEmpty()) {
        return {};
    }
    return workspace + u"/.kateai/mcp.json"_s;
}

QList<McpServerConfig> load(const QString &workspace)
{
    const QString path = configPath(workspace);
    if (path.isEmpty()) {
        return {};
    }
    QFile file(path);
    // A workspace-controlled path can be a FIFO, a device or a symlink to
    // /dev/zero; opening/readAll() would block or exhaust memory before the
    // user is even asked whether to trust the project.
    const QFileInfo fileInfo(path);
    constexpr qint64 kMaxConfigBytes = 1024 * 1024;
    if (!fileInfo.exists() || !fileInfo.isFile() || fileInfo.isSymLink()
        || fileInfo.size() <= 0 || fileInfo.size() > kMaxConfigBytes) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QByteArray payload = file.read(kMaxConfigBytes + 1);
    if (payload.size() > kMaxConfigBytes) {
        return {};
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    QList<McpServerConfig> servers = McpServerConfig::serversFromJson(document.object());
    for (McpServerConfig &server : servers) {
        for (auto it = server.headers.begin(); it != server.headers.end(); ++it) {
            it.value() = resolveValue(it.value());
        }
        for (auto it = server.env.begin(); it != server.env.end(); ++it) {
            it.value() = resolveValue(it.value());
        }
    }
    return servers;
}

bool save(const QString &workspace, const QList<McpServerConfig> &servers, QString *error)
{
    const QString path = configPath(workspace);
    if (path.isEmpty()) {
        if (error) {
            *error = u"No workspace is open."_s;
        }
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    // QSaveFile writes a temporary file and renames it over the target, so a
    // crash or a full disk mid-write cannot leave a truncated mcp.json: a
    // reader sees either the old configuration or the complete new one.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = u"Could not write %1"_s.arg(path);
        }
        return false;
    }
    // Keep header and environment values out of the workspace file: it is
    // commonly committed, and those values are bearer tokens/API keys. The
    // in-memory configuration keeps the real values; only the serialized copy
    // carries markers.
    QList<McpServerConfig> onDisk = servers;
    for (McpServerConfig &server : onDisk) {
        for (auto it = server.headers.begin(); it != server.headers.end(); ++it) {
            if (!protectValue(workspace, server.name, u"header"_s, it.key(), &it.value(), error)) {
                return false;
            }
        }
        for (auto it = server.env.begin(); it != server.env.end(); ++it) {
            if (!protectValue(workspace, server.name, u"env"_s, it.key(), &it.value(), error)) {
                return false;
            }
        }
    }
    const QJsonObject root = McpServerConfig::serversToJson(onDisk);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    // Set the permissions on the temporary file before it is renamed into
    // place: tightening after commit leaves a window in which a file carrying
    // authorization headers is world-readable.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!file.commit()) {
        if (error) {
            *error = u"Could not write %1"_s.arg(path);
        }
        return false;
    }
    // QSaveFile honours setPermissions on the temporary file, but keep a
    // post-commit tightening for platforms where it does not.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

} // namespace McpConfigStore

} // namespace KateAi
