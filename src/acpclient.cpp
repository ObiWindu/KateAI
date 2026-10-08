/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpclient.h"
#include "tools.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

static const QString JsonRpcVersion = QStringLiteral("2.0");
static const int kAcpProtocolVersion = 1;
static const int kMethodNotFound = -32601;
static const int kInvalidParams = -32602;
static const int kInternalError = -32603;
static const int kServerError = -32000;

static QString jsonText(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble());
    }
    if (value.isBool()) {
        return value.toBool() ? u"true"_s : u"false"_s;
    }
    if (value.isObject() || value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.isArray() ? QJsonDocument(value.toArray())
                                                               : QJsonDocument(value.toObject()))
                                     .toJson(QJsonDocument::Compact));
    }
    return {};
}

static QString contentBlockText(const QJsonValue &content)
{
    if (content.isString()) {
        return content.toString();
    }
    const QJsonObject obj = content.toObject();
    const QString type = obj.value(u"type"_s).toString();
    if (type == u"text"_s || type.isEmpty()) {
        return obj.value(u"text"_s).toString();
    }
    if (type == u"resource"_s) {
        return obj.value(u"resource"_s).toObject().value(u"text"_s).toString();
    }
    return obj.value(u"text"_s).toString();
}

// ACP agents use both the standard content-block array and shorthand output
// shapes. Preserve useful results even when an agent uses the latter.
static QString toolContentText(const QJsonValue &content)
{
    QStringList parts;
    const QJsonArray blocks = content.isArray() ? content.toArray() : QJsonArray{content};
    for (const QJsonValue &item : blocks) {
        if (item.isString()) {
            parts.append(item.toString());
            continue;
        }
        const QJsonObject obj = item.toObject();
        if (obj.isEmpty()) {
            continue;
        }
        const QString type = obj.value(u"type"_s).toString();
        if (type == u"content"_s || type.isEmpty()) {
            const QString text = contentBlockText(obj.contains(u"content"_s) ? obj.value(u"content"_s) : item);
            if (!text.isEmpty()) {
                parts.append(text);
            } else if (obj.contains(u"output"_s)) {
                parts.append(toolContentText(obj.value(u"output"_s)));
            } else if (obj.contains(u"result"_s)) {
                parts.append(toolContentText(obj.value(u"result"_s)));
            } else {
                parts.append(jsonText(item));
            }
        } else if (type == u"diff"_s) {
            parts.append(unifiedDiff(obj.value(u"path"_s).toString(),
                                     obj.value(u"oldText"_s).toString(),
                                     obj.value(u"newText"_s).toString()));
        } else if (type == u"text"_s) {
            parts.append(obj.value(u"text"_s).toString());
        } else if (obj.contains(u"content"_s)) {
            parts.append(toolContentText(obj.value(u"content"_s)));
        } else if (obj.contains(u"output"_s)) {
            parts.append(toolContentText(obj.value(u"output"_s)));
        } else if (obj.contains(u"result"_s)) {
            parts.append(toolContentText(obj.value(u"result"_s)));
        } else {
            parts.append(jsonText(item));
        }
    }
    return parts.join(u"\n"_s);
}

static ToolRisk riskForKind(const QString &kind, const QString &name)
{
    if (kind == u"read"_s || kind == u"search"_s || kind == u"fetch"_s || kind == u"think"_s) {
        return ToolRisk::Read;
    }
    if (kind == u"execute"_s || name == u"bash"_s) {
        return ToolRisk::Execute;
    }
    return ToolRisk::Write;
}

static QJsonObject envArrayToObject(const QJsonArray &env)
{
    QJsonObject out;
    for (const QJsonValue &item : env) {
        const QJsonObject row = item.toObject();
        const QString name = row.value(u"name"_s).toString();
        if (!name.isEmpty()) {
            out.insert(name, row.value(u"value"_s).toString());
        }
    }
    return out;
}

AcpClient::AcpClient(QObject *parent)
    : QObject(parent)
{
}

AcpClient::~AcpClient()
{
    stop();
}

void AcpClient::setSettings(const Settings &settings)
{
    const QString oldCommand = resolvedCommand(m_settings);
    const QStringList oldArgs = agentArguments(m_settings);
    m_settings = settings;
    if (m_state == State::Stopped || m_state == State::Failed) {
        return;
    }
    if (oldCommand != resolvedCommand(m_settings) || oldArgs != agentArguments(m_settings)) {
        stop();
    }
}

void AcpClient::setWorkspace(const QString &workspace)
{
    if (m_workspace == workspace) {
        return;
    }
    m_workspace = workspace;
    if (m_state != State::Stopped && m_state != State::Failed) {
        stop();
    }
}

void AcpClient::setDocumentBridge(DocumentBridge *bridge)
{
    m_bridge = bridge;
}

void AcpClient::setMcpServers(const QList<McpServerConfig> &servers)
{
    m_mcpServers = servers;
}

void AcpClient::setEditorContext(const QString &context)
{
    m_editorContext = context;
}

void AcpClient::setResumeSessionId(const QString &sessionId)
{
    m_resumeSessionId = sessionId;
}

QString AcpClient::resolvedCommand(const Settings &settings)
{
    const QString command = acpEffectiveCommand(settings);
    if (command.isEmpty() || QFileInfo(command).isAbsolute()) {
        return command;
    }
    const QString found = QStandardPaths::findExecutable(command);
    if (!found.isEmpty()) {
        return found;
    }
    const QString home = QDir::homePath();
    const QString baseName = QFileInfo(command).fileName();
    QStringList fallbacks;
    if (baseName == u"grok"_s) {
        fallbacks.append(home + u"/.grok/bin/"_s + baseName);
    }
    fallbacks.append(home + u"/.local/bin/"_s + baseName);
    for (const QString &candidate : fallbacks) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return command;
}

