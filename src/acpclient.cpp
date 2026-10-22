/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpclient.h"
#include "tools.h"
#include "graph/projectgraph.h"

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
        } else if (obj.contains(u"stdout"_s) || obj.contains(u"stderr"_s)) {
            const QString stdoutText = jsonText(obj.value(u"stdout"_s));
            const QString stderrText = jsonText(obj.value(u"stderr"_s));
            if (!stdoutText.isEmpty()) {
                parts.append(stdoutText);
            }
            if (!stderrText.isEmpty()) {
                parts.append(stderrText);
            }
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
    m_shuttingDown = true;
    disconnect();
    stop();
}

static QStringList argsWithoutModelFlag(const QStringList &args)
{
    QStringList out;
    for (int i = 0; i < args.size(); ++i) {
        if ((args.at(i) == u"--model"_s || args.at(i) == u"-m"_s) && i + 1 < args.size()) {
            ++i;
            continue;
        }
        out.append(args.at(i));
    }
    return out;
}

void AcpClient::setSettings(const Settings &settings)
{
    const QString oldCommand = resolvedCommand(m_settings);
    const QStringList oldArgs = agentArguments(m_settings);
    const QString oldModel = m_settings.acpModel;
    m_settings = settings;
    if (m_state == State::Stopped || m_state == State::Failed) {
        return;
    }
    const QStringList newArgs = agentArguments(m_settings);
    const bool commandChanged = oldCommand != resolvedCommand(m_settings);
    if (commandChanged || argsWithoutModelFlag(oldArgs) != argsWithoutModelFlag(newArgs)) {
        stop();
        return;
    }
    // Same process. A model change is applied on the live session, the way
    // Zed does, instead of restarting the agent so a --model flag is reread.
    if (oldModel != m_settings.acpModel && !setModel(m_settings.acpModel)) {
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

void AcpClient::setProjectGraph(ProjectGraph *graph)
{
    m_projectGraph = graph;
}

void AcpClient::setResumeSessionId(const QString &sessionId)
{
    m_resumeSessionId = sessionId;
}

static void appendModelId(QStringList *models, const QString &id)
{
    const QString trimmed = id.trimmed();
    if (!trimmed.isEmpty() && !models->contains(trimmed)) {
        models->append(trimmed);
    }
}

static void appendModelEntry(QStringList *models, const QJsonValue &value)
{
    if (value.isString()) {
        appendModelId(models, value.toString());
        return;
    }
    if (!value.isObject()) {
        return;
    }
    const QJsonObject obj = value.toObject();
    // Grouped SessionConfigSelectOptions nest the real values under "options".
    if (obj.contains(u"options"_s) && !obj.contains(u"value"_s) && !obj.contains(u"modelId"_s)) {
        const QJsonArray nested = obj.value(u"options"_s).toArray();
        for (const QJsonValue &child : nested) {
            appendModelEntry(models, child);
        }
        return;
    }
    QString id = obj.value(u"value"_s).toString();
    if (id.isEmpty()) {
        id = obj.value(u"modelId"_s).toString();
    }
    if (id.isEmpty()) {
        id = obj.value(u"id"_s).toString();
    }
    appendModelId(models, id);
}

static bool isModelConfigOption(const QJsonObject &option)
{
    const QString category = option.value(u"category"_s).toString();
    if (category == u"model_config"_s || category == u"mode"_s || category == u"thought_level"_s) {
        return false;
    }
    if (category == u"model"_s) {
        return true;
    }
    return option.value(u"id"_s).toString().compare(u"model"_s, Qt::CaseInsensitive) == 0;
}

// Returns true when a model selector was present, even if it had no values.
static bool collectModelConfig(const QJsonArray &configOptions, QStringList *models, QString *current, QString *configId)
{
    bool found = false;
    for (const QJsonValue &optionValue : configOptions) {
        const QJsonObject option = optionValue.toObject();
        if (!isModelConfigOption(option)) {
            continue;
        }
        const QString type = option.value(u"type"_s).toString();
        if (!type.isEmpty() && type != u"select"_s) {
            continue;
        }
        found = true;
        *configId = option.value(u"id"_s).toString();
        *current = option.value(u"currentValue"_s).toString();
        models->clear();
        const QJsonArray options = option.value(u"options"_s).toArray();
        for (const QJsonValue &optValue : options) {
            appendModelEntry(models, optValue);
        }
    }
    return found;
}

void AcpClient::ingestSessionModels(const QJsonObject &result, bool applyPreferred)
{
    QStringList models;
    QString current;
    QString configId;
    bool fromConfig = false;
    if (result.contains(u"configOptions"_s)) {
        fromConfig = collectModelConfig(result.value(u"configOptions"_s).toArray(), &models, &current, &configId);
    }

    bool legacy = false;
    if (!fromConfig && result.contains(u"models"_s)) {
        const QJsonValue modelsValue = result.value(u"models"_s);
        if (modelsValue.isObject()) {
            const QJsonObject state = modelsValue.toObject();
            current = state.value(u"currentModelId"_s).toString();
            const QJsonArray available = state.value(u"availableModels"_s).toArray();
            for (const QJsonValue &entry : available) {
                appendModelEntry(&models, entry);
            }
            legacy = !models.isEmpty() || !current.isEmpty();
        } else if (modelsValue.isArray()) {
            for (const QJsonValue &entry : modelsValue.toArray()) {
                appendModelEntry(&models, entry);
            }
            legacy = !models.isEmpty();
        }
    } else if (result.contains(u"availableModels"_s)) {
        for (const QJsonValue &entry : result.value(u"availableModels"_s).toArray()) {
            appendModelEntry(&models, entry);
        }
        if (current.isEmpty()) {
            current = result.value(u"currentModelId"_s).toString();
        }
        legacy = !models.isEmpty();
    }

    if (fromConfig) {
        m_modelConfigId = configId.isEmpty() ? u"model"_s : configId;
        m_legacySetModel = false;
    } else if (legacy) {
        m_modelConfigId.clear();
        m_legacySetModel = true;
    }
    if (!current.isEmpty()) {
        m_currentModelId = current;
    }
    // A current_model_update carries only the id. Don't drop the list we already have.
    if (fromConfig || legacy) {
        m_agentModels = models;
    }

    if (applyPreferred) {
        const QString preferred = m_settings.acpModel.trimmed();
        if (!preferred.isEmpty() && preferred != m_currentModelId
            && (m_agentModels.isEmpty() || m_agentModels.contains(preferred))) {
            setModel(preferred);
        } else if (preferred.isEmpty() && !m_currentModelId.isEmpty()) {
            m_settings.acpModel = m_currentModelId;
        }
    }

    Q_EMIT modelsReceived(m_agentModels);
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
    const QString command = acpEffectiveCommand(settings);
    const QString agentId = settings.acpAgentId;
    const QString model = settings.acpModel.trimmed();

    // Handle arguments for different ACP agents
    if (acpAgentIsGrok(settings)) {
        // Grok Build: grok agent stdio --model <model> --always-approve
        if (args.isEmpty()) {
            args = QStringList{u"agent"_s, u"stdio"_s};
        }
        int transport = args.lastIndexOf(u"stdio"_s);
        if (transport < 0) {
            transport = args.size();
        }

        QStringList flags;
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

    // Handle model arguments for other ACP agents that support --model
    if (!model.isEmpty()) {
        if (agentId == u"claude-acp"_s || command.contains(u"claude-agent-acp"_s) || raw.contains(u"claude-agent-acp"_s)) {
            // Claude Agent: npx -y @agentclientprotocol/claude-agent-acp --model <model>
            if (!args.contains(u"--model"_s) && !args.contains(u"-m"_s)) {
                args << u"--model"_s << model;
            }
        }
        else if (agentId == u"gemini"_s || command == u"gemini"_s) {
            // Gemini CLI: gemini --acp --model <model>
            if (!args.contains(u"--model"_s) && !args.contains(u"-m"_s)) {
                // Insert --model before --acp if it exists, otherwise at the end
                int acpIndex = args.indexOf(u"--acp"_s);
                if (acpIndex >= 0) {
                    args.insert(acpIndex, u"--model"_s);
                    args.insert(acpIndex + 1, model);
                } else {
                    args << u"--model"_s << model;
                }
            }
        }
        // Add more agent-specific model handling here as needed
    }

    return args;
}

void AcpClient::start()
{
    if (m_state == State::Starting || m_state == State::Ready) {
        return;
    }
    spawnProcess();
}

void AcpClient::cancelPendingPermission()
{
    if (!hasPendingPermission()) {
        return;
    }
    QJsonObject outcome;
    outcome.insert(u"outcome"_s, u"cancelled"_s);
    QJsonObject result;
    result.insert(u"outcome"_s, outcome);
    replyResult(m_permissionRpcId, result);
    m_permissionRpcId = QJsonValue();
    m_permissionOptions = QJsonArray();
    m_permissionRequest = {};
}

void AcpClient::finishLiveTools(const QString &message)
{
    for (auto it = m_liveTools.begin(); it != m_liveTools.end(); ++it) {
        ToolResult result;
        result.toolCallId = it.key();
        result.name = it.value().toolName;
        result.ok = false;
        result.cancelled = true;
        result.output = message;
        Q_EMIT toolFinished(result);
    }
    m_liveTools.clear();
    m_toolStreamed.clear();
}

void AcpClient::cancel()
{
    cancelPendingPermission();
    if (m_promptId != 0 && !m_sessionId.isEmpty()) {
        QJsonObject params;
        params.insert(u"sessionId"_s, m_sessionId);
        sendNotification(u"session/cancel"_s, params);
        finishLiveTools(u"Cancelled."_s);
        // An agent that ignores session/cancel leaves the prompt pending
        // forever, which keeps isBusy() true and makes every later prompt be
        // queued instead of sent. Give it a bounded grace period to answer and
        // then close the turn here.
        const int promptId = m_promptId;
        if (m_cancelGraceTimer) {
            m_cancelGraceTimer->stop();
            delete m_cancelGraceTimer;
        }
        auto *grace = new QTimer(this);
        grace->setSingleShot(true);
        grace->setInterval(qBound(5000, m_settings.toolCallTimeoutMs, 60000));
        connect(grace, &QTimer::timeout, this, [this, promptId] {
            if (m_promptId != promptId) {
                return;
            }
            // An agent that ignored session/cancel can still be streaming tool
            // updates into this window; those cards must be closed too, or they
            // spin in the transcript for the rest of the session.
            finishLiveTools(u"The turn was stopped before this finished."_s);
            m_pending.remove(promptId);
            m_promptId = 0;
            finishPrompt(QString(), u"The agent did not answer the cancellation in time."_s);
        });
        grace->start();
        m_cancelGraceTimer = grace;
    }
}

void AcpClient::stop()
{
    m_shuttingDown = true;
    if (m_startupTimer) {
        m_startupTimer->stop();
    }
    if (m_cancelGraceTimer) {
        m_cancelGraceTimer->stop();
        delete m_cancelGraceTimer;
        m_cancelGraceTimer = nullptr;
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
    m_discardingStdoutFrame = false;
    m_sessionId.clear();
    m_promptId = 0;
    m_permissionRpcId = QJsonValue();
    m_queued.clear();
    m_agentModels.clear();
    m_currentModelId.clear();
    m_modelConfigId.clear();
    m_legacySetModel = false;
    m_modelSetInFlight = false;
    if (m_state != State::Stopped) {
        setState(State::Stopped, u"Disconnected"_s);
    }
}

void AcpClient::resetSession()
{
    // Any permission question posed in the outgoing session must be closed
    // before the session id goes away.
    cancelPendingPermission();
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
    // If session was reset but process is still ready, create a new session
    if (m_sessionId.isEmpty()) {
        openSession();
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
        // No advertised option carried a matching kind. Picking the first one
        // could transmit a reject id for an approval, which silently inverts
        // the user's decision; cancelling keeps the tool call from running
        // without telling the agent it was refused.
        outcome.remove(u"optionId"_s);
        outcome.insert(u"outcome"_s, u"cancelled"_s);
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
    m_discardingStdoutFrame = false;
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

    // Retrying replaces the timer. The previous one must go first: it is still
    // parented to this and still counting, and a retry started inside the first
    // attempt's window would then be failed by that stale 30 s deadline.
    if (m_startupTimer) {
        m_startupTimer->stop();
        delete m_startupTimer;
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
    // Kate-specific client methods agents may call. Advertised so agents can
    // discover them the way they discover fs/terminal support instead of
    // having to hard-code Kate's extensions.
    QJsonObject kateai;
    kateai.insert(u"queryProjectGraph"_s, true);
    kateai.insert(u"writePlanFile"_s, true);
    kateai.insert(u"suggest"_s, true);
    capabilities.insert(u"kateai"_s, kateai);
    // Advertise support for session config options (used for model selection)
    QJsonObject sessionCapabilities;
    QJsonObject configOptionsCaps;
    configOptionsCaps.insert(u"boolean"_s, QJsonObject());
    sessionCapabilities.insert(u"configOptions"_s, configOptionsCaps);
    capabilities.insert(u"session"_s, sessionCapabilities);

    QJsonObject clientInfo;
    clientInfo.insert(u"name"_s, u"kateai"_s);
    clientInfo.insert(u"title"_s, u"Kate AI"_s);
    clientInfo.insert(u"version"_s, u"0.1.0"_s);

    QJsonObject params;
    params.insert(u"protocolVersion"_s, kAcpProtocolVersion);
    params.insert(u"clientCapabilities"_s, capabilities);
    params.insert(u"clientInfo"_s, clientInfo);

    sendRequest(u"initialize"_s, params, [this](const QJsonObject &result, const QString &error) {
        if (m_shuttingDown) {
            return;
        }
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

void AcpClient::fetchModels()
{
    if (m_state == State::Ready) {
        Q_EMIT modelsReceived(m_agentModels);
        return;
    }
    if (m_state == State::Starting) {
        return;
    }
    start();
}

bool AcpClient::setModel(const QString &modelId)
{
    const QString id = modelId.trimmed();
    if (id.isEmpty() || id == m_currentModelId) {
        return true;
    }
    if (m_sessionId.isEmpty()) {
        return false;
    }
    if (!m_modelConfigId.isEmpty()) {
        sendSetConfigOption(id);
        return true;
    }
    if (m_legacySetModel) {
        sendSetSessionModel(id);
        return true;
    }
    return false;
}

void AcpClient::sendSetConfigOption(const QString &modelId)
{
    m_modelSetInFlight = true;
    QJsonObject params;
    params.insert(u"sessionId"_s, m_sessionId);
    params.insert(u"configId"_s, m_modelConfigId.isEmpty() ? u"model"_s : m_modelConfigId);
    // value_id variant: `value` is the option id. `type` is omitted.
    params.insert(u"value"_s, modelId);
    sendRequest(u"session/set_config_option"_s, params, [this, modelId](const QJsonObject &result, const QString &error) {
        m_modelSetInFlight = false;
        if (!error.isEmpty()) {
            Q_EMIT logMessage(u"Failed to set model: %1"_s.arg(error));
            if (m_legacySetModel) {
                sendSetSessionModel(modelId);
            }
            return;
        }
        m_settings.acpModel = modelId;
        if (result.contains(u"configOptions"_s) || result.contains(u"models"_s)) {
            ingestSessionModels(result, false);
        } else {
            m_currentModelId = modelId;
            Q_EMIT modelsReceived(m_agentModels);
        }
    });
}

void AcpClient::sendSetSessionModel(const QString &modelId)
{
    m_modelSetInFlight = true;
    QJsonObject params;
    params.insert(u"sessionId"_s, m_sessionId);
    params.insert(u"modelId"_s, modelId);
    sendRequest(u"session/set_model"_s, params, [this, modelId](const QJsonObject &result, const QString &error) {
        m_modelSetInFlight = false;
        if (!error.isEmpty()) {
            Q_EMIT logMessage(u"Failed to set model: %1"_s.arg(error));
            return;
        }
        m_settings.acpModel = modelId;
        if (result.contains(u"models"_s) || result.contains(u"configOptions"_s)) {
            ingestSessionModels(result, false);
        } else {
            m_currentModelId = modelId;
            Q_EMIT modelsReceived(m_agentModels);
        }
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
    // Reached only with a key configured (the no-key path returned above), so
    // the preference must depend on the agent alone. Testing the key here as
    // well made the condition always false and left the first advertised method
    // selected, which is chat-gpt for codex and fails authentication.
    const QString preferredMethod = codexAgent ? u"chat-gpt"_s : QString();
    QJsonObject selectedMethod;
    for (const QJsonValue &value : authMethods) {
        const QJsonObject method = value.toObject();
        QString id = method.value(u"id"_s).toString();
        if (id.isEmpty()) id = method.value(u"methodId"_s).toString();
        // An agent advertising no id at all still has to authenticate with
        // something, and the request carries a methodId.
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
        if (m_shuttingDown) {
            return;
        }
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
        if (m_shuttingDown) {
            return;
        }
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
        ingestSessionModels(result, true);
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
    sendRequest(method, params, [this](const QJsonObject &result, const QString &error) {
        if (m_shuttingDown) {
            return;
        }
        if (!error.isEmpty()) {
            m_resumeSessionId.clear();
            sendSessionNew();
            return;
        }
        m_sessionId = m_resumeSessionId;
        ingestSessionModels(result, true);
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
    // Tools may still be open if the previous turn was cancelled by force (an
    // agent ignoring session/cancel keeps streaming updates until the grace
    // window closes). Close their cards rather than dropping them silently, or
    // they spin in the transcript for the rest of the session.
    finishLiveTools(u"The previous turn was stopped before this finished."_s);

    QJsonObject params;
    params.insert(u"sessionId"_s, m_sessionId);
    params.insert(u"prompt"_s, promptBlocks(text));
    m_promptId = sendRequest(u"session/prompt"_s, params, [this](const QJsonObject &result, const QString &error) {
        if (m_shuttingDown) {
            return;
        }
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
    const QString agentId = m_settings.acpAgentId;
    const QString command = acpEffectiveCommand(m_settings);

    // Grok Build reads yoloMode / autoMode / rules from session _meta.
    if (acpAgentIsGrok(m_settings)) {
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
    }

    // Add model information for agents that support it in _meta
    const QString model = m_settings.acpModel.trimmed();
    if (!model.isEmpty()) {
        // Add other agents that support model in _meta here
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
    constexpr qsizetype kMaxFrameBytes = 16 * 1024 * 1024;
    const QByteArray output = m_process->readAllStandardOutput();
    for (qsizetype offset = 0; offset < output.size();) {
        const qsizetype chunkSize = qMin<qsizetype>(64 * 1024, output.size() - offset);
        const QByteArray chunk = output.mid(offset, chunkSize);
        offset += chunkSize;
        if (m_discardingStdoutFrame) {
            const qsizetype newline = chunk.indexOf('\n');
            if (newline < 0) {
                continue;
            }
            m_discardingStdoutFrame = false;
            m_stdoutBuffer.append(chunk.mid(newline + 1));
        } else {
            m_stdoutBuffer.append(chunk);
        }
        handleBuffer();
        if (m_stdoutBuffer.size() > kMaxFrameBytes) {
            m_stdoutBuffer.clear();
            m_discardingStdoutFrame = true;
        }
    }
}

void AcpClient::handleBuffer()
{
    constexpr qsizetype kMaxFrameBytes = 16 * 1024 * 1024;
    int newline = m_stdoutBuffer.indexOf('\n');
    while (newline >= 0) {
        if (newline > kMaxFrameBytes) {
            m_stdoutBuffer.remove(0, newline + 1);
            newline = m_stdoutBuffer.indexOf('\n');
            continue;
        }
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
    // Our request ids are always integers, but a reply that quotes the id as a
    // string ("1000") would otherwise map to 0 here and be dropped, leaving the
    // request to end only on its timeout -- and session/prompt has none, so the
    // turn would hang. Accept a string only when it is wholly numeric, so a
    // non-numeric id can never collide with a real request.
    const QJsonValue idValue = message.value(u"id"_s);
    int id = 0;
    if (idValue.isDouble()) {
        id = idValue.toInt();
    } else if (idValue.isString()) {
        id = idValue.toString().toInt();
    } else {
        return;
    }
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
    if (method == u"write_plan_file"_s) {
        handleWritePlanFile(id, params);
        return;
    }
    if (method == u"suggest"_s) {
        handleSuggest(id, params);
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
    if (method == u"query_project_graph"_s) {
        handleQueryProjectGraph(id, params);
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
    } else if (method == u"config_option_update"_s) {
        ingestSessionModels(params, false);
    }
}

void AcpClient::handleSessionUpdate(const QJsonObject &update)
{
    const QString kind = update.value(u"sessionUpdate"_s).toString();
    if (kind == u"config_option_update"_s || kind == u"available_models_update"_s || kind == u"current_model_update"_s) {
        ingestSessionModels(update, false);
        return;
    }
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
                result.output = toolContentText(update.value(u"rawOutput"_s));
            }
            if (result.output.isEmpty()) {
                result.output = toolContentText(update.value(u"output"_s));
            }
            if (result.output.isEmpty()) {
                result.output = toolContentText(update.value(u"result"_s));
            }
            if (result.output.isEmpty()) {
                // Some ACP agents stream command output while the tool runs,
                // then send a terminal status update with no content at all.
                // Keep that last complete stream as the final card output.
                result.output = m_toolStreamed.value(toolCallId);
            }
            m_liveTools.remove(toolCallId);
            m_toolStreamed.remove(toolCallId);
            Q_EMIT toolFinished(result);
            return;
        }
        // Still running: the agent is telling us what the tool has done so far.
        // The content array is the whole of what the agent knows, so the latest
        // text replaces whatever was streamed before, exactly like bashProgress
        // does for a native command.
        const QString streamed = toolContentText(update.value(u"content"_s));
        if (streamed.isEmpty()) {
            // Some agents only refresh the title mid-flight ("Running tests…").
            // Keep the card's summary current so the header never lies, but
            // there is nothing to stream into the body yet.
            const QString title = update.value(u"title"_s).toString();
            if (!title.isEmpty() && title != existing.summary) {
                existing.summary = title;
                m_liveTools.insert(toolCallId, existing);
            }
            return;
        }
        // An agent that repeats the same content word for word must not force a
        // re-render of the whole stream on every heartbeat.
        if (m_toolStreamed.value(toolCallId) == streamed) {
            return;
        }
        m_toolStreamed.insert(toolCallId, streamed);
        Q_EMIT toolProgress(toolCallId, streamed);
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
    const QString kind = toolCall.value(u"kind"_s).toString();
    request.risk = riskForKind(kind, request.toolName);
    // Every agent names its own tools, so the edit tracker cannot rely on
    // tool names here. The ACP kind is the protocol-level statement of what
    // the call does, and a diff content block proves the agent edited text.
    request.mutatesFiles = kind == u"edit"_s || kind == u"write"_s || kind == u"delete"_s;
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
            request.mutatesFiles = true;
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
    // Only one permission can be shown at a time. If the agent pipelines a
    // second request while the first is still waiting, the first has to be
    // answered (as cancelled) before it is overwritten -- otherwise its id is
    // never replied to and the agent blocks on it forever.
    cancelPendingPermission();
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
    for (const QJsonValue &value : m_permissionOptions) {
        const QJsonObject option = value.toObject();
        const QString kind = option.value(u"kind"_s).toString();
        const QString optionId = option.value(u"optionId"_s).toString();
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
    // Last resort: an option whose kind points the same way as the decision.
    // An agent that advertises only allow_always/reject_always has no
    // allow_once/reject_once, and cancelling then would drop a tool the user
    // explicitly approved. Matching by direction still never inverts the
    // decision, unlike echoing an arbitrary option id.
    const QString direction = decision == PermissionDecision::Deny ? u"reject"_s : u"allow"_s;
    for (const QJsonValue &value : m_permissionOptions) {
        const QJsonObject option = value.toObject();
        const QString optionId = option.value(u"optionId"_s).toString();
        if (!optionId.isEmpty() && option.value(u"kind"_s).toString().startsWith(direction)) {
            return optionId;
        }
    }
    // Nothing on offer points the right way. Guessing an option id for either
    // decision can invert the user's choice: "first" may be an allow option, so
    // approving could refuse and refusing could execute. Return nothing and let
    // resolvePermission() cancel, which keeps the tool call from running
    // without telling the agent it was refused.
    return QString();
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

// Kate-specific client method, mirroring the native write_plan_file tool: the
// agent hands over the plan as markdown and Kate files it under its own
// per-workspace temp path, returned in the result so the agent can tell the
// user where to follow along. No sandbox check and no permission prompt — the
// destination is chosen by Kate, not by the agent.
void AcpClient::handleWritePlanFile(const QJsonValue &id, const QJsonObject &params)
{
    const QString content = params.value(u"content"_s).toString();
    if (content.trimmed().isEmpty()) {
        replyError(id, kInvalidParams, u"content must not be empty: pass the full plan as markdown."_s);
        return;
    }
    if (!m_bridge) {
        replyError(id, kInternalError, u"No document bridge is available."_s);
        return;
    }
    const QString path = planFilePath(m_workspace);
    QString error;
    if (!m_bridge->writeDocument(path, content, &error)) {
        replyError(id, kServerError, error.isEmpty() ? u"Could not write the plan file."_s : error);
        return;
    }
    QJsonObject result;
    result.insert(u"path"_s, path);
    replyResult(id, result);
}

// Kate-specific client method, mirroring the native suggest tool: the agent
// hands over advice instead of making a change, and Kate posts it as a card
// the user can accept or dismiss. Nothing touches the workspace, so there is
// no sandbox check and no permission prompt.
void AcpClient::handleSuggest(const QJsonValue &id, const QJsonObject &params)
{
    const QString text = params.value(u"suggestion"_s).toString().trimmed();
    if (text.isEmpty()) {
        replyError(id, kInvalidParams, u"suggestion must not be empty: describe what you are suggesting."_s);
        return;
    }

    Suggestion suggestion;
    suggestion.text = text;
    suggestion.path = params.value(u"path"_s).toString().trimmed();
    const QJsonValue lineValue = params.value(u"line"_s);
    if (lineValue.isDouble()) {
        suggestion.line = lineValue.toInt();
        if (suggestion.line < 1) {
            replyError(id, kInvalidParams, u"line must be a 1-based line number, or leave it out."_s);
            return;
        }
    } else if (!lineValue.isUndefined() && !lineValue.isNull()) {
        replyError(id, kInvalidParams, u"line must be a 1-based line number, or leave it out."_s);
        return;
    }

    Q_EMIT suggestionPosted(suggestion);
    replyResult(id, QJsonObject());
}

void AcpClient::handleQueryProjectGraph(const QJsonValue &id, const QJsonObject &params)
{
    if (!m_projectGraph || m_projectGraph->getNodeCount() == 0) {
        replyError(id, kServerError, u"Project graph is not available yet."_s);
        return;
    }

    const QString queryType = params.value(u"query_type"_s).toString().trimmed();
    const QString nodeId = params.value(u"node_id"_s).toString();
    const QString relationship = params.value(u"relationship"_s).toString();
    const QString sourceId = params.value(u"source_id"_s).toString();
    const QString targetId = params.value(u"target_id"_s).toString();

    auto formatNode = [](const GraphNode *node) {
        if (!node) {
            return QString();
        }
        return u"%1 [%2] %3 loc=%4"_s.arg(node->id, node->type, node->path.isEmpty() ? node->name : node->path)
            .arg(node->linesOfCode);
    };

    QStringList lines;
    const QString type = queryType.isEmpty() ? u"summary"_s : queryType;

    if (type == u"summary"_s) {
        lines.append(u"nodes: %1"_s.arg(m_projectGraph->getNodeCount()));
        lines.append(u"edges: %1"_s.arg(m_projectGraph->getEdgeCount()));
        QMap<QString, int> typeCount;
        for (GraphNode *node : m_projectGraph->getAllNodes()) {
            if (node) {
                typeCount[node->type]++;
            }
        }
        for (auto it = typeCount.begin(); it != typeCount.end(); ++it) {
            lines.append(u"  %1: %2"_s.arg(it.key()).arg(it.value()));
        }
    } else if (type == u"nodes"_s) {
        int shown = 0;
        for (GraphNode *node : m_projectGraph->getAllNodes()) {
            if (!node) {
                continue;
            }
            lines.append(formatNode(node));
            if (++shown >= 150) {
                lines.append(u"... truncated"_s);
                break;
            }
        }
    } else if (type == u"edges"_s) {
        int shown = 0;
        for (auto it = m_projectGraph->getEdges().begin(); it != m_projectGraph->getEdges().end(); ++it) {
            const GraphEdge *edge = it.value();
            if (!edge) {
                continue;
            }
            if (!relationship.isEmpty() && edge->relationship != relationship) {
                continue;
            }
            lines.append(u"%1 -> %2 (%3)"_s.arg(edge->sourceId, edge->targetId, edge->relationship));
            if (++shown >= 150) {
                lines.append(u"... truncated"_s);
                break;
            }
        }
    } else if (type == u"dependencies"_s || type == u"dependents"_s || type == u"find_related"_s) {
        GraphNode *node = nullptr;
        if (!nodeId.isEmpty()) {
            node = m_projectGraph->getNode(nodeId);
            if (!node) {
                // Try to find by name
                for (GraphNode *n : m_projectGraph->getAllNodes()) {
                    if (n && (n->path == nodeId || n->name == nodeId)) {
                        node = n;
                        break;
                    }
                }
            }
        }
        if (!node) {
            replyError(id, kInvalidParams, u"Unknown node: %1"_s.arg(nodeId));
            return;
        }
        lines.append(formatNode(node));
        if (type == u"dependencies"_s) {
            for (const QString &dep : node->dependencies) {
                lines.append(u"depends on: %1"_s.arg(dep));
            }
        } else if (type == u"dependents"_s) {
            for (const QString &dep : node->dependents) {
                lines.append(u"used by: %1"_s.arg(dep));
            }
        } else {
            for (GraphNode *related : m_projectGraph->findRelatedNodes(node->id, 2)) {
                if (related && related != node) {
                    lines.append(u"related: %1"_s.arg(formatNode(related)));
                }
            }
        }
    } else if (type == u"find_path"_s || type == u"dependency_chain"_s) {
        GraphNode *source = nullptr;
        GraphNode *target = nullptr;
        if (!sourceId.isEmpty()) {
            source = m_projectGraph->getNode(sourceId);
        }
        if (!targetId.isEmpty()) {
            target = m_projectGraph->getNode(targetId);
        }
        if (!source || !target) {
            replyError(id, kInvalidParams, u"Need valid source_id and target_id."_s);
            return;
        }
        if (type == u"find_path"_s) {
            const QList<QString> path = m_projectGraph->findImportPaths(source->id, target->id);
            if (path.isEmpty()) {
                lines.append(u"No import path found."_s);
            } else {
                lines.append(path.join(u" -> "_s));
            }
        } else {
            const QList<GraphNode *> chain = m_projectGraph->getDependencyChain(source->id, target->id);
            if (chain.isEmpty()) {
                lines.append(u"No dependency chain found."_s);
            } else {
                for (GraphNode *node : chain) {
                    lines.append(formatNode(node));
                }
            }
        }
    } else {
        replyError(id, kInvalidParams, u"Unknown query_type. Use summary, nodes, edges, dependencies, dependents, find_related, find_path, or dependency_chain."_s);
        return;
    }

    QJsonObject result;
    result.insert(u"content"_s, lines.join(u'\n'));
    replyResult(id, result);
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
    return Sandbox(m_workspace, m_settings.sandbox, m_settings.extraDenyGlobs, m_settings.allowReadsOutsideWorkspace);
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
    // A permission request in flight must still be answered, or the agent waits
    // on it for as long as the process lives.
    cancelPendingPermission();
    // Tool calls still open when the turn dies would otherwise sit in the
    // transcript as cards that never finish.
    finishLiveTools(error);
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
    // The turn is over, so a pending cancellation grace window must not fire.
    if (m_cancelGraceTimer) {
        m_cancelGraceTimer->stop();
        delete m_cancelGraceTimer;
    }
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