QStringList AcpClient::agentArguments(const Settings &settings)
{
    const QString raw = acpEffectiveArgs(settings);
    QStringList args = raw.isEmpty() ? QStringList() : QProcess::splitCommand(raw);
    if (!acpAgentIsGrok(settings)) {
        return args;
    }
    if (args.isEmpty()) {
        args = QStringList{u"agent"_s, u"stdio"_s};
    }

    int transport = args.lastIndexOf(u"stdio"_s);
    if (transport < 0) {
        transport = args.size();
    }

    QStringList flags;
    const QString model = settings.acpModel.trimmed();
    if (!model.isEmpty() && !args.contains(u"--model"_s) && !args.contains(u"-m"_s)) {
        flags << u"--model"_s << model;
    }
    if (settings.permissionMode == PermissionMode::AlwaysApprove
        && !args.contains(u"--always-approve"_s) && !args.contains(u"--yolo"_s)) {
        flags << u"--always-approve"_s;
    }

    QStringList out;
    for (int i = 0; i < transport; ++i) {
        out.append(args.at(i));
    }
    out += flags;
    for (int i = transport; i < args.size(); ++i) {
        out.append(args.at(i));
    }
    return out;
}

void AcpClient::start()
{
    if (m_state == State::Starting || m_state == State::Ready) {
        return;
    }
    spawnProcess();
}

void AcpClient::cancel()
{
    if (hasPendingPermission()) {
        QJsonObject outcome;
        outcome.insert(u"outcome"_s, u"cancelled"_s);
        QJsonObject result;
        result.insert(u"outcome"_s, outcome);
        replyResult(m_permissionRpcId, result);
        m_permissionRpcId = QJsonValue();
        m_permissionOptions = QJsonArray();
        m_permissionRequest = {};
    }
    if (m_promptId != 0 && !m_sessionId.isEmpty()) {
        QJsonObject params;
        params.insert(u"sessionId"_s, m_sessionId);
        sendNotification(u"session/cancel"_s, params);
        for (auto it = m_liveTools.begin(); it != m_liveTools.end(); ++it) {
            ToolResult result;
            result.toolCallId = it.key();
            result.name = it.value().toolName;
            result.ok = false;
            result.cancelled = true;
            result.output = u"Cancelled."_s;
            Q_EMIT toolFinished(result);
        }
        m_liveTools.clear();
    }
}

void AcpClient::stop()
{
    m_shuttingDown = true;
    if (m_startupTimer) {
        m_startupTimer->stop();
    }
    failAll(u"ACP agent disconnected."_s);
    releaseAllTerminals();
    if (m_process) {
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
    }
    m_stdoutBuffer.clear();
    m_sessionId.clear();
    m_promptId = 0;
    m_permissionRpcId = QJsonValue();
    m_queued.clear();
    m_shuttingDown = false;
    if (m_state != State::Stopped) {
        setState(State::Stopped, u"Disconnected"_s);
    }
}

void AcpClient::resetSession()
{
    m_resumeSessionId.clear();
    m_queued.clear();
    if (m_closeSession && !m_sessionId.isEmpty() && m_state == State::Ready) {
        QJsonObject params;
        params.insert(u"sessionId"_s, m_sessionId);
        sendRequest(u"session/close"_s, params, {}, 5000);
    }
    m_sessionId.clear();
    Q_EMIT sessionIdChanged(QString());
}

void AcpClient::prompt(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }
    if (m_state != State::Ready) {
        m_queued.append(QueuedPrompt{text});
        if (m_state != State::Starting) {
            start();
        }
        return;
    }
    if (m_promptId != 0) {
        m_queued.append(QueuedPrompt{text});
        return;
    }
    sendPromptNow(text);
}

void AcpClient::resolvePermission(PermissionDecision decision)
{
    if (!hasPendingPermission()) {
        return;
    }
    if (decision == PermissionDecision::AllowSession) {
        // Remembered on the Kate side; the agent also gets allow_always when present.
    }
    QJsonObject outcome;
    if (decision == PermissionDecision::Deny) {
        outcome.insert(u"outcome"_s, u"selected"_s);
        outcome.insert(u"optionId"_s, pickPermissionOption(PermissionDecision::Deny));
    } else {
        outcome.insert(u"outcome"_s, u"selected"_s);
        outcome.insert(u"optionId"_s, pickPermissionOption(decision));
    }
    if (outcome.value(u"optionId"_s).toString().isEmpty()) {
        outcome.insert(u"outcome"_s, decision == PermissionDecision::Deny ? u"cancelled"_s : u"selected"_s);
        if (decision != PermissionDecision::Deny && !m_permissionOptions.isEmpty()) {
            outcome.insert(u"optionId"_s, m_permissionOptions.first().toObject().value(u"optionId"_s).toString());
        }
    }
    QJsonObject result;
    result.insert(u"outcome"_s, outcome);
    replyResult(m_permissionRpcId, result);
    m_permissionRpcId = QJsonValue();
    m_permissionOptions = QJsonArray();
    m_permissionRequest = {};
}

void AcpClient::setState(State state, const QString &status)
{
    m_state = state;
    m_status = status;
}

void AcpClient::spawnProcess()
{
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_shuttingDown = false;
    setState(State::Starting, u"Starting ACP agent…"_s);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    if (!m_workspace.trimmed().isEmpty() && QFileInfo(m_workspace).isDir()) {
        m_process->setWorkingDirectory(m_workspace);
    }
    m_process->setProcessEnvironment(processEnvironment());

    connect(m_process, &QProcess::readyReadStandardOutput, this, &AcpClient::onStdioReadyRead);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        Q_UNUSED(error);
        if (m_shuttingDown) {
            return;
        }
        const QString message = u"Could not start ACP agent '%1': %2"_s.arg(resolvedCommand(m_settings), m_process->errorString());
        failAll(message);
        setState(State::Failed, message);
        Q_EMIT failed(message);
    });
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        if (m_shuttingDown) {
            return;
        }
        const QString message = u"ACP agent exited (code %1)."_s.arg(exitCode);
        failAll(message);
        setState(State::Failed, message);
        Q_EMIT failed(message);
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        const QString noise = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (!noise.isEmpty()) {
            Q_EMIT logMessage(noise);
        }
    });

    const QString command = resolvedCommand(m_settings);
    const QStringList args = agentArguments(m_settings);
    if (command.isEmpty()) {
        const QString message = u"ACP agent command is empty. Pick an agent in Settings or set a command."_s;
        setState(State::Failed, message);
        Q_EMIT failed(message);
        return;
    }
    m_process->start(command, args);
    if (!m_process->waitForStarted(10000)) {
        const QString message = u"Could not start ACP agent '%1': %2"_s.arg(command, m_process->errorString());
        setState(State::Failed, message);
        Q_EMIT failed(message);
        return;
    }

    m_startupTimer = new QTimer(this);
    m_startupTimer->setSingleShot(true);
    m_startupTimer->setInterval(30000);
    connect(m_startupTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Starting) {
            const QString message = u"Timed out waiting for the ACP handshake."_s;
            failAll(message);
            setState(State::Failed, message);
            Q_EMIT failed(message);
        }
    });
    m_startupTimer->start();

    sendInitialize();
}

QProcessEnvironment AcpClient::processEnvironment() const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString key = m_settings.acpApiKey.trimmed();
    if (!key.isEmpty()) {
        const QString envName = acpEffectiveApiKeyEnv(m_settings);
        if (!envName.isEmpty()) {
            env.insert(envName, key);
        }
        if (acpAgentIsGrok(m_settings)) {
            env.insert(u"XAI_API_KEY"_s, key);
            env.insert(u"GROK_CODE_XAI_API_KEY"_s, key);
        }
    }
    return env;
}

void AcpClient::sendInitialize()
{
    QJsonObject fs;
    fs.insert(u"readTextFile"_s, true);
    fs.insert(u"writeTextFile"_s, true);
    QJsonObject capabilities;
    capabilities.insert(u"fs"_s, fs);
    capabilities.insert(u"terminal"_s, true);

    QJsonObject clientInfo;
    clientInfo.insert(u"name"_s, u"kateai"_s);
    clientInfo.insert(u"title"_s, u"Kate AI"_s);
    clientInfo.insert(u"version"_s, u"0.1.0"_s);

    QJsonObject params;
    params.insert(u"protocolVersion"_s, kAcpProtocolVersion);
    params.insert(u"clientCapabilities"_s, capabilities);
    params.insert(u"clientInfo"_s, clientInfo);

    sendRequest(u"initialize"_s, params, [this](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            setState(State::Failed, error);
            Q_EMIT failed(error);
            return;
        }
        m_protocolVersion = result.value(u"protocolVersion"_s).toInt(kAcpProtocolVersion);
        if (m_protocolVersion != kAcpProtocolVersion) {
            const QString message = u"ACP agent negotiated protocol version %1, Kate AI supports %2."_s
                                        .arg(m_protocolVersion)
                                        .arg(kAcpProtocolVersion);
            setState(State::Failed, message);
            Q_EMIT failed(message);
            return;
        }
        const QJsonObject caps = result.value(u"agentCapabilities"_s).toObject();
        m_loadSession = caps.value(u"loadSession"_s).toBool();
        m_embeddedContext = caps.value(u"promptCapabilities"_s).toObject().value(u"embeddedContext"_s).toBool();
        const QJsonObject sessionCaps = caps.value(u"sessionCapabilities"_s).toObject();
        m_resumeSession = sessionCaps.contains(u"resume"_s);
        m_closeSession = sessionCaps.contains(u"close"_s);
        maybeAuthenticate(result.value(u"authMethods"_s).toArray());
    });
}

void AcpClient::maybeAuthenticate(const QJsonArray &authMethods)
{
    if (authMethods.isEmpty()) {
        openSession();
        return;
    }

    // If no API key is configured, skip authentication and let the agent use
    // its existing CLI login session. Most ACP agents (Grok Build, Claude Agent,
    // Codex, Gemini CLI, etc.) support this workflow.
    const bool hasApiKey = !m_settings.acpApiKey.trimmed().isEmpty();
    if (!hasApiKey) {
        openSession();
        return;
    }

    const QString effectiveArgs = acpEffectiveArgs(m_settings);
    const bool codexAgent = m_settings.acpAgentId == u"codex-acp"_s
        || effectiveArgs.contains(u"@agentclientprotocol/codex-acp"_s);
    const QString preferredMethod = codexAgent && m_settings.acpApiKey.trimmed().isEmpty()
        ? u"chat-gpt"_s : QString();
    QJsonObject selectedMethod;
    for (const QJsonValue &value : authMethods) {
        const QJsonObject method = value.toObject();
        QString id = method.value(u"id"_s).toString();
        if (id.isEmpty()) id = method.value(u"methodId"_s).toString();
        if (selectedMethod.isEmpty() || (!preferredMethod.isEmpty() && id == preferredMethod)) {
            selectedMethod = method;
        }
        if (!preferredMethod.isEmpty() && id == preferredMethod) break;
    }
    QString methodId = selectedMethod.value(u"id"_s).toString();
    if (methodId.isEmpty()) methodId = selectedMethod.value(u"methodId"_s).toString();
    if (methodId.isEmpty()) {
        openSession();
        return;
    }
    QJsonObject params;
    params.insert(u"methodId"_s, methodId);
    sendRequest(u"authenticate"_s, params, [this, methodId](const QJsonObject &, const QString &error) {
        if (!error.isEmpty()) {
            const QString message = u"ACP authentication (%1) failed: %2. Complete the agent's CLI login, then try again."_s.arg(methodId, error);
            setState(State::Failed, message);
            Q_EMIT failed(message);
            return;
        }
        openSession();
    });
}

void AcpClient::openSession()
{
    if (!m_resumeSessionId.isEmpty() && (m_loadSession || m_resumeSession)) {
        sendSessionLoadOrResume();
        return;
    }
    sendSessionNew();
}

void AcpClient::sendSessionNew()
{
    QJsonObject params;
    params.insert(u"cwd"_s, m_workspace);
    params.insert(u"mcpServers"_s, mcpServersPayload());
    const QJsonObject meta = sessionMeta();
    if (!meta.isEmpty()) {
        params.insert(u"_meta"_s, meta);
    }
    sendRequest(u"session/new"_s, params, [this](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            setState(State::Failed, error);
            Q_EMIT failed(error);
            return;
        }
        m_sessionId = result.value(u"sessionId"_s).toString();
        if (m_sessionId.isEmpty()) {
            const QString message = u"ACP agent did not return a session id."_s;
            setState(State::Failed, message);
            Q_EMIT failed(message);
            return;
        }
        if (m_startupTimer) {
            m_startupTimer->stop();
        }
        setState(State::Ready, u"ACP ready"_s);
        Q_EMIT sessionIdChanged(m_sessionId);
        Q_EMIT ready();
        if (!m_queued.isEmpty() && m_promptId == 0) {
            const QueuedPrompt next = m_queued.takeFirst();
            sendPromptNow(next.text);
        }
    });
}

void AcpClient::sendSessionLoadOrResume()
{
    const QString method = m_resumeSession ? u"session/resume"_s : u"session/load"_s;
    QJsonObject params;
    params.insert(u"sessionId"_s, m_resumeSessionId);
    params.insert(u"cwd"_s, m_workspace);
    params.insert(u"mcpServers"_s, mcpServersPayload());
    sendRequest(method, params, [this](const QJsonObject &, const QString &error) {
        if (!error.isEmpty()) {
            m_resumeSessionId.clear();
            sendSessionNew();
            return;
        }
        m_sessionId = m_resumeSessionId;
        if (m_startupTimer) {
            m_startupTimer->stop();
        }
        setState(State::Ready, u"ACP ready"_s);
        Q_EMIT sessionIdChanged(m_sessionId);
        Q_EMIT ready();
        if (!m_queued.isEmpty() && m_promptId == 0) {
            const QueuedPrompt next = m_queued.takeFirst();
            sendPromptNow(next.text);
        }
    });
}

void AcpClient::sendPromptNow(const QString &text)
{
    m_promptText = text;
    m_currentText.clear();
    m_currentThinking.clear();
    m_currentPlan = QJsonArray();
    m_toolCalls = QJsonArray();
    m_liveTools.clear();

    QJsonObject params;
    params.insert(u"sessionId"_s, m_sessionId);
    params.insert(u"prompt"_s, promptBlocks(text));
    m_promptId = sendRequest(u"session/prompt"_s, params, [this](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            finishPrompt(QString(), error);
            return;
        }
        finishPrompt(result.value(u"stopReason"_s).toString(u"end_turn"_s));
    }, 0);
}

QJsonArray AcpClient::promptBlocks(const QString &text) const
{
    QJsonArray prompt;
    if (!m_editorContext.trimmed().isEmpty()) {
        QJsonObject context;
        context.insert(u"type"_s, u"text"_s);
        const QString contextText = u"Editor context:\n"_s + m_editorContext.trimmed();
        context.insert(u"text"_s, contextText);
        prompt.append(context);
    }
    QJsonObject user;
    user.insert(u"type"_s, u"text"_s);
    user.insert(u"text"_s, text);
    prompt.append(user);
    return prompt;
}

QJsonObject AcpClient::sessionMeta() const
{
    QJsonObject meta;
    // Grok Build reads yoloMode / autoMode / rules from session _meta.
    // Other agents may reject unknown _meta, so only Grok gets it.
    if (!acpAgentIsGrok(m_settings)) {
        return meta;
    }
    if (m_settings.permissionMode == PermissionMode::AlwaysApprove) {
        meta.insert(u"yoloMode"_s, true);
    } else if (m_settings.permissionMode == PermissionMode::AcceptEdits) {
        meta.insert(u"autoMode"_s, true);
    }
    QStringList rules;
    if (!m_settings.globalRules.trimmed().isEmpty()) {
        rules.append(m_settings.globalRules.trimmed());
    }
    if (!m_settings.extraSystemPrompt.trimmed().isEmpty()) {
        rules.append(m_settings.extraSystemPrompt.trimmed());
    }
    if (!rules.isEmpty()) {
        meta.insert(u"rules"_s, rules.join(u"\n\n"_s));
    }
    return meta;
}

QJsonArray AcpClient::mcpServersPayload() const
{
    QJsonArray out;
    for (const McpServerConfig &server : m_mcpServers) {
        if (!server.enabled) {
            continue;
        }
        QJsonObject obj;
        obj.insert(u"name"_s, server.name);
        if (server.transport == McpTransport::Http) {
            obj.insert(u"type"_s, u"http"_s);
            obj.insert(u"url"_s, server.url);
            QJsonArray headers;
            for (auto it = server.headers.constBegin(); it != server.headers.constEnd(); ++it) {
                QJsonObject header;
                header.insert(u"name"_s, it.key());
                header.insert(u"value"_s, it.value());
                headers.append(header);
            }
            obj.insert(u"headers"_s, headers);
        } else {
            obj.insert(u"command"_s, server.command);
            obj.insert(u"args"_s, QJsonArray::fromStringList(server.args));
            QJsonArray env;
            for (auto it = server.env.constBegin(); it != server.env.constEnd(); ++it) {
                QJsonObject row;
                row.insert(u"name"_s, it.key());
                row.insert(u"value"_s, it.value());
                env.append(row);
            }
            obj.insert(u"env"_s, env);
        }
        out.append(obj);
    }
    return out;
}

int AcpClient::sendRequest(const QString &method, const QJsonObject &params,
                           std::function<void(const QJsonObject &, const QString &)> done,
                           int timeoutMs)
{
    const int id = m_nextId++;
    Pending pending;
    pending.id = id;
    pending.method = method;
    pending.done = std::move(done);
    if (timeoutMs > 0) {
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(timeoutMs);
        connect(timer, &QTimer::timeout, this, [this, id, method, timeoutMs] {
            const auto it = m_pending.constFind(id);
            if (it == m_pending.constEnd()) {
                return;
            }
            auto callback = it.value().done;
            m_pending.remove(id);
            if (callback) {
                callback(QJsonObject(), u"ACP '%1' timed out after %2 ms."_s.arg(method).arg(timeoutMs));
            }
        });
        pending.timeout = timer;
        timer->start();
    }
    m_pending.insert(id, pending);

    QJsonObject message;
    message.insert(u"jsonrpc"_s, JsonRpcVersion);
    message.insert(u"id"_s, id);
    message.insert(u"method"_s, method);
    message.insert(u"params"_s, params);
    writeMessage(message);
    return id;
}

void AcpClient::sendNotification(const QString &method, const QJsonObject &params)
{
    QJsonObject message;
    message.insert(u"jsonrpc"_s, JsonRpcVersion);
    message.insert(u"method"_s, method);
    message.insert(u"params"_s, params);
    writeMessage(message);
}

void AcpClient::writeMessage(const QJsonObject &message)
{
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return;
    }
    QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    payload.append('\n');
    m_process->write(payload);
}

void AcpClient::replyResult(const QJsonValue &id, const QJsonObject &result)
{
    QJsonObject message;
    message.insert(u"jsonrpc"_s, JsonRpcVersion);
    message.insert(u"id"_s, id);
    message.insert(u"result"_s, result);
    writeMessage(message);
}

void AcpClient::replyError(const QJsonValue &id, int code, const QString &messageText)
{
    QJsonObject error;
    error.insert(u"code"_s, code);
    error.insert(u"message"_s, messageText);
    QJsonObject message;
    message.insert(u"jsonrpc"_s, JsonRpcVersion);
    message.insert(u"id"_s, id);
    message.insert(u"error"_s, error);
    writeMessage(message);
}

void AcpClient::onStdioReadyRead()
{
    if (!m_process) {
        return;
    }
    m_stdoutBuffer.append(m_process->readAllStandardOutput());
    handleBuffer();
}

void AcpClient::handleBuffer()
{
    int newline = m_stdoutBuffer.indexOf('\n');
    while (newline >= 0) {
        QByteArray line = m_stdoutBuffer.left(newline);
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.isEmpty() && trimmed.startsWith('{')) {
            const QJsonDocument doc = QJsonDocument::fromJson(trimmed);
            if (doc.isObject()) {
                handleMessage(doc.object());
            }
        }
        newline = m_stdoutBuffer.indexOf('\n');
    }
}

void AcpClient::handleMessage(const QJsonObject &message)
{
    if (message.contains(u"method"_s)) {
        if (message.contains(u"id"_s)) {
            handleIncomingRequest(message);
        } else {
            handleNotification(message);
        }
        return;
    }
    if (message.contains(u"id"_s)) {
        handleResponse(message);
    }
}

void AcpClient::handleResponse(const QJsonObject &message)
{
    const int id = message.value(u"id"_s).toInt();
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        return;
    }
    Pending pending = it.value();
    m_pending.erase(it);
    if (pending.timeout) {
        pending.timeout->stop();
        pending.timeout->deleteLater();
    }
    if (!pending.done) {
        return;
    }
    if (message.contains(u"error"_s)) {
        const QJsonObject error = message.value(u"error"_s).toObject();
        pending.done(QJsonObject(), error.value(u"message"_s).toString(u"ACP request failed."_s));
        return;
    }
    pending.done(message.value(u"result"_s).toObject(), QString());
}

void AcpClient::handleIncomingRequest(const QJsonObject &message)
{
    const QString method = message.value(u"method"_s).toString();
    const QJsonValue id = message.value(u"id"_s);
    const QJsonObject params = message.value(u"params"_s).toObject();

    if (method == u"session/request_permission"_s) {
        handleRequestPermission(id, params);
        return;
    }
    if (method == u"fs/read_text_file"_s) {
        handleFsRead(id, params);
        return;
    }
    if (method == u"fs/write_text_file"_s) {
        handleFsWrite(id, params);
        return;
    }
    if (method == u"terminal/create"_s) {
        handleTerminalCreate(id, params);
        return;
    }
    if (method == u"terminal/output"_s) {
        handleTerminalOutput(id, params);
        return;
    }
    if (method == u"terminal/wait_for_exit"_s) {
        handleTerminalWait(id, params);
        return;
    }
    if (method == u"terminal/kill"_s) {
        handleTerminalKill(id, params);
        return;
    }
    if (method == u"terminal/release"_s) {
        handleTerminalRelease(id, params);
        return;
    }
    replyError(id, kMethodNotFound, u"Method not found: %1"_s.arg(method));
}

void AcpClient::handleNotification(const QJsonObject &message)
{
    const QString method = message.value(u"method"_s).toString();
    const QJsonObject params = message.value(u"params"_s).toObject();
    if (method == u"session/update"_s || method == u"x.ai/session/update"_s) {
        QJsonObject update = params.value(u"update"_s).toObject();
        if (update.isEmpty()) {
            update = params;
        }
        handleSessionUpdate(update);
    }
}

void AcpClient::handleSessionUpdate(const QJsonObject &update)
{
    const QString kind = update.value(u"sessionUpdate"_s).toString();
    if (kind == u"agent_message_chunk"_s) {
        const QString delta = contentBlockText(update.value(u"content"_s));
        if (!delta.isEmpty()) {
            m_currentText += delta;
            Q_EMIT textDelta(delta);
        }
        return;
    }
    if (kind == u"agent_thought_chunk"_s) {
        const QString delta = contentBlockText(update.value(u"content"_s));
        if (!delta.isEmpty()) {
            m_currentThinking += delta;
            Q_EMIT thinkingDelta(delta);
        }
        return;
    }
    if (kind == u"plan"_s) {
        QJsonArray katePlan;
        const QJsonArray entries = update.value(u"entries"_s).toArray();
        int index = 0;
        for (const QJsonValue &value : entries) {
            const QJsonObject entry = value.toObject();
            QJsonObject step;
            step.insert(u"id"_s, QString::number(++index));
            step.insert(u"description"_s, entry.value(u"content"_s).toString());
            const QString status = entry.value(u"status"_s).toString();
            step.insert(u"completed"_s, status == u"completed"_s);
            step.insert(u"inProgress"_s, status == u"in_progress"_s);
            katePlan.append(step);
        }
        m_currentPlan = katePlan;
        Q_EMIT planUpdated(katePlan);
        return;
    }
    if (kind == u"tool_call"_s) {
        const PermissionRequest request = permissionFromToolCall(update);
        m_liveTools.insert(request.toolCallId, request);
        QJsonObject function;
        function.insert(u"name"_s, request.toolName);
        function.insert(u"arguments"_s, request.details.isEmpty() ? u"{}"_s : request.details);
        QJsonObject call;
        call.insert(u"id"_s, request.toolCallId);
        call.insert(u"type"_s, u"function"_s);
        call.insert(u"function"_s, function);
        m_toolCalls.append(call);
        Q_EMIT toolStarted(request);
        return;
    }
    if (kind == u"tool_call_update"_s) {
        const QString toolCallId = update.value(u"toolCallId"_s).toString();
        const QString status = update.value(u"status"_s).toString();
        PermissionRequest existing = m_liveTools.value(toolCallId);
        if (existing.toolCallId.isEmpty()) {
            existing = permissionFromToolCall(update);
            m_liveTools.insert(toolCallId, existing);
            Q_EMIT toolStarted(existing);
        }
        if (status == u"completed"_s || status == u"failed"_s) {
            ToolResult result;
            result.toolCallId = toolCallId;
            result.name = existing.toolName;
            result.ok = status == u"completed"_s;
            result.output = toolContentText(update.value(u"content"_s));
            if (result.output.isEmpty()) {
                result.output = jsonText(update.value(u"rawOutput"_s));
            }
            if (result.output.isEmpty()) {
                result.output = jsonText(update.value(u"output"_s));
            }
            if (result.output.isEmpty()) {
                result.output = jsonText(update.value(u"result"_s));
            }
            m_liveTools.remove(toolCallId);
            Q_EMIT toolFinished(result);
        }
        return;
    }
}

PermissionRequest AcpClient::permissionFromToolCall(const QJsonObject &toolCall) const
{
    PermissionRequest request;
    request.toolCallId = toolCall.value(u"toolCallId"_s).toString();
    request.toolName = toolCall.value(u"name"_s).toString();
    if (request.toolName.isEmpty()) {
        request.toolName = toolCall.value(u"kind"_s).toString();
    }
    if (request.toolName.isEmpty()) {
        request.toolName = u"tool"_s;
    }
    request.summary = toolCall.value(u"title"_s).toString();
    if (request.summary.isEmpty()) {
        request.summary = request.toolName;
    }
    request.risk = riskForKind(toolCall.value(u"kind"_s).toString(), request.toolName);
    request.details = jsonText(toolCall.value(u"rawInput"_s));

    const QJsonArray locations = toolCall.value(u"locations"_s).toArray();
    if (!locations.isEmpty()) {
        request.path = locations.first().toObject().value(u"path"_s).toString();
    }
    const QJsonArray content = toolCall.value(u"content"_s).toArray();
    for (const QJsonValue &item : content) {
        const QJsonObject obj = item.toObject();
        if (obj.value(u"type"_s).toString() == u"diff"_s) {
            request.path = obj.value(u"path"_s).toString();
            request.describeDiff = unifiedDiff(request.path,
                                               obj.value(u"oldText"_s).toString(),
                                               obj.value(u"newText"_s).toString());
            break;
        }
    }
    if (request.describeDiff.isEmpty() && request.details.contains(u"old_string"_s)) {
        const QJsonDocument doc = QJsonDocument::fromJson(request.details.toUtf8());
        if (doc.isObject()) {
            const QJsonObject args = doc.object();
            request.path = args.value(u"path"_s).toString();
            request.describeDiff = unifiedDiff(request.path,
                                               args.value(u"old_string"_s).toString(),
                                               args.value(u"new_string"_s).toString());
        }
    }
    return request;
}

void AcpClient::handleRequestPermission(const QJsonValue &id, const QJsonObject &params)
{
    const QJsonObject toolCall = params.value(u"toolCall"_s).toObject();
    PermissionRequest request = permissionFromToolCall(toolCall);
    if (request.toolCallId.isEmpty()) {
        request.toolCallId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    m_permissionRpcId = id;
    m_permissionOptions = params.value(u"options"_s).toArray();
    m_permissionRequest = request;

    if (autoResolvePermission(request)) {
        resolvePermission(m_settings.permissionMode == PermissionMode::AlwaysApprove
                              ? PermissionDecision::AllowSession
                              : PermissionDecision::AllowOnce);
        return;
    }
    Q_EMIT permissionNeeded(request);
}

bool AcpClient::autoResolvePermission(const PermissionRequest &request)
{
    if (m_settings.permissionMode == PermissionMode::AlwaysApprove) {
        return true;
    }
    if (m_settings.autoApproveTools.contains(request.toolName)) {
        return true;
    }
    if (m_settings.permissionMode == PermissionMode::AcceptEdits && request.risk == ToolRisk::Read) {
        return true;
    }
    return false;
}

QString AcpClient::pickPermissionOption(PermissionDecision decision) const
{
    const QString wanted = decision == PermissionDecision::Deny
        ? u"reject_once"_s
        : (decision == PermissionDecision::AllowSession ? u"allow_always"_s : u"allow_once"_s);
    const QString fallback = decision == PermissionDecision::Deny ? u"reject_always"_s : u"allow_once"_s;
    QString first;
    for (const QJsonValue &value : m_permissionOptions) {
        const QJsonObject option = value.toObject();
        const QString kind = option.value(u"kind"_s).toString();
        const QString optionId = option.value(u"optionId"_s).toString();
        if (first.isEmpty()) {
            first = optionId;
        }
        if (kind == wanted) {
            return optionId;
        }
    }
    for (const QJsonValue &value : m_permissionOptions) {
        const QJsonObject option = value.toObject();
        if (option.value(u"kind"_s).toString() == fallback) {
            return option.value(u"optionId"_s).toString();
        }
    }
    return first;
}

void AcpClient::handleFsRead(const QJsonValue &id, const QJsonObject &params)
{
    const QString path = params.value(u"path"_s).toString();
    QString error;
    if (!pathAllowed(path, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    QString contents;
    if (!m_bridge || !m_bridge->readDocument(path, &contents)) {
        replyError(id, kServerError, u"Could not read %1."_s.arg(path));
        return;
    }
    const int line = params.value(u"line"_s).toInt(0);
    const int limit = params.value(u"limit"_s).toInt(0);
    QJsonObject result;
    result.insert(u"content"_s, sliceLines(contents, line, limit));
    replyResult(id, result);
}

void AcpClient::handleFsWrite(const QJsonValue &id, const QJsonObject &params)
{
    const QString path = params.value(u"path"_s).toString();
    QString error;
    if (!pathAllowed(path, true, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    if (!m_bridge) {
        replyError(id, kInternalError, u"No document bridge is available."_s);
        return;
    }
    if (!m_bridge->writeDocument(path, params.value(u"content"_s).toString(), &error)) {
        replyError(id, kServerError, error.isEmpty() ? u"Could not write %1."_s.arg(path) : error);
        return;
    }
    replyResult(id, QJsonObject());
}

QString AcpClient::sliceLines(const QString &text, int line, int limit) const
{
    if (line <= 0 && limit <= 0) {
        return text;
    }
    const QStringList lines = text.split(u'\n');
    int start = line > 0 ? line - 1 : 0;
    if (start < 0) {
        start = 0;
    }
    if (start >= lines.size()) {
        return {};
    }
    const int count = limit > 0 ? qMin(limit, lines.size() - start) : (lines.size() - start);
    return QStringList(lines.mid(start, count)).join(u'\n');
}

bool AcpClient::pathAllowed(const QString &path, bool forWrite, QString *error) const
{
    if (path.trimmed().isEmpty() || !QFileInfo(path).isAbsolute()) {
        if (error) {
            *error = u"ACP file paths must be absolute."_s;
        }
        return false;
    }
    const Sandbox sandbox = currentSandbox();
    if (forWrite) {
        return sandbox.allowsWrite(path, error);
    }
    return sandbox.allowsRead(path, error);
}

Sandbox AcpClient::currentSandbox() const
{
    return Sandbox(m_workspace, m_settings.sandbox, m_settings.extraDenyGlobs);
}

void AcpClient::handleTerminalCreate(const QJsonValue &id, const QJsonObject &params)
{
    if (m_settings.sandbox == SandboxProfile::ReadOnly) {
        replyError(id, kServerError, u"Terminals are disabled in the read-only sandbox."_s);
        return;
    }
    QString command;
    QStringList args;
    const QJsonValue rawCommand = params.value(u"command"_s);
    if (rawCommand.isArray()) {
        const QJsonArray argv = rawCommand.toArray();
        for (const QJsonValue &value : argv) {
            if (command.isEmpty()) {
                command = value.toString();
            } else {
                args.append(value.toString());
            }
        }
    } else {
        command = rawCommand.toString();
    }
    const QJsonArray rawArgs = params.value(u"args"_s).toArray();
    for (const QJsonValue &value : rawArgs) {
        args.append(value.toString());
    }
    if (command.trimmed().isEmpty()) {
        replyError(id, kInvalidParams, u"terminal/create requires a command."_s);
        return;
    }

    QString cwd = params.value(u"cwd"_s).toString();
    if (cwd.isEmpty()) {
        cwd = m_workspace;
    }
    QString error;
    if (!pathAllowed(cwd, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }

    auto *term = new Terminal;
    term->id = u"term_%1"_s.arg(m_nextTerminal++);
    term->outputByteLimit = params.value(u"outputByteLimit"_s).toInt(1024 * 1024);
    term->process = new QProcess(this);
    term->process->setProcessChannelMode(QProcess::MergedChannels);
    term->process->setWorkingDirectory(cwd);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QJsonObject extra = envArrayToObject(params.value(u"env"_s).toArray());
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        env.insert(it.key(), it.value().toString());
    }
    term->process->setProcessEnvironment(env);

    connect(term->process, &QProcess::readyRead, this, [this, term] {
        appendTerminalOutput(term, term->process->readAll());
    });
    connect(term->process, &QProcess::finished, this, [this, term](int exitCode, QProcess::ExitStatus) {
        markTerminalExited(term, exitCode, QString());
    });
    connect(term->process, &QProcess::errorOccurred, this, [this, term](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            markTerminalExited(term, 127, QString());
        }
    });

    QString wrapError;
    QStringList wrapped;
    if (args.isEmpty()) {
        // A lone command string is a shell line. Pass it as one `-c` argument
        // so inner quotes stay part of the script.
        wrapped = currentSandbox().wrapCommand(command, &wrapError);
    } else {
        // ACP argv stays argv. wrapArgv never rewrites this into `bash -c '…'`.
        wrapped = currentSandbox().wrapArgv(command, args, &wrapError);
    }
    if (wrapped.isEmpty()) {
        replyError(id, kServerError, wrapError.isEmpty() ? u"Failed to wrap the terminal command."_s : wrapError);
        delete term->process;
        delete term;
        return;
    }

    m_terminals.insert(term->id, term);
    term->process->start(wrapped.first(), wrapped.mid(1));
    QJsonObject result;
    result.insert(u"terminalId"_s, term->id);
    replyResult(id, result);
}

void AcpClient::handleTerminalOutput(const QJsonValue &id, const QJsonObject &params)
{
    const QString terminalId = params.value(u"terminalId"_s).toString();
    Terminal *term = m_terminals.value(terminalId);
    if (!term) {
        replyError(id, kServerError, u"Unknown terminal '%1'."_s.arg(terminalId));
        return;
    }
    if (term->process) {
        appendTerminalOutput(term, term->process->readAll());
    }
    QJsonObject result;
    result.insert(u"output"_s, QString::fromUtf8(term->output));
    result.insert(u"truncated"_s, term->truncated);
    if (term->exited) {
        QJsonObject status;
        status.insert(u"exitCode"_s, term->exitCode);
        if (term->signal.isEmpty()) {
            status.insert(u"signal"_s, QJsonValue::Null);
        } else {
            status.insert(u"signal"_s, term->signal);
        }
        result.insert(u"exitStatus"_s, status);
    }
    replyResult(id, result);
}

void AcpClient::handleTerminalWait(const QJsonValue &id, const QJsonObject &params)
{
    const QString terminalId = params.value(u"terminalId"_s).toString();
    Terminal *term = m_terminals.value(terminalId);
    if (!term) {
        replyError(id, kServerError, u"Unknown terminal '%1'."_s.arg(terminalId));
        return;
    }
    if (term->exited) {
        QJsonObject result;
        result.insert(u"exitCode"_s, term->exitCode);
        if (term->signal.isEmpty()) {
            result.insert(u"signal"_s, QJsonValue::Null);
        } else {
            result.insert(u"signal"_s, term->signal);
        }
        replyResult(id, result);
        return;
    }
    term->waiters.append(id);
}

void AcpClient::handleTerminalKill(const QJsonValue &id, const QJsonObject &params)
{
    const QString terminalId = params.value(u"terminalId"_s).toString();
    Terminal *term = m_terminals.value(terminalId);
    if (!term) {
        replyError(id, kServerError, u"Unknown terminal '%1'."_s.arg(terminalId));
        return;
    }
    killTerminal(term, false);
    replyResult(id, QJsonObject());
}

void AcpClient::handleTerminalRelease(const QJsonValue &id, const QJsonObject &params)
{
    const QString terminalId = params.value(u"terminalId"_s).toString();
    Terminal *term = m_terminals.take(terminalId);
    if (!term) {
        replyError(id, kServerError, u"Unknown terminal '%1'."_s.arg(terminalId));
        return;
    }
    killTerminal(term, true);
    if (term->process) {
        term->process->deleteLater();
    }
    delete term;
    replyResult(id, QJsonObject());
}

void AcpClient::appendTerminalOutput(Terminal *term, const QByteArray &chunk)
{
    if (chunk.isEmpty()) {
        return;
    }
    term->output.append(chunk);
    if (term->outputByteLimit > 0 && term->output.size() > term->outputByteLimit) {
        term->truncated = true;
        term->output = term->output.right(term->outputByteLimit);
    }
}

void AcpClient::markTerminalExited(Terminal *term, int exitCode, const QString &signal)
{
    if (!term || term->exited) {
        return;
    }
    term->exited = true;
    term->exitCode = exitCode;
    term->signal = signal;
    if (term->process) {
        appendTerminalOutput(term, term->process->readAll());
    }
    const QList<QJsonValue> waiters = term->waiters;
    term->waiters.clear();
    for (const QJsonValue &waiter : waiters) {
        QJsonObject result;
        result.insert(u"exitCode"_s, term->exitCode);
        if (term->signal.isEmpty()) {
            result.insert(u"signal"_s, QJsonValue::Null);
        } else {
            result.insert(u"signal"_s, term->signal);
        }
        replyResult(waiter, result);
    }
}

void AcpClient::killTerminal(Terminal *term, bool release)
{
    if (!term || !term->process) {
        return;
    }
    if (term->process->state() != QProcess::NotRunning) {
        term->process->terminate();
        if (!term->process->waitForFinished(500)) {
            term->process->kill();
            term->process->waitForFinished(500);
        }
    }
    if (!term->exited) {
        markTerminalExited(term, term->process->exitCode(), QString());
    }
    Q_UNUSED(release);
}

void AcpClient::releaseAllTerminals()
{
    const auto terminals = m_terminals;
    m_terminals.clear();
    for (Terminal *term : terminals) {
        killTerminal(term, true);
        if (term->process) {
            term->process->deleteLater();
        }
        delete term;
    }
}

void AcpClient::failAll(const QString &error)
{
    const auto pending = m_pending;
    m_pending.clear();
    for (const Pending &item : pending) {
        if (item.timeout) {
            item.timeout->stop();
            item.timeout->deleteLater();
        }
        if (item.done) {
            item.done(QJsonObject(), error);
        }
    }
    if (m_promptId != 0) {
        m_promptId = 0;
    }
}

void AcpClient::finishPrompt(const QString &stopReason, const QString &error)
{
    m_promptId = 0;
    if (!error.isEmpty()) {
        Q_EMIT failed(error);
        return;
    }
    Q_EMIT promptFinished(stopReason.isEmpty() ? u"end_turn"_s : stopReason, m_currentText, m_toolCalls);
    if (!m_queued.isEmpty() && m_state == State::Ready) {
        const QueuedPrompt next = m_queued.takeFirst();
        sendPromptNow(next.text);
    }
}

} // namespace KateAi
