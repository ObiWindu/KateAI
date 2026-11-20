/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpclient.h"
#include "acpinstall.h"
#include "processenv.h"
#include "tools.h"
#include "graph/projectgraph.h"

#include <QDir>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <limits>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

static const QString JsonRpcVersion = QStringLiteral("2.0");
static const int kAcpProtocolVersion = 1;
static const int kMethodNotFound = -32601;
static const int kInvalidParams = -32602;
static const int kInternalError = -32603;
static const int kServerError = -32000;
// The largest protocol frame the client accepts. A frame above it is dropped,
// and the turn is failed rather than left waiting for an answer that cannot
// arrive.
static constexpr qsizetype MaxFrameBytes = 16 * 1024 * 1024;

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
    if (type == u"resource_link"_s) {
        const QString title = obj.value(u"title"_s).toString();
        const QString name = obj.value(u"name"_s).toString();
        const QString uri = obj.value(u"uri"_s).toString();
        const QString label = !title.isEmpty() ? title : name;
        if (!label.isEmpty() && !uri.isEmpty()) {
            return u"%1 (%2)"_s.arg(label, uri);
        }
        return !label.isEmpty() ? label : uri;
    }
    if (type == u"image"_s) {
        const QString mime = obj.value(u"mimeType"_s).toString();
        return mime.isEmpty() ? u"[Image]"_s : u"[Image: %1]"_s.arg(mime);
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
        } else if (type == u"terminal"_s) {
            // A terminal the agent embedded streams into the card on its own
            // (see linkToolTerminals). Its JSON reference is not output.
        } else if (type == u"text"_s) {
            parts.append(obj.value(u"text"_s).toString());
        } else if (type == u"resource_link"_s || type == u"image"_s || type == u"resource"_s) {
            const QString text = contentBlockText(item);
            if (!text.isEmpty()) {
                parts.append(text);
            }
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

// Builds the final ToolResult for a tool_call/tool_call_update that already
// carries a terminal status. The tool_call branch used to ignore status, so a
// fast call that completed in one message spun forever and leaked into the
// next turn's live-tool sweep.
static bool isTerminalToolStatus(const QString &status)
{
    return status == u"completed"_s || status == u"failed"_s
        || status == u"cancelled"_s || status == u"canceled"_s
        || status == u"error"_s;
}

static ToolResult completedToolResult(const QString &toolCallId, const QString &toolName,
                                      const QJsonObject &update, const QString &streamed)
{
    ToolResult result;
    result.toolCallId = toolCallId;
    result.name = toolName;
    const QString status = update.value(u"status"_s).toString();
    result.ok = status == u"completed"_s;
    result.cancelled = status == u"cancelled"_s || status == u"canceled"_s;
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
    if (result.output.isEmpty() && update.contains(u"error"_s)) {
        result.output = toolContentText(update.value(u"error"_s));
    }
    if (result.cancelled && result.output.isEmpty()) {
        result.output = u"Tool cancelled."_s;
    } else if ((status == u"failed"_s || status == u"error"_s) && result.output.isEmpty()) {
        result.output = u"Tool failed."_s;
    }
    if (result.output.isEmpty()) {
        result.output = streamed;
    }
    return result;
}

// Streamed tool content is retained for the final card when an agent sends a
// terminal status without content. Bound it like the command buffers: a very
// chatty tool must not pin an unbounded string per live call.
static QString capToolStream(QString text)
{
    constexpr int kMaxStreamChars = 1024 * 1024;
    if (text.size() <= kMaxStreamChars) {
        return text;
    }
    return u"… (earlier output truncated) …\n"_s + text.right(kMaxStreamChars);
}

static ToolRisk riskForKind(const QString &kind, const QString &name)
{
    const QString canonical = canonicalToolName(name);
    if (kind == u"read"_s || kind == u"search"_s || kind == u"fetch"_s || kind == u"think"_s) {
        return ToolRisk::Read;
    }
    if (kind == u"execute"_s || name == u"bash"_s || canonical == u"bash"_s) {
        return ToolRisk::Execute;
    }
    if (kind == u"edit"_s || kind == u"write"_s || kind == u"delete"_s || kind == u"move"_s) {
        return ToolRisk::Write;
    }
    // Well-known read-only or UI-only tools when an agent omits kind or sends "other".
    if (canonical == u"read_file"_s || canonical == u"list_dir"_s || canonical == u"grep"_s
        || canonical == u"glob"_s || canonical == u"web_search"_s || canonical == u"web_fetch"_s
        || canonical == u"query_project_graph"_s || canonical == u"write_plan_file"_s
        || canonical == u"suggest"_s || canonical == u"question"_s || canonical == u"todo_write"_s
        || canonical == u"todo_read"_s || canonical == u"think"_s || canonical == u"exit_plan_mode"_s
        || canonical.startsWith(u"lsp_"_s)
        || name == u"command_status"_s || name == u"read_terminal"_s
        || name == u"task_boundary"_s || name == u"notify_user"_s) {
        return ToolRisk::Read;
    }
    // An unrecognized kind is not evidence the tool is safe: in AcceptEdits
    // mode a Write risk is auto-approved, which would let a mislabeled
    // command-running tool skip the prompt the way the native bash tool never
    // does. Execute makes the policy ask.
    return ToolRisk::Execute;
}

// Kate's own client methods, which agents call by name. The protocol reserves
// underscore names for extensions, so the advertised form is _kateai/<method>.
// The bare and camelCase names agents were given before still reach the same
// handlers.
static QString kateClientMethod(const QString &method)
{
    static const QHash<QString, QString> aliases = {
        {u"queryProjectGraph"_s, u"query_project_graph"_s},
        {u"writePlanFile"_s, u"write_plan_file"_s},
        {u"lspDiagnostics"_s, u"lsp_diagnostics"_s},
        {u"lspSymbols"_s, u"lsp_symbols"_s},
        {u"lspHover"_s, u"lsp_hover"_s},
        {u"lspDefinition"_s, u"lsp_definition"_s},
        {u"lspReferences"_s, u"lsp_references"_s},
        {u"lspCodeActions"_s, u"lsp_code_actions"_s},
        {u"askUser"_s, u"ask_user"_s},
        {u"askUserQuestion"_s, u"ask_user"_s},
        {u"ask_question"_s, u"ask_user"_s},
        {u"suggestion"_s, u"suggest"_s},
        {u"postSuggestion"_s, u"suggest"_s},
        {u"todoWrite"_s, u"todo_write"_s},
        {u"todoRead"_s, u"todo_read"_s},
        {u"listDir"_s, u"list_dir"_s},
        {u"listDirectory"_s, u"list_dir"_s},
        {u"editFile"_s, u"edit_file"_s},
        {u"multiEditFile"_s, u"multi_edit_file"_s},
        {u"deleteFile"_s, u"delete_file"_s},
        {u"applyPatch"_s, u"apply_patch"_s},
        {u"exitPlanMode"_s, u"exit_plan_mode"_s},
        {u"viewFile"_s, u"read_file"_s},
        {u"view_file"_s, u"read_file"_s},
        {u"readFile"_s, u"read_file"_s},
        {u"writeFile"_s, u"write_file"_s},
        {u"createFile"_s, u"write_file"_s},
        {u"client_view_file"_s, u"read_file"_s},
        {u"client_create_file"_s, u"write_file"_s},
        {u"client_edit_file"_s, u"edit_file"_s},
    };
    QString name = method;
    if (name.startsWith(u"_kateai/"_s)) {
        name = name.mid(8);
    } else if (name.startsWith(u"kateai/"_s)) {
        name = name.mid(7);
    } else if (name.startsWith(u"com.kateai/"_s)) {
        name = name.mid(11);
    }
    return aliases.value(name, name);
}

// JSON numbers arrive as numbers, but models and extensions routinely send
// integers as strings ("12"). QJsonValue::toInt() silently turns those into
// the fallback, which moved LSP queries to line 1. Accept both forms.
static int jsonIntValue(const QJsonValue &value, int fallback)
{
    if (value.isDouble()) {
        return value.toInt();
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().trimmed().toInt(&ok);
        if (ok) {
            return parsed;
        }
    }
    return fallback;
}

static QString firstStringField(const QJsonObject &object, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isString()) {
            const QString text = value.toString().trimmed();
            if (!text.isEmpty()) {
                return text;
            }
        }
    }
    return {};
}

static bool boolishValue(const QJsonValue &value, bool fallback)
{
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isDouble()) {
        return value.toInt() != 0;
    }
    if (value.isString()) {
        const QString text = value.toString().trimmed().toLower();
        if (text == u"true"_s || text == u"1"_s || text == u"yes"_s) {
            return true;
        }
        if (text == u"false"_s || text == u"0"_s || text == u"no"_s) {
            return false;
        }
    }
    return fallback;
}

static QStringList optionLabels(const QJsonValue &value)
{
    QStringList labels;
    const auto add = [&labels](const QJsonValue &item) {
        QString label;
        if (item.isString()) {
            label = item.toString().trimmed();
        } else if (item.isObject()) {
            const QJsonObject option = item.toObject();
            label = firstStringField(option, {u"label"_s, u"title"_s, u"name"_s, u"text"_s, u"value"_s});
            if (label.isEmpty()) {
                label = firstStringField(option, {u"description"_s});
            }
        }
        if (!label.isEmpty() && !labels.contains(label)) {
            labels.append(label);
        }
    };

    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) {
            add(item);
        }
    } else if (!value.isUndefined() && !value.isNull()) {
        add(value);
    }
    return labels;
}

// Normalizes the many question-tool shapes agents use into the fields the
// QuestionWidget reads. Claude/OpenCode send {questions: [{question, options:
// [{label}]}]}; simpler agents send {question, options: ["a"]}; others use
// prompt/message. The normalized form is also written back to the details JSON
// so history restoration and the suggestion/question card parsers see it.
static void normalizeQuestionMetadata(PermissionRequest &request, QJsonObject *args, const QString &title)
{
    QJsonObject source = *args;
    const QJsonValue nested = args->value(u"questions"_s);
    if (nested.isArray() && !nested.toArray().isEmpty()) {
        const QJsonObject first = nested.toArray().first().toObject();
        if (!first.isEmpty()) {
            source = first;
        }
    }

    QString text = questionTextFromToolArguments(*args);
    if (text.isEmpty()) {
        text = firstStringField(*args, {u"question"_s, u"prompt"_s, u"message"_s, u"text"_s, u"header"_s});
    }
    if (text.isEmpty() && !title.isEmpty() && title != request.originalToolName) {
        text = title;
    }
    if (text.isEmpty() && request.summary != request.originalToolName && request.summary != request.toolName) {
        text = request.summary;
    }
    if (text.isEmpty()) {
        text = u"The agent is waiting for your answer."_s;
    }

    QStringList options = questionOptionsFromToolArguments(*args);
    if (options.isEmpty()) {
        options = optionLabels(source.value(u"options"_s));
    }
    if (options.isEmpty()) {
        options = optionLabels(args->value(u"options"_s));
    }

    const QJsonValue freeform = source.contains(u"allow_freeform"_s)
        ? source.value(u"allow_freeform"_s)
        : source.value(u"allowFreeform"_s);
    request.allowFreeform = questionAllowFreeformFromToolArguments(*args, boolishValue(freeform, true));
    // A card with neither options nor a text box cannot be answered.
    if (options.isEmpty()) {
        request.allowFreeform = true;
    }

    request.questionText = text;
    request.questionOptions = options;
    if (!text.isEmpty()) {
        request.summary = text.left(200);
    }

    args->insert(u"question"_s, text);
    if (!options.isEmpty()) {
        args->insert(u"options"_s, QJsonArray::fromStringList(options));
    }
    args->insert(u"allow_freeform"_s, request.allowFreeform);
}

static void normalizeSuggestionMetadata(PermissionRequest &request, QJsonObject *args, const QString &title)
{
    QString text = suggestionTextFromToolArguments(*args);
    if (text.isEmpty()) {
        text = firstStringField(*args, {u"suggestion"_s, u"message"_s, u"text"_s, u"description"_s,
                                        u"content"_s, u"advice"_s, u"recommendation"_s});
    }
    if (text.isEmpty() && !title.isEmpty() && title != request.originalToolName) {
        text = title;
    }
    if (text.isEmpty() && request.summary != request.originalToolName && request.summary != request.toolName) {
        text = request.summary;
    }
    if (text.isEmpty()) {
        return;
    }

    QString path = firstStringField(*args, {u"path"_s, u"file_path"_s, u"filePath"_s, u"file"_s,
                                            u"target"_s, u"TargetFile"_s, u"uri"_s});
    if (path.startsWith(u"file://"_s)) {
        path = QUrl(path).toLocalFile();
    }
    int line = jsonIntValue(args->value(u"line"_s), 0);
    if (line <= 0) {
        line = jsonIntValue(args->value(u"start_line"_s), 0);
    }
    if (line <= 0) {
        line = jsonIntValue(args->value(u"startLine"_s), 0);
    }

    args->insert(u"suggestion"_s, text);
    if (!path.isEmpty()) {
        args->insert(u"path"_s, path);
        if (request.path.isEmpty()) {
            request.path = path;
        }
    }
    if (line > 0) {
        args->insert(u"line"_s, line);
    }
    request.summary = text.left(200);
}

static QJsonObject envValueToObject(const QJsonValue &env)
{
    QJsonObject out;
    if (env.isObject()) {
        const QJsonObject obj = env.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (!it.key().isEmpty()) {
                out.insert(it.key(), it.value().toString());
            }
        }
        return out;
    }
    for (const QJsonValue &item : env.toArray()) {
        const QJsonObject row = item.toObject();
        const QString name = row.value(u"name"_s).toString();
        if (!name.isEmpty()) {
            out.insert(name, row.value(u"value"_s).toString());
        }
    }
    return out;
}

// Agents normally send a local absolute path, but some registries send a
// file:// URI or use "uri" / PascalCase keys instead of "path". Accept all so
// fs and terminal methods work across implementations.
static QString acpPathParam(const QJsonObject &params)
{
    QString path = firstStringField(
        params, {u"path"_s, u"uri"_s, u"file_path"_s, u"filePath"_s, u"file"_s, u"File"_s,
                 u"Path"_s, u"target_file"_s, u"targetFile"_s, u"TargetFile"_s,
                 u"absolute_path"_s, u"AbsolutePath"_s, u"target"_s,
                 u"DirectoryPath"_s, u"SearchPath"_s, u"SearchDirectory"_s});
    if (path.startsWith(u"file:"_s, Qt::CaseInsensitive)) {
        const QUrl url(path);
        if (url.isLocalFile()) {
            path = url.toLocalFile();
        }
    }
    return path;
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

// Desktop-launched editors often inherit a minimal PATH (/usr/bin:/bin), so
// npx/uvx/node installed by nvm, Volta, fnm, Homebrew or pip --user are
// invisible. The registry is mostly npx/uvx packages, so search the standard
// user tool locations and pass the same PATH to the child.
static QStringList agentToolDirectories()
{
    const QString home = QDir::homePath();
    QStringList dirs = {
        home + u"/.local/bin"_s,
        home + u"/.npm-global/bin"_s,
        home + u"/.volta/bin"_s,
        home + u"/.bun/bin"_s,
        home + u"/.cargo/bin"_s,
        home + u"/.local/share/pnpm"_s,
        home + u"/.asdf/shims"_s,
        home + u"/.fnm/aliases/default/bin"_s,
        u"/opt/homebrew/bin"_s,
        u"/usr/local/bin"_s,
    };
    // nvm keeps each Node version under ~/.nvm/versions/node/<version>/bin.
    QDir nvm(home + u"/.nvm/versions/node"_s);
    if (nvm.exists()) {
        const QStringList versions = nvm.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (auto it = versions.crbegin(); it != versions.crend(); ++it) {
            dirs.append(nvm.absoluteFilePath(*it) + u"/bin"_s);
        }
    }
    return dirs;
}

static QString findExecutableInToolDirs(const QString &command)
{
    if (command.isEmpty() || command.contains(u'/') || command.contains(u'\\')) {
        return {};
    }
#ifdef Q_OS_WIN
    const QStringList names = {command + u".exe"_s, command + u".cmd"_s, command + u".bat"_s, command};
#else
    const QStringList names = {command};
#endif
    for (const QString &dir : agentToolDirectories()) {
        for (const QString &name : names) {
            const QString candidate = QDir(dir).filePath(name);
            const QFileInfo info(candidate);
            if (info.exists() && info.isFile() && info.isExecutable()) {
                return info.absoluteFilePath();
            }
        }
    }
    return {};
}

static QStringList argsWithoutModelFlag(const QStringList &args)
{    QStringList out;
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
    const QString oldEffort = m_settings.acpEffort;
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
    // Clearing the model must restart: setModel("") is a no-op and the old
    // --model flag would otherwise keep applying.
    if (oldModel != m_settings.acpModel && m_settings.acpModel.trimmed().isEmpty()
        && !oldModel.trimmed().isEmpty()) {
        stop();
        return;
    }
    if (oldModel != m_settings.acpModel && !setModel(m_settings.acpModel)) {
        stop();
        return;
    }
    // An effort change rides the same live-session path. Unlike the model it
    // has no --flag fallback, so an agent without an effort selector simply
    // keeps its own level and the configured one is applied at the next start.
    if (oldEffort != m_settings.acpEffort) {
        setEffort(m_settings.acpEffort);
    }
}

void AcpClient::setWorkspace(const QString &workspace)
{
    if (m_workspace == workspace) {
        return;
    }
    m_workspace = workspace;
    // A saved session belongs to its cwd; resuming it under another workspace
    // makes agents reject the load or index the wrong tree.
    m_resumeSessionId.clear();
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

// Effort selector: an option the agent exposes to control how hard it thinks
// (Claude's thinking level, Gemini's thinking level, a reasoning effort).
// Zed's agents advertise these with category "thought_level"; Mistral's
// agents use category/id "thinking"; the id or name is the fallback signal
// (thinking_level, effort, reasoning_effort, ...).
static bool isEffortConfigOption(const QJsonObject &option)
{
    const QString category = option.value(u"category"_s).toString();
    if (category == u"model"_s || category == u"model_config"_s || category == u"mode"_s) {
        return false;
    }
    if (category == u"thought_level"_s || category == u"thinking"_s) {
        return true;
    }
    const QString id = option.value(u"id"_s).toString().toLower();
    const QString name = option.value(u"name"_s).toString().toLower();
    return id.contains(u"thought"_s) || id.contains(u"effort"_s) || id.contains(u"thinking"_s)
        || name.contains(u"thought"_s) || name.contains(u"effort"_s) || name.contains(u"thinking"_s);
}

// Returns true when an effort selector was present, even if it had no values.
static bool collectEffortConfig(const QJsonArray &configOptions, QStringList *efforts, QString *current, QString *configId)
{
    bool found = false;
    for (const QJsonValue &optionValue : configOptions) {
        const QJsonObject option = optionValue.toObject();
        if (!isEffortConfigOption(option)) {
            continue;
        }
        const QString type = option.value(u"type"_s).toString();
        if (!type.isEmpty() && type != u"select"_s) {
            continue;
        }
        found = true;
        *configId = option.value(u"id"_s).toString();
        *current = option.value(u"currentValue"_s).toString();
        efforts->clear();
        const QJsonArray options = option.value(u"options"_s).toArray();
        for (const QJsonValue &optValue : options) {
            appendModelEntry(efforts, optValue);
        }
    }
    return found;
}

// Mode selector: an option the agent exposes for execution mode (default,
// accept-edits, plan, etc.).
static bool isModeConfigOption(const QJsonObject &option)
{
    const QString category = option.value(u"category"_s).toString();
    if (category == u"mode"_s) {
        return true;
    }
    if (category == u"model"_s || category == u"model_config"_s
        || category == u"thought_level"_s || category == u"thinking"_s) {
        return false;
    }
    return option.value(u"id"_s).toString().compare(u"mode"_s, Qt::CaseInsensitive) == 0;
}

static bool collectModeConfig(const QJsonArray &configOptions, QStringList *modes, QString *current, QString *configId)
{
    bool found = false;
    for (const QJsonValue &optionValue : configOptions) {
        const QJsonObject option = optionValue.toObject();
        if (!isModeConfigOption(option)) {
            continue;
        }
        const QString type = option.value(u"type"_s).toString();
        if (!type.isEmpty() && type != u"select"_s) {
            continue;
        }
        found = true;
        *configId = option.value(u"id"_s).toString();
        *current = option.value(u"currentValue"_s).toString();
        modes->clear();
        const QJsonArray options = option.value(u"options"_s).toArray();
        for (const QJsonValue &optValue : options) {
            appendModelEntry(modes, optValue);
        }
    }
    return found;
}

void AcpClient::ingestSessionModels(const QJsonObject &result, bool applyPreferred)
{
    // A current_model_update may carry only the id, with no model list.
    if (result.contains(u"currentModelId"_s)) {
        m_currentModelId = result.value(u"currentModelId"_s).toString();
    }
    if (result.contains(u"currentEffortId"_s)) {
        m_currentEffortId = result.value(u"currentEffortId"_s).toString();
    }
    // ACP v2 setup responses can carry the initial complete command list, and
    // v1 setup responses (session/new, session/resume) do the same in recent
    // agents. Only replace the list when the field is present: an update that
    // omits it must not clear commands announced earlier.
    if (result.contains(u"availableCommands"_s)) {
        setAvailableCommands(result.value(u"availableCommands"_s).toArray());
    }

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

    // Mistral Vibe keeps its selected model in its own global state and
    // reports it as current in every session report, so a configured model
    // must be pushed onto the session instead of letting the agent's state
    // win. For every other agent the report is authoritative.
    const bool pinConfiguredModel = acpAgentIsMistralVibe(m_settings)
        && !m_settings.acpModel.trimmed().isEmpty();
    if (applyPreferred) {
        const QString preferred = m_settings.acpModel.trimmed();
        if (!preferred.isEmpty() && preferred != m_currentModelId
            && (pinConfiguredModel || m_agentModels.isEmpty() || m_agentModels.contains(preferred))) {
            setModel(preferred);
        } else if (preferred.isEmpty() && !m_currentModelId.isEmpty()) {
            m_settings.acpModel = m_currentModelId;
        }
    } else if (pinConfiguredModel && !m_modelSetInFlight) {
        // A report whose current model differs from the configured one means
        // the agent switched: restore the configured model. The response of
        // that restore settles here as well, but then the ids match and no
        // second request goes out, so this cannot loop.
        setModel(m_settings.acpModel.trimmed());
    }

    ingestSessionEfforts(result);
    ingestSessionModes(result);

    Q_EMIT modelsReceived(m_agentModels);
}

void AcpClient::ingestSessionModes(const QJsonObject &result)
{
    if (result.contains(u"currentModeId"_s)) {
        const QString modeId = result.value(u"currentModeId"_s).toString().trimmed();
        if (!modeId.isEmpty() && modeId != m_currentModeId) {
            m_currentModeId = modeId;
            Q_EMIT modeChanged(m_currentModeId);
        }
    }
    QStringList modes;
    QString current;
    QString configId;
    bool fromConfig = false;
    if (result.contains(u"configOptions"_s)) {
        fromConfig = collectModeConfig(result.value(u"configOptions"_s).toArray(), &modes, &current, &configId);
    }
    bool legacy = false;
    if (!fromConfig && result.contains(u"modes"_s) && result.value(u"modes"_s).isObject()) {
        const QJsonObject state = result.value(u"modes"_s).toObject();
        current = state.value(u"currentModeId"_s).toString().trimmed();
        for (const QJsonValue &entry : state.value(u"availableModes"_s).toArray()) {
            appendModelEntry(&modes, entry);
        }
        legacy = !modes.isEmpty() || !current.isEmpty();
    }
    if (!fromConfig && !legacy) {
        return;
    }
    if (fromConfig) {
        m_modeConfigId = configId.isEmpty() ? u"mode"_s : configId;
        m_legacySetMode = result.contains(u"modes"_s);
    } else if (legacy) {
        m_modeConfigId.clear();
        m_legacySetMode = true;
    }
    if (!current.isEmpty()) {
        const bool changed = (current != m_currentModeId);
        m_currentModeId = current;
        if (changed) {
            Q_EMIT modeChanged(m_currentModeId);
        }
    }
    if (!modes.isEmpty()) {
        m_agentModes = modes;
        Q_EMIT modesReceived(m_agentModes);
    }
    applyDesiredMode();
}

void AcpClient::setDesiredMode(const QString &modeId)
{
    const QString trimmed = modeId.trimmed();
    if (m_desiredModeId == trimmed) {
        return;
    }
    m_desiredModeId = trimmed;
    applyDesiredMode();
}

void AcpClient::applyDesiredMode()
{
    if (m_desiredModeId.isEmpty() || m_desiredModeId == m_currentModeId || m_modeSetInFlight) {
        return;
    }
    if (m_sessionId.isEmpty() || m_agentModes.isEmpty()) {
        return;
    }
    QString targetMode;
    for (const QString &m : m_agentModes) {
        if (m == m_desiredModeId) {
            targetMode = m;
            break;
        }
    }
    if (targetMode.isEmpty()) {
        for (const QString &m : m_agentModes) {
            if (m.compare(m_desiredModeId, Qt::CaseInsensitive) == 0) {
                targetMode = m;
                break;
            }
        }
    }
    if (targetMode.isEmpty() || targetMode == m_currentModeId) {
        return;
    }
    setSessionMode(targetMode);
}

bool AcpClient::setSessionMode(const QString &modeId)
{
    QString id = modeId.trimmed();
    if (id.isEmpty() || id == m_currentModeId) {
        return true;
    }
    if (m_sessionId.isEmpty()) {
        return false;
    }
    if (!m_agentModes.isEmpty()) {
        if (!m_agentModes.contains(id)) {
            QString matched;
            for (const QString &m : m_agentModes) {
                if (m.compare(id, Qt::CaseInsensitive) == 0) {
                    matched = m;
                    break;
                }
            }
            if (matched.isEmpty()) {
                return false;
            }
            id = matched;
        }
    }
    if (!m_modeConfigId.isEmpty()) {
        sendSetModeOption(id);
        return true;
    }
    if (m_legacySetMode) {
        sendSetSessionMode(id);
        return true;
    }
    return false;
}

void AcpClient::sendSetModeOption(const QString &modeId)
{
    m_modeSetInFlight = true;
    QJsonObject params;
    params.insert(u"sessionId"_s, m_sessionId);
    params.insert(u"configId"_s, m_modeConfigId.isEmpty() ? u"mode"_s : m_modeConfigId);
    params.insert(u"value"_s, modeId);
    sendRequest(u"session/set_config_option"_s, params, [this, modeId](const QJsonObject &result, const QString &error) {
        m_modeSetInFlight = false;
        if (!error.isEmpty()) {
            Q_EMIT logMessage(u"Failed to set session mode: %1"_s.arg(error));
            if (m_legacySetMode) {
                sendSetSessionMode(modeId);
            }
            return;
        }
        m_currentModeId = modeId;
        if (result.contains(u"configOptions"_s)) {
            ingestSessionModes(result);
        } else {
            Q_EMIT modeChanged(m_currentModeId);
        }
    });
}

void AcpClient::sendSetSessionMode(const QString &modeId)
{
    m_modeSetInFlight = true;
    QJsonObject params;
    params.insert(u"sessionId"_s, m_sessionId);
    params.insert(u"modeId"_s, modeId);
    sendRequest(u"session/set_mode"_s, params, [this, modeId](const QJsonObject &result, const QString &error) {
        m_modeSetInFlight = false;
        if (!error.isEmpty()) {
            Q_EMIT logMessage(u"Failed to set session mode: %1"_s.arg(error));
            return;
        }
        m_currentModeId = modeId;
        if (result.contains(u"modes"_s) || result.contains(u"configOptions"_s)) {
            ingestSessionModes(result);
        } else {
            Q_EMIT modeChanged(m_currentModeId);
        }
    });
}

void AcpClient::ingestSessionEfforts(const QJsonObject &result)
{
    // Model-only updates (current_model_update) carry no effort state; the
    // advertised levels must survive them untouched.
    if (!result.contains(u"configOptions"_s)) {
        return;
    }
    QStringList efforts;
    QString current;
    QString configId;
    if (!collectEffortConfig(result.value(u"configOptions"_s).toArray(), &efforts, &current, &configId)) {
        return;
    }
    if (!configId.isEmpty()) {
        m_effortConfigId = configId;
    }
    if (!current.isEmpty()) {
        m_currentEffortId = current;
    }
    if (!efforts.isEmpty()) {
        m_agentEfforts = efforts;
    }
    // A configured effort is forced, not merely preferred: an agent that
    // changed its own level gets pushed back to the configured one. The
    // response of that restore settles here as well, but then the ids match
    // and the in-flight guard blocks any second request, so this cannot loop.
    applyForcedEffort();
    Q_EMIT effortsReceived(m_agentEfforts);
}

void AcpClient::applyForcedEffort()
{
    const QString forced = m_settings.acpEffort.trimmed();
    // Auto: the agent's own level is authoritative and Kate sends nothing.
    if (forced.isEmpty() || forced == m_currentEffortId || m_effortSetInFlight) {
        return;
    }
    if (m_sessionId.isEmpty() || m_effortConfigId.isEmpty()) {
        return;
    }
    if (!m_agentEfforts.contains(forced)) {
        // A stale id from another agent/session would fail on every update.
        return;
    }
    sendSetEffortOption(forced);
}

bool AcpClient::setEffort(const QString &effortId)
{
    const QString id = effortId.trimmed();
    if (id.isEmpty() || id == m_currentEffortId) {
        return true;
    }
    if (m_sessionId.isEmpty() || m_effortConfigId.isEmpty()) {
        return false;
    }
    sendSetEffortOption(id);
    return true;
}

void AcpClient::sendSetEffortOption(const QString &effortId)
{
    m_effortSetInFlight = true;
    QJsonObject params;
    params.insert(u"sessionId"_s, m_sessionId);
    params.insert(u"configId"_s, m_effortConfigId);
    // value_id variant: `value` is the option id. `type` is omitted.
    params.insert(u"value"_s, effortId);
    sendRequest(u"session/set_config_option"_s, params, [this, effortId](const QJsonObject &result, const QString &error) {
        m_effortSetInFlight = false;
        if (!error.isEmpty()) {
            Q_EMIT logMessage(u"Failed to set effort level: %1"_s.arg(error));
            return;
        }
        m_currentEffortId = effortId;
        if (result.contains(u"configOptions"_s)) {
            ingestSessionEfforts(result);
        } else {
            Q_EMIT effortsReceived(m_agentEfforts);
        }
    });
}

static QString findNativeAgyExecutable()
{
    const QString found = QStandardPaths::findExecutable(u"agy"_s);
    if (!found.isEmpty()) {
        return found;
    }
    const QString toolDirFound = findExecutableInToolDirs(u"agy"_s);
    if (!toolDirFound.isEmpty()) {
        return toolDirFound;
    }
    const QString home = QDir::homePath();
#ifdef Q_OS_WIN
    const QStringList candidates = {
        home + u"/.gemini/antigravity-cli/bin/agy.exe"_s,
        home + u"/.local/bin/agy.exe"_s,
    };
#else
    const QStringList candidates = {
        home + u"/.gemini/antigravity-cli/bin/agy"_s,
        home + u"/.local/bin/agy"_s,
    };
#endif
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return {};
}

static bool isLegacyAntigravityCommand(const QString &command)
{
    return QFileInfo(command).fileName().toLower().contains(u"agy_acp_server"_s);
}

static bool isLaunchingLegacyAntigravityServer(const Settings &settings)
{
    const QString launch = AcpClient::resolvedCommand(settings);
    return isLegacyAntigravityCommand(launch.isEmpty() ? acpEffectiveCommand(settings) : launch);
}

QString AcpClient::resolvedCommand(const Settings &settings)
{
    const QString command = acpEffectiveCommand(settings);
    if (command.isEmpty()) {
        return {};
    }

    // Antigravity: the ACP registry distributes the legacy standalone
    // Python/C++ `agy_acp_server.par`, which proxies model calls through an
    // embedded Python HTTP/1.1 urllib proxy that fails long/heavy reasoning
    // turns at 60s with "502 Bad Gateway: Failed to connect to backend API"
    // and requires the 4KB page shim on >4KB kernels. Prefer the modern native
    // Go `agy` CLI (`agy acp`) whenever it is installed.
    if (acpAgentIsLegacyAntigravityServer(settings)) {
        const QString nativeAgy = findNativeAgyExecutable();
        if (!nativeAgy.isEmpty()) {
            return nativeAgy;
        }
    }

    const QFileInfo commandInfo(command);
    if (commandInfo.isAbsolute()) {
        return command;
    }

    // Binary registry agents record their executable relative to the unpacked
    // archive ("./agy_acp_server.par"). Resolve it inside Kate's install
    // directory before falling back to PATH or the raw recorded command.
    const QString installedBinary = acpInstalledBinaryCommand(settings);
    if (!installedBinary.isEmpty()
        && commandInfo.fileName() == QFileInfo(installedBinary).fileName()) {
        return installedBinary;
    }

    // MiniMax Code: prefer the standalone CLI the agent installs through its own
    // installer (~/.minimax-code/bin/mcode). The ACP registry entry instead
    // records the npm distribution (@minimax-ai/code), whose bundled native
    // modules can fail to load on the current Node runtime; the process then
    // exits with code 1 before the ACP handshake, which surfaces as "ACP agent
    // exited (code 1)". The standalone binary speaks the same ACP server and is
    // the one that actually works.
    if (acpAgentIsMinimaxCode(settings)) {
        const QString standalone = QDir::homePath() + u"/.minimax-code/bin/mcode"_s;
        const QFileInfo standaloneInfo(standalone);
        if (standaloneInfo.exists() && standaloneInfo.isExecutable()) {
            return standalone;
        }
    }

    // A bare name and the registry's "./<name>" form name the same binary.
    // Resolve both the same way, so "./opencode" finds the executable the
    // user's shell would run instead of depending on the process working
    // directory, which is what QProcess uses for a relative path.
    const QString baseName = commandInfo.fileName();
    const bool relative = command.contains(u'/') || command.contains(u'\\');
    const bool registryRelative = relative && !baseName.isEmpty()
        && (command == u"./"_s + baseName || command == u".\\"_s + baseName);
    if (!relative || registryRelative) {
        const QString found = QStandardPaths::findExecutable(baseName);
        if (!found.isEmpty()) {
            return found;
        }
        const QString toolDirFound = findExecutableInToolDirs(baseName);
        if (!toolDirFound.isEmpty()) {
            return toolDirFound;
        }
        const QString home = QDir::homePath();
        QStringList fallbacks;
        if (baseName == u"grok"_s) {
            fallbacks.append(home + u"/.grok/bin/"_s + baseName);
        }
        if (baseName == u"opencode"_s) {
            // opencode's installer puts the binary in ~/.opencode/bin and only
            // adds that to the *shell* PATH; a Kate launched from the desktop
            // never sees it, so the agent cannot be started at all from there.
            fallbacks.append(home + u"/.opencode/bin/"_s + baseName);
        }
        if (baseName == u"agy"_s || baseName == u"agy.exe"_s) {
            fallbacks.append(home + u"/.gemini/antigravity-cli/bin/"_s + baseName);
        }
        fallbacks.append(home + u"/.local/bin/"_s + baseName);
        for (const QString &candidate : fallbacks) {
            const QFileInfo info(candidate);
            if (info.exists() && info.isExecutable()) {
                return info.absoluteFilePath();
            }
        }
    }
    if (!relative) {
        // An unresolved bare name is kept so the launch failure still names the
        // command the user configured.
        return command;
    }
    // Letting QProcess interpret an unresolved relative path against the
    // workspace could execute a project file that happens to share the name.
    // A binary registry entry is staged by the installer, so its absence means
    // the install is incomplete; every other relative path is the user's own
    // launch line and is left to the working directory.
    if (acpAgentHasBinaryInstall(settings)) {
        return {};
    }
    return command;
}

QStringList AcpClient::agentArguments(const Settings &settings)
{
    const QString raw = acpEffectiveArgs(settings);
    QStringList args = raw.isEmpty() ? QStringList() : QProcess::splitCommand(raw);
    const QString command = acpEffectiveCommand(settings);
    const QString launch = resolvedCommand(settings);
    const QString effectiveCommand = launch.isEmpty() ? command : launch;
    const QString agentId = settings.acpAgentId;
    const QString model = settings.acpModel.trimmed();
    // Per-agent launch quirks, keyed by registry id and the resolved launch
    // executable so upgrading a legacy registry binary (e.g. `agy_acp_server`
    // -> `agy`) applies the target binary's flags rather than the legacy ones.
    const AcpAgentQuirks quirks = acpAgentQuirks(agentId, effectiveCommand);

    for (const QString &strip : quirks.stripArgs) {
        args.removeAll(strip);
    }
    if (acpAgentIsAntigravity(settings) && !isLegacyAntigravityCommand(effectiveCommand)) {
        // The ACP registry entry for `antigravity-acp` (`agy_acp_server.par`)
        // persists `--uid=` and legacy gflags that modern `agy acp` rejects
        // with exit code 1. Strip any value-bearing variant as well.
        QStringList kept;
        kept.reserve(args.size());
        for (int i = 0; i < args.size(); ++i) {
            const QString &arg = args.at(i);
            if (arg == u"--uid"_s) {
                if (i + 1 < args.size() && !args.at(i + 1).startsWith(u'-') && args.at(i + 1) != u"acp"_s) {
                    ++i;
                }
                continue;
            }
            if (arg.startsWith(u"--uid="_s)
                || arg.startsWith(u"--enforce_kernel_ipv6_support="_s)
                || arg.startsWith(u"--core_register_sanitization_enabled="_s)
                || arg.startsWith(u"--core_sanitization_enabled="_s)
                || arg.startsWith(u"--boringssl_sanitization_enabled="_s)
                || arg.startsWith(u"--hugepage_text="_s)) {
                continue;
            }
            kept.append(arg);
        }
        args = kept;
    }
    if (quirks.stripNpmTokens) {
        // npm-only tokens from a recorded npx line ("--yes", "@scope/pkg")
        // are noise for the standalone mcode binary. When the launch command
        // is still npx (the standalone binary is not installed), stripping the
        // package would leave "npx acp" and run an unrelated package.
        const bool standalone = QFileInfo(launch).fileName() == u"mcode"_s;
        if (standalone) {
            QStringList kept;
            for (const QString &arg : args) {
                if (arg == u"--yes"_s || arg.startsWith(u"@"_s)) {
                    continue;
                }
                kept.append(arg);
            }
            args = kept;
        }
    }
    if (quirks.needsAcpSubcommand && !args.contains(u"acp"_s)) {
        // Without the "acp" subcommand these CLIs start their interactive TUI
        // instead of the ACP server and stall the handshake until timeout.
        args.append(u"acp"_s);
    }

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

    // Model flag for every agent whose CLI accepts --model (the quirks table
    // says which). Gemini wants it before its --acp flag; everyone else takes
    // it at the end.
    if (!model.isEmpty() && quirks.modelFlag && !args.contains(u"--model"_s) && !args.contains(u"-m"_s)) {
        const int acpIndex = args.indexOf(u"--acp"_s);
        if (acpIndex >= 0) {
            args.insert(acpIndex, u"--model"_s);
            args.insert(acpIndex + 1, model);
        } else {
            args << u"--model"_s << model;
        }
    }

    for (const QString &extra : quirks.extraArgs) {
        if (!args.contains(extra)) {
            args << extra;
        }
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
    // A parked client method holds the question slot when an agent-initiated
    // request arrives: refuse it, the way the earlier request is answered as
    // cancelled below, so the agent does not block on the parked call.
    cancelParkedMethods();
    cancelPendingQuestion();
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

void AcpClient::cancelPendingQuestion()
{
    if (m_permissionQuestion) {
        const QString toolCallId = m_permissionRequest.toolCallId;
        QJsonObject outcome;
        outcome.insert(u"outcome"_s, u"cancelled"_s);
        QJsonObject result;
        result.insert(u"outcome"_s, outcome);
        replyResult(m_permissionRpcId, result);
        m_permissionRpcId = QJsonValue();
        m_permissionOptions = QJsonArray();
        m_permissionRequest = {};
        m_permissionQuestion = false;
        if (!toolCallId.isEmpty()) {
            Q_EMIT questionCancelled(toolCallId);
        }
        askNextParkedMethod();
        return;
    }
    if (!hasPendingQuestion()) {
        return;
    }
    const QString toolCallId = m_questionRequest.toolCallId;
    QJsonObject result;
    if (m_questionIsElicitation) {
        // ACP elicitation uses a three-action response, not the extension's
        // {cancelled, answer} shape.
        result.insert(u"action"_s, u"cancel"_s);
    } else {
        // Same shape as the ask_user answer, but cancelled: the agent must be
        // able to tell "the user dismissed this" apart from a real answer.
        result.insert(u"cancelled"_s, true);
        result.insert(u"answer"_s, QString());
    }
    replyResult(m_questionRpcId, result);
    m_questionRpcId = QJsonValue();
    m_questionRequest = {};
    m_questionIsElicitation = false;
    m_questionElicitationProperty.clear();
    m_questionElicitationValueMap.clear();
    if (!toolCallId.isEmpty()) {
        Q_EMIT questionCancelled(toolCallId);
    }
    askNextParkedMethod();
}

void AcpClient::answerQuestion(const QString &answer)
{
    if (m_permissionQuestion) {
        // A permission request used as an ask-user channel: the chosen label
        // selects its optionId, which is the only answer ACP can carry back.
        QString chosen;
        for (const QJsonValue &value : std::as_const(m_permissionOptions)) {
            const QJsonObject option = value.toObject();
            const QString optionId = option.value(u"optionId"_s).toString();
            if (optionId.isEmpty()) {
                continue;
            }
            const QString label = firstStringField(option, {u"name"_s, u"label"_s, u"title"_s});
            const QString text = answer.trimmed();
            if (!text.isEmpty() && (text == optionId || text == label)) {
                chosen = optionId;
                break;
            }
        }
        QJsonObject outcome;
        if (chosen.isEmpty()) {
            outcome.insert(u"outcome"_s, u"cancelled"_s);
        } else {
            outcome.insert(u"outcome"_s, u"selected"_s);
            outcome.insert(u"optionId"_s, chosen);
        }
        QJsonObject result;
        result.insert(u"outcome"_s, outcome);
        replyResult(m_permissionRpcId, result);
        m_permissionRpcId = QJsonValue();
        m_permissionOptions = QJsonArray();
        m_permissionRequest = {};
        m_permissionQuestion = false;
        askNextParkedMethod();
        return;
    }
    if (!hasPendingQuestion()) {
        return;
    }
    QJsonObject result;
    if (m_questionIsElicitation) {
        if (answer.trimmed().isEmpty()) {
            result.insert(u"action"_s, u"cancel"_s);
        } else {
            result.insert(u"action"_s, u"accept"_s);
            QJsonObject content;
            // The editor card asks one textual question. Map the answer onto
            // the first schema property, which is what a single-field form
            // means in practice.
            const QString property = m_questionElicitationProperty.isEmpty() ? u"answer"_s : m_questionElicitationProperty;
            const QString mapped = m_questionElicitationValueMap.value(answer, answer);
            content.insert(property, mapped);
            result.insert(u"content"_s, content);
        }
    } else if (answer.trimmed().isEmpty()) {
        result.insert(u"cancelled"_s, true);
        result.insert(u"answer"_s, QString());
    } else {
        result.insert(u"cancelled"_s, false);
        result.insert(u"answer"_s, answer);
    }
    replyResult(m_questionRpcId, result);
    m_questionRpcId = QJsonValue();
    m_questionRequest = {};
    m_questionIsElicitation = false;
    m_questionElicitationProperty.clear();
    m_questionElicitationValueMap.clear();
    askNextParkedMethod();
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
    m_toolProgressAt.clear();
    m_toolTerminals.clear();
    m_terminalTools.clear();
    m_lastToolCallId.clear();
}

void AcpClient::cancel()
{
    // A Stop means the turn and everything queued behind it are cancelled; a
    // queued prompt replayed after the agent answers session/cancel started a
    // turn the user had already stopped.
    m_queued.clear();
    cancelPendingPermission();
    if (m_turnRetryPending) {
        // The failed turn was waiting to be retried. Stop closes it here: a
        // cancelled turn must not resurrect itself through the retry timer.
        stopTurnRetry();
        m_turnRetryAttempt = 0;
        finishPrompt(u"cancelled"_s);
        return;
    }
    if (m_promptId != 0 && m_sessionId.isEmpty()) {
        // The prompt is pending but its session id was cleared (resetSession),
        // so session/cancel cannot be sent. Close the turn locally instead of
        // leaving it pending forever.
        finishLiveTools(u"Cancelled."_s);
        finishPrompt(u"cancelled"_s);
        return;
    }
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
            m_cancelGraceTimer->deleteLater();
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
    // Do this before the bookkeeping below clears m_promptId. A stop issued
    // while a turn is running (settings/workspace change) otherwise left the
    // owner busy forever, because every prompt callback bails while shutting
    // down and no failure was reported.
    const bool hadActiveTurn = m_promptId != 0;
    m_shuttingDown = true;
    stopTurnRetry();
    m_turnRetryAttempt = 0;
    if (m_startupTimer) {
        m_startupTimer->stop();
    }
    if (m_cancelGraceTimer) {
        m_cancelGraceTimer->stop();
        m_cancelGraceTimer->deleteLater();
        m_cancelGraceTimer = nullptr;
    }
    failAll(u"ACP agent disconnected."_s);
    releaseAllTerminals();
    if (m_process) {
        if (m_process->state() != QProcess::NotRunning) {
            m_process->disconnect(this);
            m_process->terminate();
            if (!m_process->waitForFinished(500)) {
                m_process->kill();
                m_process->waitForFinished(500);
            }
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_antigravityAuthUrlOpened = false;
    m_discardingStdoutFrame = false;
    m_oversizedFrameReported = false;
    m_sessionId.clear();
    m_replayingLoad = false;
    m_promptId = 0;
    // Capabilities and commands belong to the agent that just went away.
    // Keeping them made the next agent inherit the previous one's slash
    // commands and feature flags.
    m_availableCommands = QJsonArray();
    m_loadSession = false;
    m_resumeSession = false;
    m_closeSession = false;
    m_embeddedContext = false;
    m_supportsSystemPromptAppend = false;
    m_mcpStdioCapable = true;
    m_mcpHttpCapable = false;
    m_protocolVersion = 1;
    m_permissionRpcId = QJsonValue();
    m_permissionOptions = QJsonArray();
    m_permissionRequest = {};
    m_permissionQuestion = false;
    m_questionRpcId = QJsonValue();
    m_questionRequest = {};
    m_questionIsElicitation = false;
    m_questionElicitationProperty.clear();
    m_questionElicitationValueMap.clear();
    // The agent process is going away, so parked RPCs cannot be answered
    // meaningfully; drop them with the rest of the session state.
    m_parkedMethods.clear();
    m_activeParked.reset();
    m_sessionGrants.clear();
    m_onceGrants.clear();
    m_queued.clear();
    m_sessionNotesSent = false;
    m_toolTerminals.clear();
    m_terminalTools.clear();
    m_agentModels.clear();
    m_currentModelId.clear();
    m_modelConfigId.clear();
    m_legacySetModel = false;
    m_modelSetInFlight = false;
    m_agentEfforts.clear();
    m_currentEffortId.clear();
    m_effortConfigId.clear();
    m_effortSetInFlight = false;
    m_agentModes.clear();
    m_currentModeId.clear();
    m_modeConfigId.clear();
    m_legacySetMode = false;
    m_modeSetInFlight = false;
    m_contextUsedTokens = 0;
    m_contextWindowSize = 0;
    if (m_state != State::Stopped) {
        setState(State::Stopped, u"Disconnected"_s);
    }
    if (hadActiveTurn) {
        Q_EMIT failed(u"The ACP agent was stopped before the turn finished."_s);
    }
}

void AcpClient::resetSession()
{
    // Any permission question posed in the outgoing session must be closed
    // before the session id goes away.
    cancelPendingPermission();
    // A new session is a new trust scope: approvals from the old one do not
    // carry over.
    m_sessionGrants.clear();
    m_onceGrants.clear();
    m_resumeSessionId.clear();
    m_queued.clear();
    m_sessionNotesSent = false;
    if (!m_availableCommands.isEmpty()) {
        // Slash commands belonged to the outgoing session; the next
        // session/new response replaces the list, and omission means none.
        setAvailableCommands(QJsonArray());
    }
    if (m_closeSession && !m_sessionId.isEmpty() && m_state == State::Ready) {
        QJsonObject params;
        params.insert(u"sessionId"_s, m_sessionId);
        sendRequest(u"session/close"_s, params, {}, 5000);
    }
    m_sessionId.clear();
    m_sessionOpening = false;
    m_replayingLoad = false;
    Q_EMIT sessionIdChanged(QString());
}

void AcpClient::prompt(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }
    // The task a retry continuation quotes: a retry prompt overwrites
    // m_promptText with itself, and a chained retry must still name the
    // user's request, not the previous retry.
    m_taskPrompt = text;
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
    resolvePermission(decision, QString());
}

void AcpClient::resolvePermission(PermissionDecision decision, const QString &toolCallId)
{
    // A parked client method is answered by running it or refusing it, not by
    // replying with a permission outcome.
    if (m_activeParked) {
        if (!toolCallId.isEmpty() && m_activeParked->request.toolCallId != toolCallId) {
            return;
        }
        const ParkedMethod parked = *m_activeParked;
        m_activeParked.reset();
        if (decision == PermissionDecision::Deny) {
            replyError(parked.rpcId, kServerError, u"Permission denied."_s);
            ToolResult result;
            result.toolCallId = parked.request.toolCallId;
            result.name = parked.request.toolName;
            result.ok = false;
            result.cancelled = true;
            result.output = u"Permission denied."_s;
            Q_EMIT toolFinished(result);
        } else {
            if (decision == PermissionDecision::AllowSession) {
                m_sessionGrants.insert(parked.grantKey);
            } else {
                m_onceGrants.insert(parked.grantKey);
            }
            dispatchParkedMethod(parked);
            // The card was opened by askNextParkedMethod(); the handler only
            // answers the JSON-RPC, so without this it stayed "running" and
            // the end-of-turn sweep marked a successful write as cancelled.
            ToolResult result;
            result.toolCallId = parked.request.toolCallId;
            result.name = parked.request.toolName;
            result.ok = true;
            if (parked.method == u"terminal/create"_s) {
                result.output = u"Terminal started."_s;
            } else if (parked.method == u"fs/delete_file"_s) {
                result.output = u"File deleted."_s;
            } else if (parked.method == u"fs/edit_file"_s || parked.method == u"fs/multi_edit_file"_s) {
                result.output = u"Edit completed."_s;
            } else if (parked.method == u"apply_patch"_s) {
                result.output = u"Patch applied."_s;
            } else {
                result.output = u"Write completed."_s;
            }
            Q_EMIT toolFinished(result);
        }
        askNextParkedMethod();
        return;
    }
    if (!hasPendingPermission()) {
        return;
    }
    if (!toolCallId.isEmpty() && m_permissionRequest.toolCallId != toolCallId) {
        return;
    }
    if (decision != PermissionDecision::Deny) {
        // The approval covers the matching client method, so an agent that
        // announced its edit or command before asking is not asked twice.
        recordApprovalGrants(m_permissionRequest, decision);
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
    m_permissionQuestion = false;
    // An agent-initiated request that just resolved frees the question slot:
    // the next parked client method can be asked now.
    askNextParkedMethod();
}

void AcpClient::setState(State state, const QString &status)
{
    m_state = state;
    m_status = status;
    if (state == State::Ready || state == State::Failed || state == State::Stopped) {
        m_sessionOpening = false;
    }
}

QString AcpClient::sessionCwd() const
{
    if (!m_workspace.trimmed().isEmpty() && QFileInfo(m_workspace).isDir()) {
        return m_workspace;
    }
    return QDir::currentPath();
}

// MiniMax Code refuses session/new with "The OAuth credential file
// permissions are not private" when its own credentials under ~/.minimax/auth
// are readable by group or other (the agent itself writes them that way under
// a permissive umask). Tighten them to owner-only before launching: 0600 is
// what the agent demands, and nothing besides the agent reads those files.
void acpTightenMinimaxCredentials()
{
    const QString authRoot = QDir::homePath() + u"/.minimax/auth"_s;
    QDirIterator it(authRoot, {u"auth.json"_s, u"auth-state.json"_s}, QDir::Files,
                   QDirIterator::Subdirectories);
    const QFile::Permissions privateMode = QFile::ReadOwner | QFile::WriteOwner;
    while (it.hasNext()) {
        QFile credential(it.next());
        if ((credential.permissions() & ~privateMode) != 0) {
            credential.setPermissions(privateMode);
        }
    }
}

void AcpClient::spawnProcess()
{
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(500);
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_shuttingDown = false;
    m_discardingStdoutFrame = false;
    m_oversizedFrameReported = false;
    m_stderrBuffer.clear();
    // A respawn is a new process/session. Leftovers from the previous one
    // (half a frame, live tool ids, one-turn grants) must not be applied to
    // the new agent.
    m_stdoutBuffer.clear();
    m_sessionId.clear();
    m_sessionOpening = false;
    m_replayingLoad = false;
    m_promptId = 0;
    m_lastToolCallId.clear();
    m_liveTools.clear();
    m_toolStreamed.clear();
    m_toolProgressAt.clear();
    m_toolTerminals.clear();
    m_terminalTools.clear();
    m_onceGrants.clear();
    m_sessionGrants.clear();
    m_antigravityAuthUrlOpened = false;
    m_permissionRpcId = QJsonValue();
    m_permissionOptions = QJsonArray();
    m_permissionRequest = {};
    m_permissionQuestion = false;
    m_questionRpcId = QJsonValue();
    m_questionRequest = {};
    m_questionIsElicitation = false;
    m_questionElicitationProperty.clear();
    m_parkedMethods.clear();
    m_activeParked.reset();
    setState(State::Starting, u"Starting ACP agent…"_s);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    if (!m_workspace.trimmed().isEmpty() && QFileInfo(m_workspace).isDir()) {
        m_process->setWorkingDirectory(m_workspace);
    }
    m_process->setProcessEnvironment(processEnvironment());

    connect(m_process, &QProcess::readyReadStandardOutput, this, &AcpClient::onStdioReadyRead);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // A crash after a successful start is reported by finished(), with
        // the exit code and stderr. Reporting it here as "could not start"
        // was both wrong and a duplicate failure.
        if (error != QProcess::FailedToStart) {
            return;
        }
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
        QString message = u"ACP agent exited (code %1)."_s.arg(exitCode);
        const QString stderrText = m_stderrBuffer.trimmed();
        if (!stderrText.isEmpty()) {
            // The exit code alone does not explain much; agents print their own
            // diagnostics (native module load failures, missing binaries, auth
            // problems) on stderr right before dying, and that is the useful part.
            const QStringList stderrLines = stderrText.split(u"\n"_s, Qt::SkipEmptyParts);
            QString detail;
            for (const QString &line : stderrLines) {
                detail += u"%1\n"_s.arg(line);
            }
            message += u" Stderr: %1"_s.arg(detail.trimmed());
        }
        failAll(message);
        setState(State::Failed, message);
        Q_EMIT failed(message);
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        const QString noise = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (!noise.isEmpty()) {
            // The buffer holds the agent's diagnostics for the whole lifetime
            // of the process and is pasted into the error message when it
            // exits. An agent that chatters on stderr would grow it without
            // bound, so the head and the tail are kept and the rest dropped -
            // the tail is where a failure summary lives.
            constexpr qsizetype kMaxStderrChars = 64 * 1024;
            m_stderrBuffer.append(u"%1\n"_s.arg(noise));
            if (m_stderrBuffer.size() > kMaxStderrChars) {
                m_stderrBuffer = u"[... earlier output dropped ...]\n"_s + m_stderrBuffer.right(kMaxStderrChars);
            }
            Q_EMIT logMessage(noise);
            if (acpAgentIsAntigravity(m_settings) && !m_antigravityAuthUrlOpened) {
                static const QRegularExpression authUrlPattern(
                    u"https://accounts\\.google\\.com/[^\\s\\\"'<>]+"_s);
                const auto match = authUrlPattern.match(m_stderrBuffer);
                if (match.hasMatch()) {
                    QString authUrl = match.captured();
                    while (!authUrl.isEmpty() && u".,);]"_s.contains(authUrl.back())) {
                        authUrl.chop(1);
                    }
                    const QUrl url(authUrl);
                    if (url.isValid() && url.scheme() == u"https"_s
                        && url.host() == u"accounts.google.com"_s) {
                        m_antigravityAuthUrlOpened = true;
                        if (!QDesktopServices::openUrl(url)) {
                            Q_EMIT logMessage(u"Could not open Antigravity sign-in URL: %1"_s.arg(authUrl));
                        }
                    }
                }
            }
        }
    });

    const QString command = resolvedCommand(m_settings);
    const QStringList args = agentArguments(m_settings);
    if (command.isEmpty()) {
        const QString configured = acpEffectiveCommand(m_settings);
        const QString message = configured.isEmpty()
            ? u"ACP agent command is empty. Pick an agent in Settings or set a command."_s
            : u"Could not find the ACP agent executable '%1'. Install it or fix the launch command in Settings."_s.arg(configured);
        setState(State::Failed, message);
        Q_EMIT failed(message);
        return;
    }
    if (acpAgentIsMinimaxCode(m_settings)) {
        acpTightenMinimaxCredentials();
    }
    m_process->start(command, args);
    if (!m_process->waitForStarted(5000)) {
        // errorOccurred(FailedToStart) is delivered during waitForStarted and
        // already reported the failure. Do not report it a second time.
        if (m_state == State::Failed) {
            return;
        }
        const QString message = u"Could not start ACP agent '%1': %2"_s.arg(command, m_process->errorString());
        failAll(message);
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
    // Antigravity ACP authentication may wait for the user to finish its
    // browser OAuth flow. npx/uvx have to fetch and unpack a package on first
    // run, which can take longer than a plain local binary; give them room so
    // the first launch of a registry agent does not fail the handshake.
    const QString startupCommand = acpEffectiveCommand(m_settings);
    const QString startupBase = QFileInfo(startupCommand).fileName();
    const bool packageRunner = startupBase == u"npx"_s || startupBase == u"uvx"_s
        || startupBase == u"npm"_s || startupBase == u"pnpm"_s || startupBase == u"bunx"_s;
    int startupTimeoutMs = 60000;
    if (acpAgentIsAntigravity(m_settings)) {
        startupTimeoutMs = 300000;
    } else if (packageRunner) {
        startupTimeoutMs = 180000;
    }
    m_startupTimer->setInterval(startupTimeoutMs);
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
    // GUI-launched editors often get a minimal PATH. Registry agents are mostly
    // npx/uvx packages installed under the user's tool managers, so make those
    // locations visible to both the parent-side lookup and the child process.
    {
        QString path = env.value(u"PATH"_s);
        QStringList entries = path.split(QDir::listSeparator(), Qt::SkipEmptyParts);
        for (const QString &dir : agentToolDirectories()) {
            if (QFileInfo(dir).isDir() && !entries.contains(dir)) {
                entries.append(dir);
            }
        }
        env.insert(u"PATH"_s, entries.join(QDir::listSeparator()));
    }
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
        if (acpAgentIsAntigravity(m_settings)) {
            env.insert(u"GEMINI_API_KEY"_s, key);
        }
    }

    // Registry entries can carry an env array (top-level and per-distribution)
    // naming variables the agent needs, e.g. which variable holds its API key
    // or feature flags. Merge it under the user's explicit key so the typed
    // credential always wins.
    const QJsonArray installed = QJsonDocument::fromJson(m_settings.acpInstalledAgents.toUtf8()).array();
    for (const QJsonValue &value : installed) {
        const QJsonObject agent = value.toObject();
        if (agent.value(u"id"_s).toString() != m_settings.acpAgentId) {
            continue;
        }
        auto mergeEnv = [&env](const QJsonValue &raw) {
            auto insert = [&env](const QString &name, const QString &value) {
                if (!name.isEmpty() && !env.contains(name)) {
                    env.insert(name, value);
                }
            };
            if (raw.isObject()) {
                // The registry schema uses a JSON object for env; older
                // records and some hand-written entries use [{name,value}].
                const QJsonObject map = raw.toObject();
                for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
                    insert(it.key(), it.value().toString());
                }
                return;
            }
            for (const QJsonValue &row : raw.toArray()) {
                const QJsonObject obj = row.toObject();
                insert(obj.value(u"name"_s).toString(), obj.value(u"value"_s).toString());
            }
        };
        mergeEnv(agent.value(u"env"_s));
        const QJsonObject distribution = agent.value(u"distribution"_s).toObject();
        for (const QJsonValue &dist : {distribution.value(u"npx"_s), distribution.value(u"uvx"_s), distribution.value(u"binary"_s)}) {
            mergeEnv(dist.toObject().value(u"env"_s));
        }
        break;
    }

    // Only the legacy standalone C++/Chromium agy_acp_server binary needs the
    // 4KB page shim on >4KB hosts. The modern Go `agy` CLI runs natively and
    // must not get a spoofed sysconf(_SC_PAGESIZE).
    if (isLaunchingLegacyAntigravityServer(m_settings) && acpNeedsPageShim()) {
        const QString shim = acpPageShimPath(m_settings.acpAgentId);
        if (!shim.isEmpty()) {
            const QString currentPreload = env.value(u"LD_PRELOAD"_s);
            if (currentPreload.isEmpty()) {
                env.insert(u"LD_PRELOAD"_s, shim);
            } else if (!currentPreload.split(u':').contains(shim)) {
                env.insert(u"LD_PRELOAD"_s, currentPreload + u':' + shim);
            }
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
    // Elicitation form is implemented; the value must be an object, not a bool.
    capabilities.insert(u"elicitation"_s, QJsonObject{{u"form"_s, QJsonObject{}}});
    // Config option support is a standard client-session capability. Kate
    // handles select options; advertising boolean support would promise
    // options sendSetConfigOption cannot send, so only select is implied.
    capabilities.insert(u"session"_s, QJsonObject{{u"configOptions"_s, QJsonObject{}}});
    // Kate-specific client methods agents may call are advertised under _meta,
    // where the protocol expects extensions. A top-level "kateai" field is
    // outside the schema and strict validators can reject initialize outright.
    capabilities.insert(u"_meta"_s, QJsonObject{
        {u"com.kateai/methods"_s, QJsonArray{u"_kateai/query_project_graph"_s, u"_kateai/write_plan_file"_s, u"_kateai/suggest"_s,
                                           u"_kateai/ask_user"_s, u"_kateai/todo_write"_s, u"_kateai/todo_read"_s,
                                           u"_kateai/exit_plan_mode"_s,
                                           u"_kateai/list_dir"_s, u"_kateai/edit_file"_s, u"_kateai/multi_edit_file"_s,
                                           u"_kateai/delete_file"_s, u"_kateai/grep"_s, u"_kateai/glob"_s,
                                           u"_kateai/apply_patch"_s,
                                           u"_kateai/lsp_diagnostics"_s, u"_kateai/lsp_symbols"_s, u"_kateai/lsp_hover"_s,
                                           u"_kateai/lsp_definition"_s, u"_kateai/lsp_references"_s, u"_kateai/lsp_code_actions"_s,
                                           u"_kateai/debug_status"_s, u"_kateai/debug_stack"_s, u"_kateai/debug_locals"_s,
                                           u"_kateai/debug_breakpoints"_s, u"_kateai/debug_select_frame"_s, u"_kateai/debug_select_thread"_s,
                                           u"_kateai/debug_snapshot"_s, u"_kateai/debug_set_breakpoint"_s, u"_kateai/debug_step"_s,
                                           u"_kateai/debug_eval"_s, u"_kateai/debug_session"_s, u"_kateai/debug_console"_s,
                                           u"_kateai/build_targets"_s, u"_kateai/build_run"_s, u"_kateai/terminal_run"_s}},
    });

    QJsonObject clientInfo;
    // Some ACP agents (e.g. Google Antigravity) restrict third-party models
    // to specific known clients. Pretend to be Zed for those agents so we
    // can discover the full model list. Keyed by registry id via the quirks
    // table, so other gated agents can be added there.
    const QString launchCmd = resolvedCommand(m_settings);
    if (acpAgentQuirks(m_settings.acpAgentId, launchCmd.isEmpty() ? acpEffectiveCommand(m_settings) : launchCmd).spoofZedClientInfo) {
        clientInfo.insert(u"name"_s, u"zed"_s);
        clientInfo.insert(u"title"_s, u"Zed"_s);
    } else {
        clientInfo.insert(u"name"_s, u"kateai"_s);
        clientInfo.insert(u"title"_s, u"Kate AI"_s);
    }
#ifdef KATEAI_VERSION
    clientInfo.insert(u"version"_s, QStringLiteral(KATEAI_VERSION));
#else
    clientInfo.insert(u"version"_s, u"0.4.0"_s);
#endif

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
        // Negotiate like Zed does: send our maximum, accept whatever the agent
        // chose (clamped to what we speak), and gate optional features on it
        // instead of failing the handshake. A v2 agent that answers 2 is
        // clamped to our v1 and simply does not get the v2 features.
        const int agentVersion = result.value(u"protocolVersion"_s).toInt(kAcpProtocolVersion);
        m_protocolVersion = qMin(agentVersion, kAcpProtocolVersion);
        if (agentVersion != kAcpProtocolVersion) {
            Q_EMIT logMessage(u"ACP agent speaks protocol version %1; Kate AI negotiated %2."_s.arg(agentVersion).arg(m_protocolVersion));
        }
        const QJsonObject caps = result.value(u"agentCapabilities"_s).toObject();
        m_loadSession = caps.value(u"loadSession"_s).toBool() || caps.value(u"load_session"_s).toBool();
        m_embeddedContext = caps.value(u"promptCapabilities"_s).toObject().value(u"embeddedContext"_s).toBool();
        m_supportsSystemPromptAppend = caps.value(u"_meta"_s).toObject()
            .value(u"antigravity"_s).toObject()
            .value(u"systemPrompt"_s).toObject()
            .value(u"append"_s).toBool(false);
        QJsonObject sessionCaps = caps.value(u"sessionCapabilities"_s).toObject();
        if (sessionCaps.isEmpty()) {
            sessionCaps = caps.value(u"session_capabilities"_s).toObject();
        }
        const QJsonValue resumeCap = sessionCaps.value(u"resume"_s);
        const QJsonValue closeCap = sessionCaps.value(u"close"_s);
        const QJsonValue loadCap = sessionCaps.value(u"load"_s);
        m_resumeSession = !resumeCap.isUndefined() && !resumeCap.isNull();
        m_closeSession = !closeCap.isUndefined() && !closeCap.isNull();
        // ACP v2 moved load under sessionCapabilities; accept both spellings.
        if (!loadCap.isUndefined() && !loadCap.isNull()) {
            m_loadSession = true;
        }
        const QJsonValue mcpCapsValue = caps.value(u"mcpCapabilities"_s);
        if (mcpCapsValue.isObject()) {
            const QJsonObject mcpCaps = mcpCapsValue.toObject();
            m_mcpStdioCapable = mcpCaps.value(u"stdio"_s).toBool(false);
            m_mcpHttpCapable = mcpCaps.value(u"http"_s).toBool(false);
        } else {
            m_mcpStdioCapable = true;
            m_mcpHttpCapable = false;
        }
        m_authMethods = result.value(u"authMethods"_s).toArray();
        m_authRetryDone = false;
        maybeAuthenticate(m_authMethods);
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

bool AcpClient::isAuthFailureError(const QString &error)
{
    const QString lowerError = error.toLower();
    return lowerError.contains(u"auth"_s) || lowerError.contains(u"login"_s)
        || lowerError.contains(u"credential"_s) || lowerError.contains(u"oauth"_s)
        || lowerError.contains(u"api key"_s) || lowerError.contains(u"unauthorized"_s)
        || lowerError.contains(u"required"_s);
}

void AcpClient::maybeAuthenticate(const QJsonArray &authMethods, bool force)
{
    if (authMethods.isEmpty()) {
        openSession();
        return;
    }

    // Most ACP agents — including modern `agy acp` — use their own CLI login
    // session silently during session/new when no API key is configured.
    // Only the legacy standalone `agy_acp_server` requires an upfront
    // authenticate call before session/new. If session/new or session/load
    // later rejects with an auth error, it retries here with force=true.
    const QString configuredKey = m_settings.acpApiKey.trimmed();
    const bool hasConfiguredKey = !configuredKey.isEmpty();
    const QProcessEnvironment env = processEnvironment();
    const QString keyEnvName = acpEffectiveApiKeyEnv(m_settings);
    const bool hasEnvKey = (!keyEnvName.isEmpty() && !env.value(keyEnvName).trimmed().isEmpty())
        || (acpAgentIsAntigravity(m_settings) && !env.value(u"GEMINI_API_KEY"_s).trimmed().isEmpty());
    const bool hasAnyApiKey = hasConfiguredKey || hasEnvKey;

    if (!force && !hasConfiguredKey && !isLaunchingLegacyAntigravityServer(m_settings)) {
        openSession();
        return;
    }

    const QString effectiveArgs = acpEffectiveArgs(m_settings);
    const bool codexAgent = m_settings.acpAgentId == u"codex-acp"_s
        || effectiveArgs.contains(u"@agentclientprotocol/codex-acp"_s);
    QString preferredMethod;
    if (codexAgent) {
        preferredMethod = u"chat-gpt"_s;
    } else if (acpAgentIsAntigravity(m_settings)) {
        // When an API key is available, select gemini-api-key; otherwise
        // select oauth-personal so Google OAuth runs instead of failing with
        // "GEMINI_API_KEY is not set".
        preferredMethod = hasAnyApiKey ? u"gemini-api-key"_s : u"oauth-personal"_s;
    }

    QJsonObject selectedMethod;
    QJsonObject fallbackMethod;
    auto methodIdOf = [](const QJsonObject &method) {
        QString id = method.value(u"id"_s).toString();
        if (id.isEmpty()) id = method.value(u"methodId"_s).toString();
        return id;
    };
    for (const QJsonValue &value : authMethods) {
        const QJsonObject method = value.toObject();
        const QString id = methodIdOf(method);
        if (fallbackMethod.isEmpty()) {
            fallbackMethod = method;
        }
        if (!preferredMethod.isEmpty() && id == preferredMethod) {
            selectedMethod = method;
            break;
        }
        if (selectedMethod.isEmpty()) {
            const QString type = method.value(u"type"_s).toString().toLower();
            const QString lowerId = id.toLower();
            const bool envLike = type == u"env"_s || lowerId.contains(u"api"_s)
                || lowerId.contains(u"key"_s) || lowerId.contains(u"env"_s)
                || lowerId.contains(u"token"_s);
            const bool interactive = type == u"agent"_s || type == u"terminal"_s
                || lowerId.contains(u"oauth"_s) || lowerId.contains(u"login"_s);
            if (hasAnyApiKey) {
                // Prefer a method that uses the configured key over an
                // interactive browser/terminal flow.
                if (envLike || !interactive) {
                    selectedMethod = method;
                }
            } else {
                // Without an API key, never choose an API-key method that will
                // immediately fail due to a missing key; pick an interactive
                // or OAuth method instead.
                if (!envLike) {
                    selectedMethod = method;
                }
            }
        }
    }
    if (selectedMethod.isEmpty()) {
        selectedMethod = fallbackMethod;
    }
    QString methodId = selectedMethod.value(u"id"_s).toString();
    if (methodId.isEmpty()) methodId = selectedMethod.value(u"methodId"_s).toString();
    if (methodId.isEmpty()) {
        openSession();
        return;
    }
    QJsonObject params;
    params.insert(u"methodId"_s, methodId);
    // Antigravity ACP (`agy acp`) accepts the API key in `_meta.apiKey` in
    // addition to GEMINI_API_KEY in the environment.
    const QString lowerMethodId = methodId.toLower();
    if (hasConfiguredKey
        && (lowerMethodId.contains(u"api"_s) || lowerMethodId.contains(u"key"_s)
            || lowerMethodId.contains(u"token"_s))) {
        params.insert(u"_meta"_s, QJsonObject{{u"apiKey"_s, configuredKey}});
    }
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
    }, acpAgentIsAntigravity(m_settings) ? 300000 : 30000);
}

void AcpClient::openSession()
{
    if (m_sessionOpening) {
        return;
    }
    m_sessionOpening = true;
    if (!m_resumeSessionId.isEmpty() && (m_loadSession || m_resumeSession)) {
        sendSessionLoadOrResume();
        return;
    }
    sendSessionNew();
}

void AcpClient::sendSessionNew()
{
    QJsonObject params;
    params.insert(u"cwd"_s, sessionCwd());
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
            // Some agents only discover that they need the ACP authenticate
            // request when session/new rejects them. Retry once through the
            // advertised methods instead of failing the turn.
            if (!m_authRetryDone && !m_authMethods.isEmpty() && isAuthFailureError(error)) {
                m_authRetryDone = true;
                m_sessionOpening = false;
                maybeAuthenticate(m_authMethods, true);
                return;
            }
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
    }, 0);
}

void AcpClient::sendSessionLoadOrResume()
{
    const QString method = m_resumeSession ? u"session/resume"_s : u"session/load"_s;
    m_replayingLoad = method == u"session/load"_s;
    QJsonObject params;
    params.insert(u"sessionId"_s, m_resumeSessionId);
    params.insert(u"cwd"_s, sessionCwd());
    params.insert(u"mcpServers"_s, mcpServersPayload());
    const QJsonObject meta = sessionMeta();
    if (!meta.isEmpty()) {
        params.insert(u"_meta"_s, meta);
    }
    sendRequest(method, params, [this, method](const QJsonObject &result, const QString &error) {
        if (m_shuttingDown) {
            return;
        }
        if (!error.isEmpty()) {
            m_replayingLoad = false;
            if (!m_authRetryDone && !m_authMethods.isEmpty() && isAuthFailureError(error)) {
                m_authRetryDone = true;
                m_sessionOpening = false;
                maybeAuthenticate(m_authMethods, true);
                return;
            }
            const QString lowerError = error.toLower();
            const bool resumeSuggestsLoad = method == u"session/resume"_s && m_loadSession
                && (lowerError.contains(u"session/load"_s)
                    || lowerError.contains(u"failed to read session"_s));
            const bool unknownSession = lowerError.contains(u"not found"_s)
                || lowerError.contains(u"unknown session"_s)
                || lowerError.contains(u"no such session"_s)
                || lowerError.contains(u"does not exist"_s);
            if (resumeSuggestsLoad || unknownSession) {
                if (method == u"session/resume"_s && m_loadSession) {
                    Q_EMIT logMessage(u"Session resume is unavailable; loading the saved session instead."_s);
                    m_resumeSession = false;
                    sendSessionLoadOrResume();
                    return;
                }
                Q_EMIT logMessage(u"The agent no longer has that session; starting a new one."_s);
                m_resumeSessionId.clear();
                sendSessionNew();
                return;
            }
            // A timeout or transient load failure must not silently start a
            // blank session while the transcript is full of old context.
            setState(State::Failed, error);
            Q_EMIT failed(error);
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
        auto drainQueue = [this]() {
            if (!m_queued.isEmpty() && m_promptId == 0) {
                const QueuedPrompt next = m_queued.takeFirst();
                sendPromptNow(next.text);
            }
        };
        if (m_replayingLoad) {
            // session/load streams the old conversation as session/update.
            // Give the tail of that replay a moment to arrive before live
            // output starts, so history is not painted as a new answer.
            const QString loadedSession = m_sessionId;
            QTimer::singleShot(150, this, [this, loadedSession, drainQueue]() {
                if (m_sessionId != loadedSession) {
                    return;
                }
                m_replayingLoad = false;
                drainQueue();
            });
        } else {
            m_replayingLoad = false;
            drainQueue();
        }
    }, 0);
}

void AcpClient::sendPromptNow(const QString &text)
{
    stopTurnRetry();
    // Approval-once is scoped to the turn that asked for it.
    m_onceGrants.clear();
    // Every turn starts with a full retry budget. executeTurnRetry() restores
    // the spent count after this reset, so automatic retries count against one
    // budget instead of resetting it.
    m_turnRetryAttempt = 0;
    m_promptText = text;
    m_currentText.clear();
    m_currentThinking.clear();
    m_currentPlan = QJsonArray();
    m_toolCalls = QJsonArray();
    // The live tools of the previous turn are gone, so an update that omits
    // its id must not be filed under one of them.
    m_lastToolCallId.clear();
    // Tools may still be open if the previous turn was cancelled by force (an
    // agent ignoring session/cancel keeps streaming updates until the grace
    // window closes). Close their cards rather than dropping them silently, or
    // they spin in the transcript for the rest of the session.
    finishLiveTools(u"The previous turn was stopped before this finished."_s);

    QJsonObject params;
    params.insert(u"sessionId"_s, m_sessionId);
    QJsonArray prompt = promptBlocks(text);
    const bool includeSessionNotes = !m_sessionNotesSent;
    if (includeSessionNotes) {
        prompt.prepend(sessionNotesBlock());
    }
    params.insert(u"prompt"_s, prompt);
    const int promptRequestId = sendRequest(u"session/prompt"_s, params, [this](const QJsonObject &result, const QString &error) {
        if (m_shuttingDown) {
            return;
        }
        if (!error.isEmpty()) {
            // A transient drop of the agent's stream to its model provider is
            // recoverable: the session survives, so ask the agent to continue
            // instead of failing the turn in front of the user.
            if (scheduleTurnRetry(error)) {
                return;
            }
            finishPrompt(QString(), error);
            return;
        }
        const QString stopReason = result.value(u"stopReason"_s).toString(u"end_turn"_s);
        // A "max_tokens" stop is not an error, but the turn is unfinished:
        // the agent's model ran out of output tokens mid-response — almost
        // always while generating one oversized tool call, like a whole large
        // file in a single write. Agents like Mistral Vibe then loop announcing
        // the write without ever completing it. Continue the turn with
        // steering to split the work instead of surfacing a dead turn.
        if (stopReason == u"max_tokens"_s && scheduleOutputLimitRetry()) {
            return;
        }
        finishPrompt(stopReason);
    }, 0);
    // sendRequest invokes the completion callback synchronously when the
    // write fails; that callback cleared m_promptId. Assigning its return
    // value unconditionally put the dead id back and wedged the client busy
    // forever.
    m_promptId = m_pending.contains(promptRequestId) ? promptRequestId : 0;
    if (m_promptId != 0 && includeSessionNotes) {
        m_sessionNotesSent = true;
    }
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

QJsonObject AcpClient::sessionNotesBlock() const
{
    QStringList lines;
    // Grok Build gets the rules through session _meta.rules, and agents that
    // advertise _meta.antigravity.systemPrompt.append get them through
    // _meta.systemPrompt.append (see sessionMeta()).
    if (!acpAgentIsGrok(m_settings) && !m_supportsSystemPromptAppend) {
        if (!m_settings.globalRules.trimmed().isEmpty()) {
            lines.append(u"Rules the user set for every workspace:"_s);
            lines.append(m_settings.globalRules.trimmed());
        }
        if (!m_settings.extraSystemPrompt.trimmed().isEmpty()) {
            lines.append(m_settings.extraSystemPrompt.trimmed());
        }
    }
    lines.append(u"Kate AI shows your tool calls in its transcript. Kate also offers these client methods, "
                 u"which you may call when the capability is advertised:"_s);
    lines.append(u"- _kateai/write_plan_file {content}: publish the plan of a multi-step task as markdown the user can follow."_s);
    lines.append(u"- _kateai/suggest {suggestion, path?, line?}: offer an improvement the user did not ask for, instead of making the change."_s);
    lines.append(u"- _kateai/ask_user {question, options?, allow_freeform?}: ask the user something and wait for the answer (the call blocks until they reply; the answer is the result)."_s);
    lines.append(u"- _kateai/todo_write {todos: [{content, status}]} / _kateai/todo_read: keep the user's task checklist current; status is pending, in_progress, or completed."_s);
    lines.append(u"- _kateai/exit_plan_mode: notify Kate AI to leave plan mode once the plan is ready."_s);
    lines.append(u"- _kateai/list_dir {path?} / _kateai/grep {pattern, path?, glob?, case_insensitive?, context?} / _kateai/glob {pattern, path?}: read-only workspace inspection respecting Kate's sandbox."_s);
    lines.append(u"- _kateai/edit_file {path, old_string, new_string} / _kateai/multi_edit_file {path, edits} / _kateai/delete_file {path} / _kateai/apply_patch {patch}: edit workspace files through Kate's open-buffer bridge and permission policy."_s);
    lines.append(u"- _kateai/query_project_graph {query_type, node_id?, relationship?, source_id?, target_id?}: read the project's files, symbols and imports."_s);
    lines.append(u"- _kateai/lsp_diagnostics {path?, severity?}: read language-server diagnostics (errors/warnings) for a file, or workspace-wide when path is omitted. When no LSP server is running, the call reports that instead."_s);
    lines.append(u"- _kateai/lsp_symbols {path}, _kateai/lsp_hover {path, line, column?}, _kateai/lsp_definition {path, line, column?, symbol?}, _kateai/lsp_references {path, line, column?, symbol?, include_declaration?}, _kateai/lsp_code_actions {path, line?, column?}: the same read-only language-server answers the editor tools give."_s);
    lines.append(u"- _kateai/debug_status {include_output?}, _kateai/debug_stack {thread_id?, max_frames?}, _kateai/debug_locals {frame_level?, filter?}, _kateai/debug_breakpoints, _kateai/debug_select_frame {frame_level}, _kateai/debug_select_thread {thread_id}, _kateai/debug_snapshot {action?, label?, first?, second?}: inspect Kate's live debugger (DAP/GDB/LLDB)."_s);
    lines.append(u"- _kateai/debug_set_breakpoint {path?, line?, condition?, hit_condition?, log_message?, remove?, clear_all?}, _kateai/debug_step {action, path?, line?, timeout_ms?}, _kateai/debug_eval {expression}, _kateai/debug_session {action, target?}, _kateai/debug_console {command}: control Kate's live debugger (permission-gated)."_s);
    lines.append(u"- _kateai/build_targets, _kateai/build_run {target?}, _kateai/terminal_run {command, working_dir?}: inspect/trigger Kate's Build plugin and embedded Konsole terminal."_s);
    if (m_projectGraph && m_projectGraph->getNodeCount() > 0) {
        const QString graph = m_projectGraph->contextSummary(40, 60, 2);
        if (!graph.trimmed().isEmpty()) {
            lines.append(u"\nProject graph summary (paths are workspace-relative; use _kateai/query_project_graph for details):"_s);
            lines.append(graph);
        }
    }
    QJsonObject block;
    block.insert(u"type"_s, u"text"_s);
    block.insert(u"text"_s, lines.join(u'\n'));
    return block;
}

QJsonObject AcpClient::sessionMeta() const
{
    QJsonObject meta;
    const QString agentId = m_settings.acpAgentId;
    const QString command = acpEffectiveCommand(m_settings);
    Q_UNUSED(agentId);
    Q_UNUSED(command);

    QStringList rules;
    if (!m_settings.globalRules.trimmed().isEmpty()) {
        rules.append(m_settings.globalRules.trimmed());
    }
    if (!m_settings.extraSystemPrompt.trimmed().isEmpty()) {
        rules.append(m_settings.extraSystemPrompt.trimmed());
    }

    // Grok Build reads yoloMode / autoMode / rules from session _meta.
    if (acpAgentIsGrok(m_settings)) {
        if (m_settings.permissionMode == PermissionMode::AlwaysApprove) {
            meta.insert(u"yoloMode"_s, true);
        } else if (m_settings.permissionMode == PermissionMode::AcceptEdits) {
            meta.insert(u"autoMode"_s, true);
        }
        if (!rules.isEmpty()) {
            meta.insert(u"rules"_s, rules.join(u"\n\n"_s));
        }
    }

    // Antigravity ACP (`agy acp`) advertises _meta.antigravity.systemPrompt.append
    // and appends `_meta.systemPrompt.append` directly to the agent's system prompt.
    if (m_supportsSystemPromptAppend && !rules.isEmpty()) {
        meta.insert(u"systemPrompt"_s, QJsonObject{{u"append"_s, rules.join(u"\n\n"_s)}});
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
            if (!m_mcpHttpCapable) {
                continue;
            }
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
            if (!m_mcpStdioCapable) {
                continue;
            }
            // ACP requires an absolute executable path. Kate's config is typed
            // by the user and normally holds "npx"/"uvx"/"node", so resolve it
            // the same way a shell would.
            QString command = server.command.trimmed();
            if (command.isEmpty()) {
                continue;
            }
            if (!QFileInfo(command).isAbsolute()) {
                if (command.contains(u'/') || command.contains(u'\\')) {
                    const QString candidate = QDir(m_workspace).absoluteFilePath(command);
                    if (QFileInfo(candidate).isFile() && QFileInfo(candidate).isExecutable()) {
                        command = candidate;
                    } else {
                        continue;
                    }
                } else {
                    const QString resolved = QStandardPaths::findExecutable(command);
                    if (resolved.isEmpty()) {
                        const QString tool = findExecutableInToolDirs(command);
                        if (tool.isEmpty()) {
                            qWarning() << "KateAI: MCP server" << server.name
                                       << "was not passed to the agent: command not found:" << server.command;
                            continue;
                        }
                        command = tool;
                    } else {
                        command = resolved;
                    }
                }
            }
            obj.insert(u"command"_s, command);
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
    if (m_shuttingDown || m_failingAll) {
        if (done) {
            done(QJsonObject(), u"The ACP client is shutting down; the request was not sent."_s);
        }
        return 0;
    }
    // int wraps after 2^31 and would collide with live pending ids.
    if (m_nextId <= 0 || m_nextId == std::numeric_limits<int>::max()) {
        m_nextId = 1;
        while (m_pending.contains(m_nextId) && m_nextId < std::numeric_limits<int>::max()) {
            ++m_nextId;
        }
    }
    while (m_pending.contains(m_nextId) && m_nextId < std::numeric_limits<int>::max()) {
        ++m_nextId;
    }
    const int id = m_nextId++;
    Pending pending;
    pending.id = id;
    pending.method = method;
    pending.done = std::move(done);
    if (timeoutMs > 0) {
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(timeoutMs);
        connect(timer, &QTimer::timeout, this, [this, id, method, timeoutMs, timer] {
            const auto it = m_pending.constFind(id);
            if (it == m_pending.constEnd()) {
                timer->deleteLater();
                return;
            }
            auto callback = it.value().done;
            m_pending.remove(id);
            timer->deleteLater();
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
    if (!writeMessage(message)) {
        // The request never reached the agent. Leaving it pending would wait
        // for an answer that cannot come - and session/prompt has no timeout,
        // so the turn would hang until the user stopped it.
        const auto it = m_pending.constFind(id);
        if (it != m_pending.constEnd()) {
            auto callback = it.value().done;
            QTimer *timer = it.value().timeout;
            m_pending.remove(id);
            if (timer) {
                timer->stop();
                timer->deleteLater();
            }
            if (callback) {
                callback(QJsonObject(), u"The ACP agent is not running, so the request could not be sent."_s);
            }
        }
        return id;
    }
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

bool AcpClient::writeMessage(const QJsonObject &message)
{
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return false;
    }
    QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    payload.append('\n');
    // QProcess::write() is allowed to accept fewer bytes than requested. A
    // short write used to leave a partial frame on the wire and treat the
    // request as failed; loop until the whole frame is queued.
    qsizetype written = 0;
    while (written < payload.size()) {
        const qint64 n = m_process->write(payload.constData() + written, payload.size() - written);
        if (n <= 0) {
            return false;
        }
        written += n;
    }
    return true;
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
    QPointer<AcpClient> self(this);
    const QByteArray output = m_process->readAllStandardOutput();
    for (qsizetype offset = 0; offset < output.size();) {
        if (!self || m_shuttingDown) {
            return;
        }
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
        if (!self || m_shuttingDown) {
            return;
        }
        if (m_stdoutBuffer.size() > MaxFrameBytes) {
            m_stdoutBuffer.clear();
            m_discardingStdoutFrame = true;
            reportOversizedFrame();
            return;
        }
    }
}

void AcpClient::reportOversizedFrame()
{
    if (m_oversizedFrameReported) {
        return;
    }
    m_oversizedFrameReported = true;
    // A frame this large cannot be parsed, so the answer it carried is lost.
    // Dropping it silently left the turn without an answer: the agent waits
    // for a response to session/prompt, and that request has no timeout, so
    // the chat sat on "Working on it…" forever. Reporting the failure closes
    // the turn with a reason instead.
    const QString message = QStringLiteral("The agent sent a message larger than the %1 MiB limit, so the turn cannot continue.")
                                .arg(MaxFrameBytes / (1024 * 1024));
    // failAll() settles the pending prompt, and its callback reports the
    // failure itself. Without a prompt in flight there is nobody to report it.
    const bool hadPrompt = m_promptId != 0;
    failAll(message);
    setState(State::Failed, message);
    if (!hadPrompt) {
        Q_EMIT failed(message);
    }
}

void AcpClient::handleBuffer()
{
    QPointer<AcpClient> self(this);
    // A UTF-8 BOM before the first JSON frame is not part of the JSON text.
    if (m_stdoutBuffer.startsWith("\xEF\xBB\xBF")) {
        m_stdoutBuffer.remove(0, 3);
    }
    int newline = m_stdoutBuffer.indexOf('\n');
    while (newline >= 0) {
        if (!self || m_shuttingDown) {
            return;
        }
        if (newline > MaxFrameBytes) {
            // A single line this long cannot be a protocol frame. It is
            // dropped, and the turn is failed: the frame it belonged to is
            // lost, and the agent would otherwise wait for an answer that
            // never comes.
            m_stdoutBuffer.remove(0, newline + 1);
            reportOversizedFrame();
            return;
        }
        QByteArray line = m_stdoutBuffer.left(newline);
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.isEmpty() && trimmed.startsWith('{')) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                handleMessage(doc.object());
                if (!self || m_shuttingDown) {
                    return;
                }
            } else {
                Q_EMIT logMessage(u"Ignored malformed ACP JSON-RPC frame: %1"_s.arg(parseError.errorString()));
            }
        }
        newline = m_stdoutBuffer.indexOf('\n');
    }
}

void AcpClient::handleMessage(const QJsonObject &message)
{
    const QJsonValue idValue = message.value(u"id"_s);
    const bool hasValidId = !idValue.isUndefined() && !idValue.isNull();
    if (message.contains(u"method"_s)) {
        if (hasValidId) {
            handleIncomingRequest(message);
        } else {
            handleNotification(message);
        }
        return;
    }
    if (hasValidId) {
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
        bool ok = false;
        const int parsed = idValue.toString().toInt(&ok);
        // "123abc" -> toInt() returns 123; only wholly numeric ids may route.
        if (!ok || QString::number(parsed) != idValue.toString().trimmed()) {
            return;
        }
        id = parsed;
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
        const QJsonValue errorValue = message.value(u"error"_s);
        QString messageText;
        QString details;
        if (errorValue.isObject()) {
            const QJsonObject error = errorValue.toObject();
            messageText = error.value(u"message"_s).toString();
            const QJsonValue data = error.value(u"data"_s);
            if (data.isObject()) {
                const QJsonObject dataObj = data.toObject();
                details = dataObj.value(u"details"_s).toString();
                if (details.isEmpty()) {
                    details = dataObj.value(u"message"_s).toString();
                }
                if (details.isEmpty() && dataObj.value(u"error"_s).isObject()) {
                    details = dataObj.value(u"error"_s).toObject().value(u"message"_s).toString();
                } else if (details.isEmpty() && dataObj.value(u"error"_s).isString()) {
                    details = dataObj.value(u"error"_s).toString();
                }
            } else if (data.isString()) {
                details = data.toString();
            }
        } else if (errorValue.isString()) {
            messageText = errorValue.toString();
        }
        if (messageText.isEmpty()) {
            messageText = u"ACP request failed."_s;
        }
        // Agents like MiniMax Code answer with a generic message ("Internal
        // error") and put the actual explanation in error.data.details. Without
        // it the user sees "Error: Internal error" and no way forward.
        if (!details.isEmpty() && !messageText.contains(details)) {
            messageText += u": %1"_s.arg(details);
        }
        pending.done(QJsonObject(), messageText);
        return;
    }
    pending.done(message.value(u"result"_s).toObject(), QString());
}

void AcpClient::handleIncomingRequest(const QJsonObject &message)
{
    const QString method = kateClientMethod(message.value(u"method"_s).toString());
    const QJsonValue id = message.value(u"id"_s);
    const QJsonObject params = message.value(u"params"_s).toObject();
    const QString requestSession = params.value(u"sessionId"_s).toString();
    if (!requestSession.isEmpty() && !m_sessionId.isEmpty() && requestSession != m_sessionId) {
        replyError(id, kInvalidParams, u"This request belongs to a session that is no longer active."_s);
        return;
    }

    if (method == u"session/request_permission"_s) {
        handleRequestPermission(id, params);
        return;
    }
    if (method == u"fs/read_text_file"_s || method == u"fs/read_file"_s
        || method == u"read_file"_s || method == u"read_text_file"_s || method == u"view_file"_s) {
        handleFsRead(id, params);
        return;
    }
    if (method == u"fs/write_text_file"_s || method == u"fs/write_file"_s
        || method == u"write_file"_s || method == u"write_text_file"_s || method == u"create_file"_s) {
        handleFsWrite(id, params);
        return;
    }
    if (method == u"fs/edit_file"_s || method == u"edit_file"_s) {
        handleFsEdit(id, params);
        return;
    }
    if (method == u"fs/multi_edit_file"_s || method == u"multi_edit_file"_s) {
        handleFsMultiEdit(id, params);
        return;
    }
    if (method == u"fs/delete_file"_s || method == u"fs/remove_file"_s || method == u"delete_file"_s) {
        handleFsDelete(id, params);
        return;
    }
    if (method == u"fs/list_directory"_s || method == u"list_dir"_s) {
        handleFsListDir(id, params);
        return;
    }
    if (method == u"fs/grep"_s || method == u"grep"_s) {
        handleFsGrep(id, params);
        return;
    }
    if (method == u"fs/glob"_s || method == u"glob"_s) {
        handleFsGlob(id, params);
        return;
    }
    if (method == u"apply_patch"_s) {
        handleApplyPatch(id, params);
        return;
    }
    if (method == u"exit_plan_mode"_s) {
        handleExitPlanMode(id, params);
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
    if (method == u"terminal/send_input"_s) {
        handleTerminalSendInput(id, params);
        return;
    }
    if (method == u"query_project_graph"_s) {
        handleQueryProjectGraph(id, params);
        return;
    }
    if (method == u"lsp_diagnostics"_s) {
        handleLspDiagnostics(id, params);
        return;
    }
    if (method == u"lsp_symbols"_s) {
        handleLspSymbols(id, params);
        return;
    }
    if (method == u"lsp_hover"_s) {
        handleLspHover(id, params);
        return;
    }
    if (method == u"lsp_definition"_s) {
        handleLspDefinition(id, params);
        return;
    }
    if (method == u"lsp_references"_s) {
        handleLspReferences(id, params);
        return;
    }
    if (method == u"lsp_code_actions"_s) {
        handleLspCodeActions(id, params);
        return;
    }
    if (method == u"debug_status"_s || method == u"debug_stack"_s || method == u"debug_locals"_s
        || method == u"debug_breakpoints"_s || method == u"debug_select_frame"_s
        || method == u"debug_select_thread"_s || method == u"debug_snapshot"_s
        || method == u"debug_set_breakpoint"_s || method == u"debug_step"_s
        || method == u"debug_eval"_s || method == u"debug_session"_s
        || method == u"debug_console"_s || method == u"build_targets"_s
        || method == u"build_run"_s || method == u"terminal_run"_s) {
        handleKateToolMethod(method, id, params);
        return;
    }
    if (method == u"ask_user"_s) {
        handleAskUser(id, params);
        return;
    }
    if (method == u"todo_write"_s) {
        handleTodoWrite(id, params);
        return;
    }
    if (method == u"todo_read"_s) {
        handleTodoRead(id, params);
        return;
    }
    // ACP v2 draft: elicitation is the standardized ask-the-user channel.
    if (method == u"elicitation/create"_s) {
        handleElicitationCreate(id, params);
        return;
    }
    replyError(id, kMethodNotFound, u"Method not found: %1"_s.arg(method));
}

void AcpClient::handleNotification(const QJsonObject &message)
{
    const QString method = message.value(u"method"_s).toString();
    const QJsonObject params = message.value(u"params"_s).toObject();
    if (m_replayingLoad) {
        // session/load replays history through session/update. Rendering it
        // again would duplicate the prior conversation in the live transcript.
        return;
    }
    const QString notificationSession = params.value(u"sessionId"_s).toString();
    if (!notificationSession.isEmpty() && !m_sessionId.isEmpty()
        && notificationSession != m_sessionId) {
        // A late update from a session that was replaced or closed.
        return;
    }
    if (method == u"session/update"_s || method == u"x.ai/session/update"_s) {
        QJsonObject update = params.value(u"update"_s).toObject();
        if (update.isEmpty()) {
            update = params;
        }
        handleSessionUpdate(update);
    } else if (method == u"config_option_update"_s) {
        ingestSessionModels(params, false);
    } else if (method == u"available_commands_update"_s) {
        // Some non-conforming agents send this as a top-level notification.
        // The protocol-defined form is a session/update variant, handled in
        // handleSessionUpdate(); keep this as a compatibility fallback.
        const QJsonArray commands = params.value(u"availableCommands"_s).toArray();
        setAvailableCommands(commands.isEmpty() ? params.value(u"commands"_s).toArray() : commands);
    }
}

void AcpClient::setAvailableCommands(const QJsonArray &commands)
{
    m_availableCommands = commands;
    Q_EMIT availableCommandsChanged(m_availableCommands);
    Q_EMIT logMessage(u"Agent advertised %1 slash command(s)."_s.arg(m_availableCommands.size()));
}

// A plan entry's content is an array of content blocks in the ACP spec, but
// agents in the wild also send a single block or a bare string (see
// toolContentText() for the same story on tool content). Every variant has to
// end up as the step's description, or the todo list renders blank rows.
static QString planEntryText(const QJsonValue &content)
{
    QStringList parts;
    const QJsonArray blocks = content.isArray() ? content.toArray() : QJsonArray{content};
    for (const QJsonValue &item : blocks) {
        const QString text = contentBlockText(item);
        if (!text.isEmpty()) {
            parts.append(text);
        }
    }
    return parts.join(u' ').trimmed();
}

void AcpClient::handleSessionUpdate(const QJsonObject &update)
{
    const QString kind = update.value(u"sessionUpdate"_s).toString();
    if (kind == u"config_option_update"_s || kind == u"available_models_update"_s || kind == u"current_model_update"_s) {
        ingestSessionModels(update, false);
        return;
    }
    if (kind == u"current_mode_update"_s) {
        ingestSessionModes(update);
        return;
    }
    if (kind == u"usage_update"_s) {
        const qint64 used = static_cast<qint64>(update.value(u"used"_s).toDouble(0));
        const qint64 size = static_cast<qint64>(update.value(u"size"_s).toDouble(0));
        if (used >= 0 && size >= 0) {
            m_contextUsedTokens = used;
            m_contextWindowSize = size;
            Q_EMIT usageUpdated(used, size);
        }
        return;
    }
    if (kind == u"session_info_update"_s || kind == u"user_message_chunk"_s) {
        return;
    }
    if (kind == u"available_commands_update"_s) {
        // Protocol-defined form: params.update.sessionUpdate names the variant
        // and availableCommands is the complete replacement list.
        setAvailableCommands(update.value(u"availableCommands"_s).toArray());
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
            const QString description = planEntryText(entry.value(u"content"_s));
            // An entry without any text carries no information; keeping it
            // would render a row of bare glyphs in the todo list.
            if (description.isEmpty()) {
                continue;
            }
            QJsonObject step;
            step.insert(u"id"_s, QString::number(++index));
            step.insert(u"description"_s, description);
            // The statuses are "pending", "in_progress" and "completed" in the
            // spec; agents also send spellings like "in-progress", "started"
            // or "done".
            const QString status = entry.value(u"status"_s).toString().toLower().replace(QChar(u'-'), QChar(u'_'));
            step.insert(u"completed"_s, status == u"completed"_s || status == u"done"_s);
            step.insert(u"inProgress"_s, status == u"in_progress"_s || status == u"started"_s);
            katePlan.append(step);
        }
        m_currentPlan = katePlan;
        Q_EMIT planUpdated(katePlan);
        return;
    }
    if (kind == u"tool_call"_s) {
        PermissionRequest request = permissionFromToolCall(update);
        if (request.toolCallId.isEmpty()) {
            // An agent that omits the id would key every later update under
            // "" and leak the entry in m_liveTools; give the call a local id.
            request.toolCallId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        constexpr int kMaxLiveTools = 2000;
        if (!m_liveTools.contains(request.toolCallId) && m_liveTools.size() >= kMaxLiveTools) {
            // A broken or hostile agent must not be able to grow the per-turn
            // live-tool maps without bound.
            return;
        }
        m_liveTools.insert(request.toolCallId, request);
        // Remembered so a later update that omits the id can be filed under
        // the call it belongs to.
        m_lastToolCallId = request.toolCallId;
        linkToolTerminals(request.toolCallId, update.value(u"content"_s));
        QJsonObject function;
        function.insert(u"name"_s, request.toolName);
        // Only a JSON object is a valid arguments payload: extracted plain
        // text (a content-block rawInput) would fail every parse downstream,
        // so it is not passed off as arguments.
        function.insert(u"arguments"_s, request.details.startsWith(u'{') ? request.details : u"{}"_s);
        QJsonObject call;
        call.insert(u"id"_s, request.toolCallId);
        call.insert(u"type"_s, u"function"_s);
        call.insert(u"function"_s, function);
        m_toolCalls.append(call);
        Q_EMIT toolStarted(request);
        const QString status = update.value(u"status"_s).toString();
        if (isTerminalToolStatus(status)) {
            const ToolResult result = completedToolResult(request.toolCallId, request.toolName, update,
                                                          m_toolStreamed.value(request.toolCallId));
            m_liveTools.remove(request.toolCallId);
            m_toolStreamed.remove(request.toolCallId);
            m_toolProgressAt.remove(request.toolCallId);
            forgetToolTerminal(request.toolCallId);
            Q_EMIT toolFinished(result);
        }
        return;
    }
    if (kind == u"tool_call_update"_s) {
        QString toolCallId = update.value(u"toolCallId"_s).toString();
        if (toolCallId.isEmpty()) {
            // An update without an id belongs to the call that was announced
            // last. Keying it under "" instead would file every id-less
            // update under the same entry, so the card would show the last
            // one and the others were lost.
            toolCallId = m_lastToolCallId;
            if (toolCallId.isEmpty()) {
                return;
            }
        }
        const QString status = update.value(u"status"_s).toString();
        PermissionRequest existing = m_liveTools.value(toolCallId);
        if (existing.toolCallId.isEmpty()) {
            constexpr int kMaxLiveTools = 2000;
            if (m_liveTools.size() >= kMaxLiveTools) {
                return;
            }
            existing = permissionFromToolCall(update);
            existing.toolCallId = toolCallId;
            m_liveTools.insert(toolCallId, existing);
            QJsonObject function;
            function.insert(u"name"_s, existing.toolName);
            function.insert(u"arguments"_s,
                            existing.details.startsWith(u'{') ? existing.details : u"{}"_s);
            QJsonObject call;
            call.insert(u"id"_s, toolCallId);
            call.insert(u"type"_s, u"function"_s);
            call.insert(u"function"_s, function);
            m_toolCalls.append(call);
            Q_EMIT toolStarted(existing);
        }
        // Agents may report a diff, a path or a terminal only in an update, not
        // in the initial tool_call. What the update adds is kept on the live
        // entry and announced, so the card and the review queue catch up.
        linkToolTerminals(toolCallId, update.value(u"content"_s));
        // Some agents reference the terminal by a top-level field instead of a
        // content block; link that too so its output streams into the card.
        if (!update.value(u"terminalId"_s).toString().isEmpty()) {
            linkToolTerminals(toolCallId, QJsonObject{{u"type"_s, u"terminal"_s}, {u"terminalId"_s, update.value(u"terminalId"_s)}});
        }
        const PermissionRequest refreshed = permissionFromToolCall(update);
        PermissionRequest merged = mergeToolCallDetails(existing, refreshed);
        const QString updatedTitle = update.value(u"title"_s).toString().trimmed();
        if (!updatedTitle.isEmpty() && updatedTitle != merged.summary) {
            merged.summary = updatedTitle;
        }
        if (merged.describeDiff != existing.describeDiff || merged.resolvedPath != existing.resolvedPath
            || merged.mutatesFiles != existing.mutatesFiles || merged.additionalDiffs != existing.additionalDiffs
            || merged.details != existing.details || merged.summary != existing.summary
            || merged.toolName != existing.toolName || merged.path != existing.path) {
            existing = merged;
            m_liveTools.insert(toolCallId, existing);
            // Keep the recorded tool call arguments up to date for transcript history.
            for (int i = m_toolCalls.size() - 1; i >= 0; --i) {
                QJsonObject callObj = m_toolCalls.at(i).toObject();
                if (callObj.value(u"id"_s).toString() == toolCallId) {
                    QJsonObject fn = callObj.value(u"function"_s).toObject();
                    fn.insert(u"name"_s, existing.toolName);
                    if (existing.details.startsWith(u'{')) {
                        fn.insert(u"arguments"_s, existing.details);
                    }
                    callObj.insert(u"function"_s, fn);
                    m_toolCalls.replace(i, callObj);
                    break;
                }
            }
            Q_EMIT toolDetailsChanged(existing);
        }
        if (isTerminalToolStatus(status)) {
            // Some ACP agents stream command output while the tool runs, then
            // send a terminal status update with no content at all. Keep that
            // last complete stream as the final card output.
            const ToolResult result = completedToolResult(toolCallId, existing.toolName, update,
                                                          m_toolStreamed.value(toolCallId));
            m_liveTools.remove(toolCallId);
            m_toolStreamed.remove(toolCallId);
            m_toolProgressAt.remove(toolCallId);
            forgetToolTerminal(toolCallId);
            Q_EMIT toolFinished(result);
            return;
        }
        // Still running: the agent is telling us what the tool has done so far.
        // The content array is the whole of what the agent knows, so the latest
        // text replaces whatever was streamed before, exactly like bashProgress
        // does for a native command.
        const QString streamed = toolContentText(update.value(u"content"_s));
        // Progress aggregation: some agents send delta chunks, others send
        // cumulative content. Detect and handle both.
        QString finalStreamed = streamed;
        const QString previous = m_toolStreamed.value(toolCallId);
        if (!previous.isEmpty() && !streamed.isEmpty()) {
            if (streamed.startsWith(previous)) {
                // Cumulative: use as-is
                finalStreamed = streamed;
            } else if (previous.endsWith(streamed) || previous.contains(streamed)) {
                // Already included: keep previous
                finalStreamed = previous;
            } else if (streamed.length() < previous.length() && !previous.contains(streamed)) {
                // Possible delta: append
                finalStreamed = previous + streamed;
            } else {
                finalStreamed = streamed;
            }
        }
        if (finalStreamed.isEmpty()) {
            // Some agents only refresh the title mid-flight ("Running tests…").
            // Keep the card's summary current so the header never lies, but
            // there is nothing to stream into the body yet. Announce the new
            // title too: mutating the entry silently left the card header
            // stale until the call finished.
            const QString title = update.value(u"title"_s).toString();
            if (!title.isEmpty() && title != existing.summary) {
                existing.summary = title;
                m_liveTools.insert(toolCallId, existing);
                Q_EMIT toolDetailsChanged(existing);
            }
            return;
        }
        // An agent that repeats the same content word for word must not force a
        // re-render of the whole stream on every heartbeat.
        const QString capped = capToolStream(finalStreamed);
        if (m_toolStreamed.value(toolCallId) == capped) {
            return;
        }
        m_toolStreamed.insert(toolCallId, capped);
        // A multi-megabyte card update every few milliseconds is expensive even
        // after ChatWidget coalescing. Keep the stored text current but rate
        // limit large progress emissions.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (capped.size() > 64 * 1024 && now - m_toolProgressAt.value(toolCallId) < 100) {
            return;
        }
        m_toolProgressAt.insert(toolCallId, now);
        Q_EMIT toolProgress(toolCallId, capped);
        return;
    }
}

PermissionRequest AcpClient::permissionFromToolCall(const QJsonObject &toolCall) const
{
    PermissionRequest request;
    request.toolCallId = toolCall.value(u"toolCallId"_s).toString();
    const QString rawName = toolCall.value(u"name"_s).toString();
    request.originalToolName = rawName.isEmpty() ? toolCall.value(u"kind"_s).toString() : rawName;
    request.toolName = request.originalToolName;
    if (request.toolName.isEmpty()) {
        request.toolName = u"tool"_s;
    }
    // Normalize ACP/varied tool names to canonical KateAI names
    request.toolName = canonicalToolName(request.toolName);
    const QString title = toolCall.value(u"title"_s).toString().trimmed();
    // Registry agents keep inventing names for the same two UI-only calls.
    // The alias map catches the common ones; the title is the last hint when
    // the name itself is unknown ("other", "tool", ...).
    if (request.toolName == request.originalToolName) {
        const QString probe = (request.originalToolName + u' ' + title).toLower();
        if (probe.contains(u"suggest"_s) || probe.contains(u"propose"_s) || probe.contains(u"advice"_s)) {
            request.toolName = u"suggest"_s;
        } else if (probe.contains(u"question"_s)
                   || (probe.contains(u"ask"_s)
                       && (probe.contains(u"user"_s) || probe.contains(u"human"_s)))) {
            request.toolName = u"question"_s;
        }
    }
    request.summary = title;
    if (request.summary.isEmpty()) {
        request.summary = request.originalToolName.isEmpty() ? request.toolName : request.originalToolName;
    }
    const QString kind = toolCall.value(u"kind"_s).toString();
    request.risk = riskForKind(kind, request.toolName);
    // These two tools only drive Kate's UI. A registry agent often labels them
    // "other", which riskForKind() maps to Execute and would turn into a
    // permission prompt; they must stay Read.
    if (request.toolName == u"question"_s || request.toolName == u"suggest"_s) {
        request.risk = ToolRisk::Read;
    }
    // Every agent names its own tools, so the edit tracker cannot rely on
    // tool names here. The ACP kind is the protocol-level statement of what
    // the call does, and a diff content block proves the agent edited text.
    request.mutatesFiles = kind == u"edit"_s || kind == u"write"_s || kind == u"delete"_s || kind == u"move"_s;

    // --- Locations: the files the agent says the call touches. The first one
    // is the primary file until a diff names another; they are listed after
    // the diffs (see below).
    QStringList locationPaths;
    const QJsonArray locations = toolCall.value(u"locations"_s).toArray();
    for (const QJsonValue &loc : locations) {
        const QJsonObject locObj = loc.toObject();
        QString p = locObj.value(u"path"_s).toString();
        if (p.isEmpty()) p = locObj.value(u"filePath"_s).toString();
        if (p.isEmpty()) p = locObj.value(u"file"_s).toString();
        if (p.isEmpty()) p = locObj.value(u"uri"_s).toString();
        if (p.startsWith(u"file:"_s, Qt::CaseInsensitive)) {
            const QUrl url(p);
            if (url.isLocalFile()) p = url.toLocalFile();
        }
        if (!p.isEmpty() && !locationPaths.contains(p)) {
            locationPaths.append(p);
        }
    }
    if (!locationPaths.isEmpty()) {
        request.path = locationPaths.first();
    }

    // --- Raw input normalization ---
    const QJsonValue rawInput = toolCall.value(u"rawInput"_s);
    QJsonObject normalizedArgs;
    if (rawInput.isObject()) {
        normalizedArgs = rawInput.toObject();
        request.details = jsonText(rawInput);
    } else if (rawInput.isArray()) {
        // Could be content blocks or args array
        const QJsonArray arr = rawInput.toArray();
        // Try to interpret as content blocks
        bool isContent = false;
        for (const QJsonValue &v : arr) {
            if (v.isObject() && v.toObject().contains(u"type"_s)) { isContent = true; break; }
        }
        if (isContent) {
            request.details = toolContentText(rawInput);
        } else {
            // Try to parse each as JSON object and merge
            QJsonObject merged;
            for (const QJsonValue &v : arr) {
                if (v.isObject()) {
                    const QJsonObject o = v.toObject();
                    for (auto it = o.begin(); it != o.end(); ++it) merged.insert(it.key(), it.value());
                }
            }
            if (!merged.isEmpty()) {
                normalizedArgs = merged;
                request.details = jsonText(merged);
            } else {
                request.details = toolContentText(rawInput);
            }
        }
    } else if (rawInput.isString()) {
        const QString s = rawInput.toString();
        const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8());
        if (doc.isObject()) {
            normalizedArgs = doc.object();
            request.details = jsonText(normalizedArgs);
        } else if (doc.isArray()) {
            request.details = toolContentText(doc.array());
        } else {
            request.details = s;
        }
    } else if (rawInput.isDouble() || rawInput.isBool()) {
        request.details = jsonText(rawInput);
    } else {
        request.details = QString();
    }

    // --- Content blocks (diffs, text) ---
    // Every diff the call carries, with the file it edits. The pairs are kept
    // together, so a diff is never attributed to another file.
    struct DiffPart {
        QString path;
        QString oldText;
        QString newText;
        QString diff;
    };
    QList<DiffPart> diffs;
    auto addDiff = [&diffs, &request](const QJsonObject &obj) {
        QString p = obj.value(u"path"_s).toString();
        if (p.isEmpty()) p = obj.value(u"filePath"_s).toString();
        if (p.isEmpty()) p = obj.value(u"file"_s).toString();
        if (p.isEmpty()) p = obj.value(u"uri"_s).toString();
        if (p.startsWith(u"file://"_s)) p = QUrl(p).toLocalFile();
        if (p.isEmpty()) p = request.path;
        QString oldText = obj.value(u"oldText"_s).toString();
        if (oldText.isEmpty()) oldText = obj.value(u"old_text"_s).toString();
        QString newText = obj.value(u"newText"_s).toString();
        if (newText.isEmpty()) newText = obj.value(u"new_text"_s).toString();
        diffs.append({p, oldText, newText, unifiedDiff(p, oldText, newText)});
    };
    const QJsonArray content = toolCall.value(u"content"_s).toArray();
    // If content is a single object (not array), wrap it
    QJsonArray contentArr = content;
    if (contentArr.isEmpty() && toolCall.value(u"content"_s).isObject()) {
        contentArr = QJsonArray{toolCall.value(u"content"_s)};
    }
    QStringList contentTexts;
    for (const QJsonValue &item : contentArr) {
        const QJsonObject obj = item.toObject();
        if (obj.isEmpty()) continue;
        const QString type = obj.value(u"type"_s).toString();
        if (type == u"diff"_s) {
            addDiff(obj);
        } else if (type == u"content"_s || type == u"text"_s || type.isEmpty()) {
            const QString t = contentBlockText(obj.contains(u"content"_s) ? obj.value(u"content"_s) : item);
            if (!t.isEmpty()) {
                contentTexts.append(t);
            }
            // Try to extract text for details if empty
            if (request.details.isEmpty() && !t.isEmpty()) {
                request.details = t;
            }
        }
    }
    const QString contentText = contentTexts.join(u'\n');
    // Some agents send their diffs in a separate contents list.
    for (const QJsonValue &c : toolCall.value(u"contents"_s).toArray()) {
        const QJsonObject co = c.toObject();
        if (co.value(u"type"_s).toString() == u"diff"_s) {
            addDiff(co);
        }
    }

    // Question and suggestion tools are UI-only, but every agent names and
    // shapes them differently. Normalize the arguments once here so the card,
    // the history, and the transcript rebuild all see the same canonical
    // `question`/`options`/`suggestion`/`path`/`line` fields. This runs after
    // content blocks so a title-only tool_call whose real text lives in a
    // content block still gets it.
    if (request.toolName == u"question"_s || request.toolName == u"suggest"_s) {
        QJsonObject semanticArgs = normalizedArgs;
        if (semanticArgs.isEmpty() && !request.details.trimmed().isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(request.details.toUtf8());
            if (doc.isObject()) {
                semanticArgs = doc.object();
            } else {
                // The agent sent the advice/question as plain text rather than
                // JSON; keep it as the text field so the card is not blank.
                semanticArgs.insert(u"text"_s, request.details);
            }
        }
        if (!contentText.isEmpty() && !semanticArgs.contains(u"text"_s)) {
            semanticArgs.insert(u"text"_s, contentText);
        }
        if (request.toolName == u"question"_s) {
            normalizeQuestionMetadata(request, &semanticArgs, title);
        } else {
            normalizeSuggestionMetadata(request, &semanticArgs, title);
        }
        if (!semanticArgs.isEmpty()) {
            normalizedArgs = semanticArgs;
            request.details = jsonText(semanticArgs);
        }
    }

    // apply_patch-style calls carry one unified diff as plain text, often in a
    // text content block or in rawInput. Parse it into per-file diffs so a
    // multi-file patch renders one card entry per file instead of one blob.
    if (diffs.isEmpty()) {
        QString patchText;
        for (const QJsonValue &item : contentArr) {
            const QJsonObject obj = item.toObject();
            const QString type = obj.value(u"type"_s).toString();
            if (type == u"text"_s || type == u"content"_s || type.isEmpty()) {
                const QString t = contentBlockText(obj.contains(u"content"_s) ? obj.value(u"content"_s) : item);
                if (t.contains(u"\n--- "_s) || t.startsWith(u"--- "_s)) {
                    patchText = t;
                    break;
                }
            }
        }
        if (patchText.isEmpty() && (request.details.contains(u"\n--- "_s) || request.details.startsWith(u"--- "_s))) {
            patchText = request.details;
        }
        if (!patchText.isEmpty()) {
            const QList<PatchFile> files = parseUnifiedPatch(patchText);
            for (const PatchFile &file : files) {
                if (file.path.isEmpty() || file.hunks.isEmpty()) {
                    continue;
                }
                QStringList hunkText;
                for (const PatchHunk &hunk : file.hunks) {
                    hunkText.append(u"@@ -%1,%2 +%3,%4 @@"_s.arg(hunk.oldStart).arg(hunk.oldCount).arg(hunk.newStart).arg(hunk.newCount));
                    hunkText.append(hunk.lines);
                }
                const QString diff = u"--- a/%1\n+++ b/%2\n"_s.arg(file.path, file.path) + hunkText.join(u'\n');
                diffs.append({file.path, QString(), QString(), diff});
            }
        }
    }

    // The primary diff is the one for the primary file. When no diff names
    // that file, the first diff's file becomes the primary one.
    auto absolutePath = [this](const QString &p) {
        if (QFileInfo(p).isAbsolute() || m_workspace.isEmpty()) {
            return QDir::cleanPath(p);
        }
        return QDir::cleanPath(QDir(m_workspace).absoluteFilePath(p));
    };
    int primary = -1;
    if (!diffs.isEmpty()) {
        for (int i = 0; i < diffs.size() && primary < 0; ++i) {
            if (!request.path.isEmpty() && absolutePath(diffs.at(i).path) == absolutePath(request.path)) {
                primary = i;
            }
        }
        if (primary < 0) {
            primary = 0;
            request.path = diffs.first().path;
        }
        request.describeDiff = diffs.at(primary).diff;
        request.mutatesFiles = true;
        for (int i = 0; i < diffs.size(); ++i) {
            if (i != primary) {
                request.additionalPaths.append(diffs.at(i).path);
                request.additionalDiffs.append(diffs.at(i).diff);
            }
        }
    }
    // Files a location names without a diff follow the diffs. That keeps
    // additionalPaths and additionalDiffs aligned by index.
    for (const QString &p : std::as_const(locationPaths)) {
        if (absolutePath(p) != absolutePath(request.path) && !request.additionalPaths.contains(p)) {
            request.additionalPaths.append(p);
        }
    }

    // --- Path resolution from args ---
    auto tryPath = [&](const QJsonObject &o, const QString &key) {
        if (request.path.isEmpty()) {
            QString p = o.value(key).toString();
            if (p.startsWith(u"file:"_s, Qt::CaseInsensitive)) {
                const QUrl url(p);
                if (url.isLocalFile()) {
                    p = url.toLocalFile();
                }
            }
            if (!p.isEmpty()) request.path = p;
        }
    };
    if (request.path.isEmpty() && !normalizedArgs.isEmpty()) {
        tryPath(normalizedArgs, u"path"_s);
        tryPath(normalizedArgs, u"Path"_s);
        tryPath(normalizedArgs, u"file_path"_s);
        tryPath(normalizedArgs, u"filePath"_s);
        tryPath(normalizedArgs, u"file"_s);
        tryPath(normalizedArgs, u"File"_s);
        tryPath(normalizedArgs, u"uri"_s);
        tryPath(normalizedArgs, u"target"_s);
        tryPath(normalizedArgs, u"target_file"_s);
        tryPath(normalizedArgs, u"targetFile"_s);
        tryPath(normalizedArgs, u"TargetFile"_s);
        tryPath(normalizedArgs, u"absolute_path"_s);
        tryPath(normalizedArgs, u"AbsolutePath"_s);
        tryPath(normalizedArgs, u"DirectoryPath"_s);
        tryPath(normalizedArgs, u"SearchPath"_s);
        tryPath(normalizedArgs, u"SearchDirectory"_s);
    }
    // Resolve relative paths against workspace
    if (!request.path.isEmpty() && !QFileInfo(request.path).isAbsolute() && !m_workspace.isEmpty()) {
        request.resolvedPath = QDir(m_workspace).absoluteFilePath(request.path);
    } else {
        request.resolvedPath = request.path;
    }

    // --- Args-based diff extraction ---
    QString rawText = request.details;
    if (request.describeDiff.isEmpty() || request.path.isEmpty()) {
        QJsonObject args = normalizedArgs;
        if (args.isEmpty() && !rawText.isEmpty()) {
            ToolCall tempCall;
            tempCall.argumentsJson = rawText;
            QString parseError;
            args = parseToolArgs(tempCall, &parseError);
        }
        if (!args.isEmpty()) {
            if (request.path.isEmpty()) {
                QString p = firstStringField(
                    args, {u"path"_s, u"Path"_s, u"file_path"_s, u"filePath"_s, u"file"_s, u"File"_s,
                           u"target_file"_s, u"targetFile"_s, u"TargetFile"_s,
                           u"absolute_path"_s, u"AbsolutePath"_s, u"uri"_s, u"target"_s,
                           u"DirectoryPath"_s, u"SearchPath"_s, u"SearchDirectory"_s});
                if (p.startsWith(u"file:"_s, Qt::CaseInsensitive)) {
                    const QUrl url(p);
                    if (url.isLocalFile()) {
                        p = url.toLocalFile();
                    }
                }
                if (!p.isEmpty()) {
                    request.path = p;
                    request.resolvedPath = (!QFileInfo(p).isAbsolute() && !m_workspace.isEmpty())
                        ? QDir(m_workspace).absoluteFilePath(p)
                        : p;
                }
            }
            QString oldText = args.value(u"old_string"_s).toString();
            if (oldText.isEmpty()) oldText = args.value(u"oldText"_s).toString();
            if (oldText.isEmpty()) oldText = args.value(u"old"_s).toString();
            if (oldText.isEmpty()) oldText = args.value(u"Target"_s).toString();
            if (oldText.isEmpty()) oldText = args.value(u"TargetContent"_s).toString();
            QString newText = args.value(u"new_string"_s).toString();
            if (newText.isEmpty()) newText = args.value(u"newText"_s).toString();
            if (newText.isEmpty()) newText = args.value(u"new"_s).toString();
            if (newText.isEmpty()) newText = args.value(u"Replacement"_s).toString();
            if (newText.isEmpty()) newText = args.value(u"ReplacementContent"_s).toString();
            if (request.describeDiff.isEmpty() && (!oldText.isEmpty() || !newText.isEmpty())) {
                request.describeDiff = unifiedDiff(request.path, oldText, newText);
                if (!request.path.isEmpty()) request.mutatesFiles = true;
            }
            if (request.describeDiff.isEmpty()) {
                QJsonArray chunks = args.value(u"ReplacementChunks"_s).toArray();
                if (chunks.isEmpty()) chunks = args.value(u"edits"_s).toArray();
                if (chunks.isEmpty()) chunks = args.value(u"chunks"_s).toArray();
                if (!chunks.isEmpty()) {
                    QStringList chunkDiffs;
                    for (const QJsonValue &chunkVal : chunks) {
                        const QJsonObject chunk = chunkVal.toObject();
                        QString cOld = chunk.value(u"old_string"_s).toString();
                        if (cOld.isEmpty()) cOld = chunk.value(u"oldText"_s).toString();
                        if (cOld.isEmpty()) cOld = chunk.value(u"TargetContent"_s).toString();
                        if (cOld.isEmpty()) cOld = chunk.value(u"Target"_s).toString();
                        QString cNew = chunk.value(u"new_string"_s).toString();
                        if (cNew.isEmpty()) cNew = chunk.value(u"newText"_s).toString();
                        if (cNew.isEmpty()) cNew = chunk.value(u"ReplacementContent"_s).toString();
                        if (cNew.isEmpty()) cNew = chunk.value(u"Replacement"_s).toString();
                        if (!cOld.isEmpty() || !cNew.isEmpty()) {
                            chunkDiffs.append(unifiedDiff(request.path, cOld, cNew));
                        }
                    }
                    if (!chunkDiffs.isEmpty()) {
                        request.describeDiff = chunkDiffs.join(u"\n"_s);
                        if (!request.path.isEmpty()) request.mutatesFiles = true;
                    }
                }
            }
            if (request.describeDiff.isEmpty()) {
                QString content = args.value(u"content"_s).toString();
                if (content.isEmpty()) content = args.value(u"contents"_s).toString();
                if (content.isEmpty()) content = args.value(u"Content"_s).toString();
                if (content.isEmpty()) content = args.value(u"CodeContent"_s).toString();
                if (!content.isEmpty() && !request.path.isEmpty()
                    && (request.toolName.trimmed().toLower().contains(u"write"_s)
                        || kind == u"write"_s)) {
                    request.describeDiff = unifiedDiff(request.path, QString(), content);
                    request.mutatesFiles = true;
                }
            }
        }
    }

    // Risk re-evaluation based on canonical name. A file mutation raises a Read
    // risk to Write, but it must never lower an Execute risk: AcceptEdits
    // auto-approves Write, so a command-running tool that also carries a diff
    // block would otherwise run without a prompt, undoing the fail-safe in
    // riskForKind().
    if (request.toolName.contains(u"bash"_s) || request.toolName == u"bash"_s) {
        request.risk = ToolRisk::Execute;
    } else if (request.mutatesFiles && request.risk == ToolRisk::Read) {
        request.risk = ToolRisk::Write;
    }

    // A diff that arrived as a content block is not in the arguments the agent
    // sent, and a restored transcript rebuilds the card from those arguments.
    // Record the diff in the shape the edit tools use, so history restores the
    // same card as the live one.
    if (primary >= 0) {
        QJsonObject args = normalizedArgs;
        if (!args.contains(u"old_string"_s) && !args.contains(u"oldText"_s) && !args.contains(u"content"_s)) {
            const DiffPart &part = diffs.at(primary);
            if (part.oldText.isEmpty()) {
                args.insert(u"content"_s, part.newText);
            } else {
                args.insert(u"old_string"_s, part.oldText);
                args.insert(u"new_string"_s, part.newText);
            }
        }
        if (!args.contains(u"path"_s) && !request.path.isEmpty()) {
            args.insert(u"path"_s, request.path);
        }
        request.details = jsonText(args);
    }

    // Ensure details is valid JSON if possible
    if (request.details.isEmpty() && !normalizedArgs.isEmpty()) {
        request.details = jsonText(normalizedArgs);
    }

    return request;
}

PermissionRequest AcpClient::mergeToolCallDetails(const PermissionRequest &base, const PermissionRequest &other)
{
    PermissionRequest merged = base;
    if (merged.originalToolName.isEmpty() && !other.originalToolName.isEmpty()) {
        // A permission request often repeats only the toolCallId. Without this
        // the call would be keyed under the placeholder name "tool", and an
        // "always allow" would be granted to that name instead of the tool.
        merged.originalToolName = other.originalToolName;
        merged.toolName = other.toolName;
        merged.risk = other.risk;
    }
    if (merged.toolCallId.isEmpty()) {
        merged.toolCallId = other.toolCallId;
    }
    if (merged.summary.isEmpty() || merged.summary == merged.toolName || merged.summary == merged.originalToolName) {
        if (!other.summary.isEmpty()) {
            merged.summary = other.summary;
        }
    }
    if (merged.details.isEmpty()) {
        merged.details = other.details;
    }
    if (merged.path.isEmpty()) {
        merged.path = other.path;
    }
    if (merged.resolvedPath.isEmpty()) {
        merged.resolvedPath = other.resolvedPath;
    }
    if (merged.describeDiff.isEmpty() && !other.describeDiff.isEmpty()) {
        // The diffs travel together: the primary one and its additional files.
        merged.describeDiff = other.describeDiff;
        merged.additionalDiffs = other.additionalDiffs;
        merged.additionalPaths = other.additionalPaths;
    }
    for (const QString &p : other.additionalPaths) {
        if (p != merged.path && !merged.additionalPaths.contains(p)) {
            merged.additionalPaths.append(p);
        }
    }
    merged.mutatesFiles = merged.mutatesFiles || other.mutatesFiles;
    return merged;
}

void AcpClient::handleRequestPermission(const QJsonValue &id, const QJsonObject &params)
{
    const QJsonObject toolCall = params.value(u"toolCall"_s).toObject();
    PermissionRequest request = permissionFromToolCall(toolCall);
    // The request usually repeats only part of the tool call the agent has
    // already announced. The announced call is the authority on what it is.
    const PermissionRequest live = m_liveTools.value(request.toolCallId);
    if (!live.toolCallId.isEmpty()) {
        request = mergeToolCallDetails(live, request);
    }
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

    // Some agents without a dedicated ask-user method tunnel their question
    // through session/request_permission: the options are answer labels, not
    // allow/reject decisions. Present those as a question card; selecting a
    // chip responds with its optionId.
    bool looksLikePermission = false;
    for (const QJsonValue &value : std::as_const(m_permissionOptions)) {
        const QString kind = value.toObject().value(u"kind"_s).toString();
        if (kind.startsWith(u"allow"_s) || kind.startsWith(u"reject"_s)) {
            looksLikePermission = true;
            break;
        }
    }
    const QStringList answerChoices = optionLabels(m_permissionOptions);
    if (request.toolName == u"question"_s && !looksLikePermission && !answerChoices.isEmpty()) {
        m_permissionQuestion = true;
        QString text = request.questionText;
        if (text.isEmpty()) {
            text = request.summary;
        }
        if (text.isEmpty() || text == request.originalToolName || text == request.toolName) {
            text = u"The agent is waiting for your answer."_s;
        }
        request.questionText = text;
        request.questionOptions = answerChoices;
        request.allowFreeform = false;
        request.risk = ToolRisk::Read;
        request.summary = text.left(200);
        m_permissionRequest = request;
        Q_EMIT questionAsked(request);
        return;
    }

    // Whether the request is answered by the user or auto-decided is the
    // AgentLoop's call: it owns the permission policy for every provider.
    Q_EMIT permissionNeeded(request);
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

QString AcpClient::writeGrantKey(const QString &path) const
{
    // fs/write_text_file paths are absolute (pathAllowed enforces it); the
    // approval side resolves relative paths against the workspace. Both sides
    // must agree on one spelling or the grant never matches.
    if (QFileInfo(path).isAbsolute() || m_workspace.isEmpty()) {
        return u"write:"_s + QDir::cleanPath(QDir::fromNativeSeparators(path));
    }
    return u"write:"_s + QDir::cleanPath(QDir(m_workspace).absoluteFilePath(path));
}

QString AcpClient::execGrantKey(const QString &command, const QStringList &args) const
{
    // The command line as one string. terminal/create accepts the command as a
    // string or an argv array plus an args list; the approval side normalises
    // whichever form the agent used to the same key.
    return u"exec:"_s + command + u'\n' + args.join(u'\n');
}

bool AcpClient::ensureMethodPermission(const QString &grantKey, PermissionRequest request,
                                       const QJsonValue &rpcId, const QString &method, const QJsonObject &params)
{
    if (m_sessionGrants.contains(grantKey)) {
        return true;
    }
    if (m_onceGrants.remove(grantKey)) {
        return true;
    }
    // An agent can issue these methods far faster than a user can approve
    // them. Bound the queue so a misbehaving agent cannot grow it without
    // limit; the refused call gets a normal error response.
    constexpr int kMaxParkedMethods = 64;
    constexpr qint64 kMaxParkedBytes = 32 * 1024 * 1024;
    if (m_parkedMethods.size() >= kMaxParkedMethods) {
        replyError(rpcId, kServerError, u"Too many permission requests are queued; retry after they settle."_s);
        return false;
    }
    const qint64 incomingBytes = QJsonDocument(params).toJson(QJsonDocument::Compact).size();
    qint64 parkedBytes = incomingBytes;
    for (const ParkedMethod &queued : std::as_const(m_parkedMethods)) {
        parkedBytes += QJsonDocument(queued.params).toJson(QJsonDocument::Compact).size();
    }
    if (incomingBytes > 8 * 1024 * 1024 || parkedBytes > kMaxParkedBytes) {
        replyError(rpcId, kServerError, u"The queued permission payload is too large."_s);
        return false;
    }
    ParkedMethod parked;
    parked.rpcId = rpcId;
    parked.method = method;
    parked.params = params;
    parked.grantKey = grantKey;
    parked.request = std::move(request);
    m_parkedMethods.append(std::move(parked));
    askNextParkedMethod();
    return false;
}

void AcpClient::askNextParkedMethod()
{
    // One permission question at a time: a parked method is asked only when no
    // agent-initiated request, no pending question, and no other parked method
    // is being answered.
    if (m_activeParked || hasPendingPermission() || hasPendingQuestion() || m_parkedMethods.isEmpty()) {
        return;
    }
    m_activeParked = m_parkedMethods.takeFirst();
    // Copy the request before emitting: a synchronous permission handler (the
    // tests, and any UI that answers immediately) resolves the request from
    // inside permissionNeeded(), which resets m_activeParked. Dereferencing the
    // optional afterwards would crash on the empty value.
    const PermissionRequest request = m_activeParked->request;
    // Tell the transcript the call is parked, not dead: without a progress
    // line an agent waiting on fs/write_text_file looks like a stuck card.
    Q_EMIT toolProgress(request.toolCallId, u"Waiting for approval: %1"_s.arg(request.summary));
    Q_EMIT permissionNeeded(request);
}

void AcpClient::dispatchParkedMethod(const ParkedMethod &parked)
{
    // Re-enters the handler with the grant recorded, so the permission check
    // passes and the call runs (or fails on its own merits).
    if (parked.method == u"fs/write_text_file"_s) {
        handleFsWrite(parked.rpcId, parked.params);
    } else if (parked.method == u"fs/edit_file"_s) {
        handleFsEdit(parked.rpcId, parked.params);
    } else if (parked.method == u"fs/multi_edit_file"_s) {
        handleFsMultiEdit(parked.rpcId, parked.params);
    } else if (parked.method == u"fs/delete_file"_s) {
        handleFsDelete(parked.rpcId, parked.params);
    } else if (parked.method == u"apply_patch"_s) {
        handleApplyPatch(parked.rpcId, parked.params);
    } else if (parked.method == u"terminal/create"_s) {
        handleTerminalCreate(parked.rpcId, parked.params);
    } else if (parked.method == u"debug_set_breakpoint"_s || parked.method == u"debug_step"_s
               || parked.method == u"debug_eval"_s || parked.method == u"debug_session"_s
               || parked.method == u"debug_console"_s || parked.method == u"build_run"_s
               || parked.method == u"terminal_run"_s) {
        handleKateToolMethod(parked.method, parked.rpcId, parked.params);
    }
}

void AcpClient::cancelParkedMethods()
{
    const auto refuse = [this](const ParkedMethod &parked) {
        replyError(parked.rpcId, kServerError, u"Cancelled before it was approved."_s);
        ToolResult result;
        result.toolCallId = parked.request.toolCallId;
        result.name = parked.request.toolName;
        result.ok = false;
        result.cancelled = true;
        result.output = u"Cancelled before it was approved."_s;
        Q_EMIT toolFinished(result);
    };
    if (m_activeParked) {
        refuse(*m_activeParked);
        m_activeParked.reset();
    }
    for (const ParkedMethod &parked : std::as_const(m_parkedMethods)) {
        refuse(parked);
    }
    m_parkedMethods.clear();
}

void AcpClient::recordApprovalGrants(const PermissionRequest &request, PermissionDecision decision)
{
    // Approving a read request must not mint a write grant for the same path.
    if (request.risk == ToolRisk::Read && !request.mutatesFiles) {
        return;
    }
    QSet<QString> &grants = decision == PermissionDecision::AllowSession ? m_sessionGrants : m_onceGrants;
    auto absolutePath = [this](const QString &p) {
        if (QFileInfo(p).isAbsolute() || m_workspace.isEmpty()) {
            return QDir::cleanPath(QDir::fromNativeSeparators(p));
        }
        return QDir::cleanPath(QDir(m_workspace).absoluteFilePath(p));
    };
    QStringList paths;
    if (!request.resolvedPath.isEmpty()) {
        paths.append(absolutePath(request.resolvedPath));
    }
    if (!request.path.isEmpty()) {
        paths.append(absolutePath(request.path));
    }
    for (const QString &p : request.additionalPaths) {
        paths.append(absolutePath(p));
    }
    paths.removeDuplicates();
    for (const QString &p : std::as_const(paths)) {
        grants.insert(u"write:"_s + p);
    }
    // A command the agent announced in the permission request's raw input
    // covers a terminal/create with the same command line.
    const QJsonDocument doc = QJsonDocument::fromJson(request.details.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject args = doc.object();
    QJsonValue commandValue = args.value(u"command"_s);
    if (commandValue.isUndefined() || commandValue.isNull()) {
        commandValue = args.value(u"cmd"_s);
    }
    if (commandValue.isUndefined() || commandValue.isNull()) {
        commandValue = args.value(u"CommandLine"_s);
    }
    QStringList argv;
    if (commandValue.isArray()) {
        for (const QJsonValue &value : commandValue.toArray()) {
            argv.append(value.toString());
        }
    } else if (commandValue.isString()) {
        argv.append(commandValue.toString());
    }
    for (const QJsonValue &value : args.value(u"args"_s).toArray()) {
        argv.append(value.toString());
    }
    if (!argv.isEmpty()) {
        grants.insert(execGrantKey(argv.first(), argv.mid(1)));
    }
}

void AcpClient::handleFsRead(const QJsonValue &id, const QJsonObject &params)
{
    QString path = acpPathParam(params);
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute() && !m_workspace.isEmpty()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QString error;
    if (!pathAllowed(path, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    // Use the resolved path for the actual read. pathAllowed() canonicalises
    // internally but only returns a bool, so reading the raw path let a
    // symlink swapped in after the check escape the workspace.
    const QString resolved = currentSandbox().resolve(path, &error, false);
    if (resolved.isEmpty()) {
        replyError(id, kServerError, error);
        return;
    }
    // Bound the work before reading: an agent can ask a client to read a
    // multi-gigabyte log, and QTextStream::readAll() would copy it several
    // times on the GUI thread. Paginated reads are the supported shape.
    const QFileInfo fileInfo(resolved);
    constexpr qint64 kMaxAcpReadBytes = 64 * 1024 * 1024;
    if (fileInfo.exists() && fileInfo.size() > kMaxAcpReadBytes) {
        replyError(id, kServerError, u"File is larger than 64 MB; read it in line ranges."_s);
        return;
    }
    int line = qMax(0, jsonIntValue(params.value(u"line"_s), 0));
    if (line == 0) {
        line = qMax(0, jsonIntValue(params.value(u"offset"_s), 0));
    }
    const int startLine = qMax(0, jsonIntValue(params.value(u"start_line"_s), jsonIntValue(params.value(u"StartLine"_s), 0)));
    const int endLine = qMax(0, jsonIntValue(params.value(u"end_line"_s), jsonIntValue(params.value(u"EndLine"_s), 0)));
    if (startLine > 0) {
        line = startLine;
    }
    int requestedLimit = jsonIntValue(params.value(u"limit"_s), 0);
    if (requestedLimit == 0 && startLine > 0 && endLine >= startLine) {
        requestedLimit = (endLine - startLine + 1);
    }
    const int limit = requestedLimit <= 0 ? 10000 : qMin(requestedLimit, 10000);

    QStringList page;
    int totalLines = 0;
    if (!m_bridge
        || !m_bridge->readDocumentLines(resolved, line > 0 ? line : 1, limit, &page, &totalLines)) {
        replyError(id, kServerError, u"Could not read %1."_s.arg(path));
        return;
    }
    const int start = line > 0 ? line - 1 : 0;
    // fs/read_text_file returns the file text for the requested slice. Do not
    // append a pagination note: agents parse this as file content (JSON/YAML
    // lockfiles, generated sources) and a note corrupts it.
    QString sliced = (totalLines > 0 && start < totalLines) ? page.join(u'\n') : QString();
    QJsonObject result;
    result.insert(u"content"_s, sliced);
    if (totalLines > start + page.size()) {
        QJsonObject meta;
        meta.insert(u"truncated"_s, true);
        meta.insert(u"totalLines"_s, totalLines);
        meta.insert(u"nextLine"_s, start + page.size() + 1);
        result.insert(u"_meta"_s, meta);
    }
    replyResult(id, result);
}

void AcpClient::handleFsWrite(const QJsonValue &id, const QJsonObject &params)
{
    QString path = acpPathParam(params);
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute() && !m_workspace.isEmpty()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QString contentKey;
    for (const QString &k : {u"content"_s, u"Content"_s, u"contents"_s, u"code_content"_s, u"CodeContent"_s, u"text"_s}) {
        if (params.contains(k)) {
            contentKey = k;
            break;
        }
    }
    if (contentKey.isEmpty()) {
        replyError(id, kInvalidParams, u"content is required (pass an empty string to write an empty file)."_s);
        return;
    }
    const QString content = params.value(contentKey).toString();

    // The permission bar is the contract for every write: an approval for this
    // exact path lets the call through, anything else parks it until the user
    // or the policy answers.
    PermissionRequest request;
    request.toolName = u"write_file"_s;
    request.toolCallId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.path = path;
    request.resolvedPath = path;
    request.risk = ToolRisk::Write;
    request.mutatesFiles = true;
    request.summary = u"Write %1"_s.arg(path);
    QString existingForDiff;
    if (m_bridge && m_bridge->readDocument(path, &existingForDiff)) {
        request.describeDiff = unifiedDiff(path, existingForDiff, content);
    } else {
        request.describeDiff = unifiedDiff(path, QString(), content);
    }
    request.details = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
    if (!ensureMethodPermission(writeGrantKey(path), std::move(request), id, u"fs/write_text_file"_s, params)) {
        return;
    }
    QString error;
    if (!pathAllowed(path, true, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const QString resolved = currentSandbox().resolve(path, &error, true);
    if (resolved.isEmpty()) {
        replyError(id, kServerError, error);
        return;
    }
    if (!m_bridge) {
        replyError(id, kInternalError, u"No document bridge is available."_s);
        return;
    }
    if (!m_bridge->writeDocument(resolved, content, &error)) {
        replyError(id, kServerError, error.isEmpty() ? u"Could not write %1."_s.arg(path) : error);
        return;
    }
    replyResult(id, QJsonObject());
}

void AcpClient::handleFsEdit(const QJsonValue &id, const QJsonObject &params)
{
    QString path = acpPathParam(params);
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute() && !m_workspace.isEmpty()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QString oldText = params.value(u"old_string"_s).toString();
    if (oldText.isEmpty()) oldText = params.value(u"oldText"_s).toString();
    if (oldText.isEmpty()) oldText = params.value(u"Target"_s).toString();
    if (oldText.isEmpty()) oldText = params.value(u"TargetContent"_s).toString();
    QString newText = params.value(u"new_string"_s).toString();
    if (!params.contains(u"new_string"_s)) {
        if (params.contains(u"newText"_s)) newText = params.value(u"newText"_s).toString();
        else if (params.contains(u"Replacement"_s)) newText = params.value(u"Replacement"_s).toString();
        else if (params.contains(u"ReplacementContent"_s)) newText = params.value(u"ReplacementContent"_s).toString();
    }
    const bool isFullReplacement = oldText.isEmpty()
        && (params.contains(u"code_content"_s) || params.contains(u"CodeContent"_s)
            || params.contains(u"content"_s) || params.contains(u"Content"_s));
    if (isFullReplacement) {
        if (params.contains(u"code_content"_s)) newText = params.value(u"code_content"_s).toString();
        else if (params.contains(u"CodeContent"_s)) newText = params.value(u"CodeContent"_s).toString();
        else if (params.contains(u"content"_s)) newText = params.value(u"content"_s).toString();
        else if (params.contains(u"Content"_s)) newText = params.value(u"Content"_s).toString();
    } else if (path.isEmpty() || oldText.isEmpty()) {
        replyError(id, kInvalidParams, u"edit_file requires path and non-empty old_string."_s);
        return;
    }

    PermissionRequest request;
    request.toolName = u"edit_file"_s;
    request.toolCallId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.path = path;
    request.resolvedPath = path;
    request.risk = ToolRisk::Write;
    request.mutatesFiles = true;
    request.summary = u"Edit %1"_s.arg(path);
    if (isFullReplacement && m_bridge) {
        QString docBefore;
        if (m_bridge->readDocument(path, &docBefore)) {
            request.describeDiff = unifiedDiff(path, docBefore, newText);
        }
    }
    if (request.describeDiff.isEmpty()) {
        request.describeDiff = unifiedDiff(path, oldText, newText);
    }
    request.details = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
    if (!ensureMethodPermission(writeGrantKey(path), std::move(request), id, u"fs/edit_file"_s, params)) {
        return;
    }

    QString error;
    if (!pathAllowed(path, true, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const QString resolved = currentSandbox().resolve(path, &error, true);
    if (resolved.isEmpty()) {
        replyError(id, kServerError, error);
        return;
    }
    if (!m_bridge) {
        replyError(id, kInternalError, u"No document bridge is available."_s);
        return;
    }
    QString contents;
    if (!m_bridge->readDocument(resolved, &contents)) {
        replyError(id, kServerError, u"Could not read %1."_s.arg(path));
        return;
    }
    QString updated = contents;
    if (isFullReplacement) {
        updated = newText;
    } else {
        const bool replaceAll = boolishValue(params.value(u"replace_all"_s), false);
        const qsizetype first = contents.indexOf(oldText);
        if (first < 0) {
            replyError(id, kServerError, u"old_string was not found in %1."_s.arg(path));
            return;
        }
        if (!replaceAll && contents.indexOf(oldText, first + oldText.size()) >= 0) {
            replyError(id, kServerError, u"old_string appears more than once in %1; provide more context or set replace_all."_s.arg(path));
            return;
        }
        if (replaceAll) {
            updated.replace(oldText, newText);
        } else {
            updated.replace(first, oldText.size(), newText);
        }
    }
    if (!m_bridge->writeDocumentIfUnchanged(resolved, contents, updated, &error)) {
        replyError(id, kServerError, error.isEmpty() ? u"Could not edit %1."_s.arg(path) : error);
        return;
    }
    QJsonObject result;
    result.insert(u"path"_s, resolved);
    result.insert(u"diff"_s, unifiedDiff(path, contents, updated));
    replyResult(id, result);
}

void AcpClient::handleFsMultiEdit(const QJsonValue &id, const QJsonObject &params)
{
    QString path = acpPathParam(params);
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute() && !m_workspace.isEmpty()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QJsonArray edits = params.value(u"edits"_s).toArray();
    if (edits.isEmpty()) edits = params.value(u"chunks"_s).toArray();
    if (edits.isEmpty()) edits = params.value(u"ReplacementChunks"_s).toArray();
    if (path.isEmpty() || edits.isEmpty()) {
        replyError(id, kInvalidParams, u"multi_edit_file requires path and a non-empty edits array."_s);
        return;
    }

    PermissionRequest request;
    request.toolName = u"multi_edit_file"_s;
    request.toolCallId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.path = path;
    request.resolvedPath = path;
    request.risk = ToolRisk::Write;
    request.mutatesFiles = true;
    request.summary = u"Edit %1 (%2 edits)"_s.arg(path).arg(edits.size());
    request.details = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
    if (!ensureMethodPermission(writeGrantKey(path), std::move(request), id, u"fs/multi_edit_file"_s, params)) {
        return;
    }

    QString error;
    if (!pathAllowed(path, true, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const QString resolved = currentSandbox().resolve(path, &error, true);
    if (resolved.isEmpty()) {
        replyError(id, kServerError, error);
        return;
    }
    if (!m_bridge) {
        replyError(id, kInternalError, u"No document bridge is available."_s);
        return;
    }
    QString original;
    if (!m_bridge->readDocument(resolved, &original)) {
        replyError(id, kServerError, u"Could not read %1."_s.arg(path));
        return;
    }
    QString updated = original;
    for (int i = 0; i < edits.size(); ++i) {
        const QJsonObject edit = edits.at(i).toObject();
        QString oldText = edit.value(u"old_string"_s).toString();
        if (oldText.isEmpty()) oldText = edit.value(u"oldText"_s).toString();
        if (oldText.isEmpty()) oldText = edit.value(u"TargetContent"_s).toString();
        if (oldText.isEmpty()) oldText = edit.value(u"Target"_s).toString();
        QString newText = edit.value(u"new_string"_s).toString();
        if (!edit.contains(u"new_string"_s)) {
            if (edit.contains(u"newText"_s)) newText = edit.value(u"newText"_s).toString();
            else if (edit.contains(u"ReplacementContent"_s)) newText = edit.value(u"ReplacementContent"_s).toString();
            else if (edit.contains(u"Replacement"_s)) newText = edit.value(u"Replacement"_s).toString();
        }
        if (oldText.isEmpty()) {
            replyError(id, kInvalidParams, u"Edit %1 has an empty old_string."_s.arg(i + 1));
            return;
        }
        const bool replaceAll = boolishValue(edit.value(u"replace_all"_s), false);
        const qsizetype first = updated.indexOf(oldText);
        if (first < 0) {
            replyError(id, kServerError, u"Edit %1: old_string was not found in %2."_s.arg(i + 1).arg(path));
            return;
        }
        if (!replaceAll && updated.indexOf(oldText, first + oldText.size()) >= 0) {
            replyError(id, kServerError, u"Edit %1: old_string appears more than once in %2."_s.arg(i + 1).arg(path));
            return;
        }
        if (replaceAll) {
            updated.replace(oldText, newText);
        } else {
            updated.replace(first, oldText.size(), newText);
        }
    }
    if (!m_bridge->writeDocumentIfUnchanged(resolved, original, updated, &error)) {
        replyError(id, kServerError, error.isEmpty() ? u"Could not edit %1."_s.arg(path) : error);
        return;
    }
    QJsonObject result;
    result.insert(u"path"_s, resolved);
    result.insert(u"editsApplied"_s, edits.size());
    result.insert(u"diff"_s, unifiedDiff(path, original, updated));
    replyResult(id, result);
}

void AcpClient::handleFsDelete(const QJsonValue &id, const QJsonObject &params)
{
    QString path = acpPathParam(params);
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute() && !m_workspace.isEmpty()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    if (path.isEmpty()) {
        replyError(id, kInvalidParams, u"delete_file requires path."_s);
        return;
    }

    PermissionRequest request;
    request.toolName = u"edit_file"_s;
    request.originalToolName = u"delete_file"_s;
    request.toolCallId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.path = path;
    request.resolvedPath = path;
    request.risk = ToolRisk::Write;
    request.mutatesFiles = true;
    request.summary = u"Delete %1"_s.arg(path);
    if (m_bridge) {
        QString docBefore;
        if (m_bridge->readDocument(path, &docBefore)) {
            request.describeDiff = unifiedDiff(path, docBefore, QString());
        }
    }
    request.details = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
    if (!ensureMethodPermission(writeGrantKey(path), std::move(request), id, u"fs/delete_file"_s, params)) {
        return;
    }

    QString error;
    if (!pathAllowed(path, true, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const QString resolved = currentSandbox().resolve(path, &error, true);
    if (resolved.isEmpty()) {
        replyError(id, kServerError, error);
        return;
    }
    if (!m_bridge) {
        replyError(id, kInternalError, u"No document bridge is available."_s);
        return;
    }
    if (!m_bridge->removeDocument(resolved, &error)) {
        replyError(id, kServerError, error.isEmpty() ? u"Could not delete %1."_s.arg(path) : error);
        return;
    }
    replyResult(id, QJsonObject{{u"path"_s, resolved}});
}

void AcpClient::handleFsListDir(const QJsonValue &id, const QJsonObject &params)
{
    QString path = acpPathParam(params);
    if (path.isEmpty()) {
        path = sessionCwd();
    } else if (!QFileInfo(path).isAbsolute() && !m_workspace.isEmpty()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QString error;
    if (!pathAllowed(path, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const QString resolved = currentSandbox().resolve(path, &error, false);
    if (resolved.isEmpty()) {
        replyError(id, kServerError, error);
        return;
    }
    const QDir dir(resolved);
    if (!dir.exists()) {
        replyError(id, kServerError, u"Directory does not exist: %1"_s.arg(path));
        return;
    }
    const Sandbox sandbox = currentSandbox();
    const QFileInfoList infos = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                                                  QDir::DirsFirst | QDir::Name);
    QJsonArray entries;
    QStringList lines;
    constexpr int kMaxEntries = 500;
    for (const QFileInfo &info : infos) {
        if (entries.size() >= kMaxEntries) {
            lines.append(u"… (truncated)"_s);
            break;
        }
        const QString entryPath = info.absoluteFilePath();
        if (!sandbox.allowsRead(entryPath, nullptr)) {
            continue;
        }
        QJsonObject item;
        item.insert(u"name"_s, info.fileName());
        item.insert(u"type"_s, info.isDir() ? u"directory"_s : (info.isSymLink() ? u"symlink"_s : u"file"_s));
        if (info.isFile()) {
            item.insert(u"size"_s, info.size());
        }
        entries.append(item);
        lines.append(info.isDir() ? info.fileName() + u'/' : info.fileName());
    }
    QJsonObject result;
    result.insert(u"entries"_s, entries);
    result.insert(u"content"_s, lines.join(u'\n'));
    replyResult(id, result);
}

void AcpClient::handleFsGrep(const QJsonValue &id, const QJsonObject &params)
{
    const QString pattern = firstStringField(params, {u"pattern"_s, u"query"_s, u"Query"_s, u"regex"_s});
    if (pattern.isEmpty()) {
        replyError(id, kInvalidParams, u"grep requires a non-empty pattern."_s);
        return;
    }
    QString root = acpPathParam(params);
    if (root.isEmpty()) {
        root = firstStringField(params, {u"SearchPath"_s, u"SearchDirectory"_s});
    }
    if (root.isEmpty()) {
        root = sessionCwd();
    } else if (!QFileInfo(root).isAbsolute() && !m_workspace.isEmpty()) {
        root = QDir(m_workspace).absoluteFilePath(root);
    }
    QString error;
    if (!pathAllowed(root, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const QString resolvedRoot = currentSandbox().resolve(root, &error, false);
    if (resolvedRoot.isEmpty()) {
        replyError(id, kServerError, error);
        return;
    }
    const bool caseInsensitive = boolishValue(params.value(u"case_insensitive"_s), false);
    const QRegularExpression regex(
        pattern, caseInsensitive ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption);
    if (!regex.isValid()) {
        replyError(id, kInvalidParams, u"Invalid regex pattern: %1"_s.arg(regex.errorString()));
        return;
    }
    const QString globFilter = params.value(u"glob"_s).toString().trimmed();
    QRegularExpression globRe;
    if (!globFilter.isEmpty()) {
        globRe = QRegularExpression(QRegularExpression::wildcardToRegularExpression(globFilter));
    }

    const Sandbox sandbox = currentSandbox();
    QStringList filesToSearch;
    const QFileInfo rootInfo(resolvedRoot);
    if (rootInfo.isFile()) {
        filesToSearch.append(resolvedRoot);
    } else {
        static const QSet<QString> skipDirs = {
            u".git"_s, u"node_modules"_s, u"build"_s, u"CMakeFiles"_s, u".cache"_s, u"__pycache"_s,
        };
        QDirIterator it(resolvedRoot, QDir::Files, QDirIterator::Subdirectories);
        constexpr int kMaxFiles = 4000;
        while (it.hasNext() && filesToSearch.size() < kMaxFiles) {
            const QString filePath = it.next();
            const QString rel = QDir(resolvedRoot).relativeFilePath(filePath);
            const QStringList parts = rel.split(u'/');
            bool skipped = false;
            for (int i = 0; i + 1 < parts.size(); ++i) {
                if (skipDirs.contains(parts.at(i))) {
                    skipped = true;
                    break;
                }
            }
            if (skipped || !sandbox.allowsRead(filePath, nullptr)) {
                continue;
            }
            if (globRe.isValid() && !globFilter.isEmpty()
                && !globRe.match(QFileInfo(filePath).fileName()).hasMatch()
                && !globRe.match(rel).hasMatch()) {
                continue;
            }
            filesToSearch.append(filePath);
        }
    }

    QStringList matches;
    constexpr int kMaxMatches = 200;
    constexpr qint64 kMaxSearchFileBytes = 2 * 1024 * 1024;
    for (const QString &filePath : filesToSearch) {
        if (matches.size() >= kMaxMatches) {
            break;
        }
        const QFileInfo fi(filePath);
        if (fi.size() > kMaxSearchFileBytes) {
            continue;
        }
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray raw = file.read(kMaxSearchFileBytes);
        if (raw.contains('\0')) {
            continue; // skip binary files
        }
        const QStringList lines = QString::fromUtf8(raw).split(u'\n');
        const QString rel = m_workspace.isEmpty() ? filePath : QDir(m_workspace).relativeFilePath(filePath);
        for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
            if (regex.match(lines.at(lineIdx)).hasMatch()) {
                matches.append(u"%1:%2:%3"_s.arg(rel).arg(lineIdx + 1).arg(lines.at(lineIdx)));
                if (matches.size() >= kMaxMatches) {
                    matches.append(u"… (matches truncated)"_s);
                    break;
                }
            }
        }
    }

    QJsonObject result;
    result.insert(u"content"_s, matches.isEmpty() ? u"(no matches)"_s : matches.join(u'\n'));
    result.insert(u"matches"_s, QJsonArray::fromStringList(matches));
    replyResult(id, result);
}

void AcpClient::handleFsGlob(const QJsonValue &id, const QJsonObject &params)
{
    const QString pattern = firstStringField(params, {u"pattern"_s, u"glob"_s, u"Pattern"_s});
    if (pattern.isEmpty()) {
        replyError(id, kInvalidParams, u"glob requires a non-empty pattern."_s);
        return;
    }
    QString root = acpPathParam(params);
    if (root.isEmpty()) {
        root = firstStringField(params, {u"SearchDirectory"_s, u"SearchPath"_s});
    }
    if (root.isEmpty()) {
        root = sessionCwd();
    } else if (!QFileInfo(root).isAbsolute() && !m_workspace.isEmpty()) {
        root = QDir(m_workspace).absoluteFilePath(root);
    }
    QString error;
    if (!pathAllowed(root, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const QString resolvedRoot = currentSandbox().resolve(root, &error, false);
    if (resolvedRoot.isEmpty()) {
        replyError(id, kServerError, error);
        return;
    }

    // Support `**/` recursive glob patterns cleanly.
    QString rxStr = QRegularExpression::wildcardToRegularExpression(
        pattern, QRegularExpression::UnanchoredWildcardConversion);
    rxStr.replace(u"[^/]*[^/]*/"_s, u"(?:.*/)?"_s);
    rxStr.replace(u"[^/]*[^/]*"_s, u".*"_s);
    const QRegularExpression re(u"\\A(?:"_s + rxStr + u")\\z"_s);

    const Sandbox sandbox = currentSandbox();
    QStringList matched;
    constexpr int kMaxMatches = 500;
    QDirIterator it(resolvedRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && matched.size() < kMaxMatches) {
        const QString filePath = it.next();
        const QString rel = QDir(resolvedRoot).relativeFilePath(filePath);
        if (rel.startsWith(u".git/"_s) || rel.contains(u"/.git/"_s)
            || rel.startsWith(u"node_modules/"_s) || rel.contains(u"/node_modules/"_s)) {
            continue;
        }
        if (!sandbox.allowsRead(filePath, nullptr)) {
            continue;
        }
        if (re.match(rel).hasMatch() || (!pattern.contains(u'/') && re.match(QFileInfo(filePath).fileName()).hasMatch())) {
            matched.append(rel);
        }
    }
    std::sort(matched.begin(), matched.end());
    QJsonObject result;
    result.insert(u"files"_s, QJsonArray::fromStringList(matched));
    result.insert(u"content"_s, matched.isEmpty() ? u"(no files matched)"_s : matched.join(u'\n'));
    replyResult(id, result);
}

void AcpClient::handleApplyPatch(const QJsonValue &id, const QJsonObject &params)
{
    const QString patchText = firstStringField(params, {u"patch"_s, u"diff"_s, u"content"_s});
    if (patchText.isEmpty()) {
        replyError(id, kInvalidParams, u"apply_patch requires a non-empty patch string."_s);
        return;
    }
    const QList<PatchFile> files = parseUnifiedPatch(patchText);
    if (files.isEmpty()) {
        replyError(id, kInvalidParams, u"Could not parse a valid unified diff patch."_s);
        return;
    }
    QStringList targetPaths;
    for (const PatchFile &f : files) {
        const QString raw = f.isDelete ? f.oldPath : f.path;
        if (raw.isEmpty()) {
            continue;
        }
        const QString abs = (!QFileInfo(raw).isAbsolute() && !m_workspace.isEmpty())
            ? QDir(m_workspace).absoluteFilePath(raw)
            : raw;
        targetPaths.append(QDir::cleanPath(abs));
    }
    if (targetPaths.isEmpty()) {
        replyError(id, kInvalidParams, u"Patch did not target any files."_s);
        return;
    }

    PermissionRequest request;
    request.toolName = u"apply_patch"_s;
    request.toolCallId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.path = targetPaths.first();
    request.resolvedPath = targetPaths.first();
    request.additionalPaths = targetPaths.mid(1);
    request.risk = ToolRisk::Write;
    request.mutatesFiles = true;
    request.summary = u"Apply patch (%1 file(s))"_s.arg(targetPaths.size());
    request.describeDiff = patchText;
    request.details = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
    const QString grantKey = u"patch:"_s + targetPaths.join(u'|');
    if (!ensureMethodPermission(grantKey, std::move(request), id, u"apply_patch"_s, params)) {
        return;
    }
    if (!m_bridge) {
        replyError(id, kInternalError, u"No document bridge is available."_s);
        return;
    }

    // Validate every file before writing any of them.
    struct PlannedFile {
        QString resolved;
        QString original;
        QString updated;
        bool isNew = false;
        bool isDelete = false;
    };
    QList<PlannedFile> planned;
    for (const PatchFile &pf : files) {
        const QString raw = pf.isDelete ? pf.oldPath : pf.path;
        const QString abs = (!QFileInfo(raw).isAbsolute() && !m_workspace.isEmpty())
            ? QDir(m_workspace).absoluteFilePath(raw)
            : raw;
        QString error;
        if (!pathAllowed(abs, true, &error)) {
            replyError(id, kServerError, error);
            return;
        }
        const QString resolved = currentSandbox().resolve(abs, &error, true);
        if (resolved.isEmpty()) {
            replyError(id, kServerError, error);
            return;
        }
        PlannedFile plan;
        plan.resolved = resolved;
        plan.isNew = pf.isNew;
        plan.isDelete = pf.isDelete;
        if (!pf.isNew) {
            if (!m_bridge->readDocument(resolved, &plan.original)) {
                replyError(id, kServerError, u"Could not read %1."_s.arg(raw));
                return;
            }
        }
        if (!pf.isDelete) {
            QString patchErr;
            plan.updated = applyPatchToContent(plan.original, pf.hunks, &patchErr);
            if (!patchErr.isEmpty()) {
                replyError(id, kServerError, u"Patch failed on %1: %2"_s.arg(raw, patchErr));
                return;
            }
        }
        planned.append(std::move(plan));
    }

    QStringList appliedPaths;
    for (const PlannedFile &plan : planned) {
        QString error;
        bool ok = false;
        if (plan.isDelete) {
            ok = m_bridge->removeDocument(plan.resolved, &error);
        } else if (plan.isNew) {
            ok = m_bridge->writeDocumentIfMissing(plan.resolved, plan.updated, &error);
        } else {
            ok = m_bridge->writeDocumentIfUnchanged(plan.resolved, plan.original, plan.updated, &error);
        }
        if (!ok) {
            replyError(id, kServerError, error.isEmpty() ? u"Failed writing %1."_s.arg(plan.resolved) : error);
            return;
        }
        appliedPaths.append(plan.resolved);
    }

    QJsonObject result;
    result.insert(u"files"_s, QJsonArray::fromStringList(appliedPaths));
    result.insert(u"content"_s, u"Applied patch to %1 file(s)."_s.arg(appliedPaths.size()));
    replyResult(id, result);
}

void AcpClient::handleExitPlanMode(const QJsonValue &id, const QJsonObject &params)
{
    Q_UNUSED(params);
    Q_EMIT exitPlanModeRequested();
    replyResult(id, QJsonObject{{u"ok"_s, true}});
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
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
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
    const QString text = suggestionTextFromToolArguments(params);
    if (text.isEmpty()) {
        replyError(id, kInvalidParams, u"suggestion must not be empty: describe what you are suggesting."_s);
        return;
    }

    Suggestion suggestion;
    suggestion.text = text;
    suggestion.path = firstStringToolArgument(
        params, {u"path"_s, u"file_path"_s, u"filePath"_s, u"file"_s, u"target"_s, u"uri"_s});
    if (suggestion.path.startsWith(u"file://"_s)) {
        suggestion.path = QUrl(suggestion.path).toLocalFile();
    }
    // Resolve a workspace-relative anchor, the same way the native path does,
    // so Accept opens the right file.
    if (!suggestion.path.isEmpty() && !QFileInfo(suggestion.path).isAbsolute() && !m_workspace.isEmpty()) {
        suggestion.path = QDir(m_workspace).absoluteFilePath(suggestion.path);
    }
    const QJsonValue lineValue = params.contains(u"line"_s)
        ? params.value(u"line"_s)
        : (params.contains(u"start_line"_s) ? params.value(u"start_line"_s) : params.value(u"startLine"_s));
    if (!lineValue.isUndefined() && !lineValue.isNull()) {
        const int line = jsonIntValue(lineValue, -1);
        if (line < 1) {
            replyError(id, kInvalidParams, u"line must be a 1-based line number, or leave it out."_s);
            return;
        }
        suggestion.line = line;
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

    auto formatNode = [this](const GraphNode *node) {
        if (!node) {
            return QString();
        }
        QString line = u"- %1 [%2]"_s.arg(m_projectGraph->nodeLabel(node), node->type);
        if (node->linesOfCode > 0) {
            line += u" %1 loc"_s.arg(node->linesOfCode);
        }
        const QString symbols = m_projectGraph->nodeSymbols(node);
        if (!symbols.isEmpty()) {
            line += u"; symbols: "_s + symbols;
        }
        return line;
    };

    QStringList lines;
    const QString type = queryType.isEmpty() ? u"summary"_s : queryType;

    if (type == u"summary"_s) {
        lines.append(m_projectGraph->contextSummary(40, 60, 1));
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
            const GraphNode *source = m_projectGraph->getNode(edge->sourceId);
            const GraphNode *target = m_projectGraph->getNode(edge->targetId);
            lines.append(u"%1 -> %2 (%3)"_s.arg(
                source ? m_projectGraph->nodeLabel(source) : edge->sourceId.left(12),
                target ? m_projectGraph->nodeLabel(target) : edge->targetId.left(12),
                edge->relationship));
            if (++shown >= 150) {
                lines.append(u"... truncated"_s);
                break;
            }
        }
    } else if (type == u"dependencies"_s || type == u"dependents"_s || type == u"find_related"_s) {
        GraphNode *node = nullptr;
        if (!nodeId.isEmpty()) {
            node = m_projectGraph->findNodeByPath(nodeId);
            if (!node) {
                node = m_projectGraph->getNode(nodeId);
            }
            if (!node) {
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
                const GraphNode *depNode = m_projectGraph->getNode(dep);
                lines.append(u"depends on: %1"_s.arg(depNode ? m_projectGraph->nodeLabel(depNode) : dep));
            }
        } else if (type == u"dependents"_s) {
            for (const QString &dep : node->dependents) {
                const GraphNode *depNode = m_projectGraph->getNode(dep);
                lines.append(u"used by: %1"_s.arg(depNode ? m_projectGraph->nodeLabel(depNode) : dep));
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
            source = m_projectGraph->findNodeByPath(sourceId);
            if (!source) {
                source = m_projectGraph->getNode(sourceId);
            }
        }
        if (!targetId.isEmpty()) {
            target = m_projectGraph->findNodeByPath(targetId);
            if (!target) {
                target = m_projectGraph->getNode(targetId);
            }
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
                QStringList labels;
                for (const QString &nodeId : path) {
                    const GraphNode *node = m_projectGraph->getNode(nodeId);
                    labels.append(node ? m_projectGraph->nodeLabel(node) : nodeId);
                }
                lines.append(labels.join(u" -> "_s));
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

// Kate-specific client method, mirroring the native lsp_diagnostics tool: the
// agent asks for language-server/editor diagnostics and Kate answers from its
// LSP bridge. Read-only, so no sandbox write check and no permission prompt.
void AcpClient::handleLspDiagnostics(const QJsonValue &id, const QJsonObject &params)
{
    if (!m_lsp || !m_lsp->isAvailable()) {
        replyError(id, kServerError,
                   u"No language server available: Kate's LSP plugin is not loaded. Run a build or lint command instead."_s);
        return;
    }

    QString path = params.value(u"path"_s).toString().trimmed();
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute()) {
        // Agents may send a workspace-relative path; the native tool resolves
        // against the workspace, so do the same here.
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    if (!path.isEmpty()) {
        QString error;
        if (!pathAllowed(path, false, &error)) {
            replyError(id, kServerError, error);
            return;
        }
    }

    static const auto severityName = [](int severity) {
        switch (severity) {
        case 1:
            return u"error"_s;
        case 2:
            return u"warning"_s;
        case 3:
            return u"info"_s;
        default:
            return u"hint"_s;
        }
    };
    const QString severityFilter = params.value(u"severity"_s).toString().trimmed().toLower();
    int minSeverity = 4;
    if (severityFilter == u"error"_s) {
        minSeverity = 1;
    } else if (severityFilter == u"warning"_s) {
        minSeverity = 2;
    } else if (severityFilter == u"info"_s || severityFilter == u"information"_s) {
        minSeverity = 3;
    }

    const QList<LspDiagnostic> diags = m_lsp->diagnostics(path);
    QStringList lines;
    int errors = 0;
    int warnings = 0;
    for (const LspDiagnostic &d : diags) {
        if (d.severity > minSeverity) {
            continue;
        }
        if (d.severity == 1) {
            ++errors;
        } else if (d.severity == 2) {
            ++warnings;
        }
        QString rel = QDir(m_workspace).relativeFilePath(d.path);
        if (rel.isEmpty()) {
            rel = d.path;
        }
        lines.append(u"%1:%2:%3: %4: %5%6"_s
                         .arg(rel)
                         .arg(d.line)
                         .arg(d.column > 0 ? d.column : 1)
                         .arg(severityName(d.severity))
                         .arg(d.message)
                         .arg(d.source.isEmpty() ? QString() : u" [%1]"_s.arg(d.source)));
    }
    std::sort(lines.begin(), lines.end());
    lines.prepend(u"%1 error(s), %2 warning(s)%3"_s
                      .arg(errors)
                      .arg(warnings)
                      .arg(path.isEmpty() ? u" in the workspace"_s : u" in %1"_s.arg(QDir(m_workspace).relativeFilePath(path))));
    if (path.isEmpty()) {
        const QStringList messages = m_lsp->recentMessages(5);
        if (!messages.isEmpty()) {
            lines.append(u"Recent LSP messages:"_s);
            for (const QString &m : messages) {
                lines.append(u"  "_s + m);
            }
        }
    }

    QJsonObject result;
    result.insert(u"content"_s, lines.join(u'\n'));
    replyResult(id, result);
}

void AcpClient::handleLspSymbols(const QJsonValue &id, const QJsonObject &params)
{
    if (!m_lsp || !m_lsp->isAvailable()) {
        replyError(id, kServerError,
                   u"No language server available: Kate's LSP plugin is not loaded. Use grep or query_project_graph instead."_s);
        return;
    }
    QString path = params.value(u"path"_s).toString().trimmed();
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QString error;
    if (!pathAllowed(path, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const QList<LspSymbol> symbols = m_lsp->documentSymbols(path);
    QStringList lines;
    if (symbols.isEmpty()) {
        lines.append(u"No symbols reported for %1 (no language server running for this file, or none found)."_s.arg(path));
    } else {
        for (const LspSymbol &s : symbols) {
            lines.append(u"%1: %2 %3"_s.arg(s.line).arg(s.kind, s.name));
        }
    }
    QJsonObject result;
    result.insert(u"content"_s, lines.join(u'\n'));
    replyResult(id, result);
}

void AcpClient::handleLspHover(const QJsonValue &id, const QJsonObject &params)
{
    if (!m_lsp || !m_lsp->isAvailable()) {
        replyError(id, kServerError,
                   u"No language server available: Kate's LSP plugin is not loaded."_s);
        return;
    }
    QString path = params.value(u"path"_s).toString().trimmed();
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QString error;
    if (!pathAllowed(path, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const int line = qMax(1, jsonIntValue(params.value(u"line"_s), 1));
    const int column = qMax(1, jsonIntValue(params.value(u"column"_s), 1));
    const QString hover = m_lsp->hover(path, line, column);
    QJsonObject result;
    result.insert(u"content"_s, hover.isEmpty() ? u"No hover information at %1:%2:%3."_s.arg(path).arg(line).arg(column) : hover);
    replyResult(id, result);
}

void AcpClient::handleLspDefinition(const QJsonValue &id, const QJsonObject &params)
{
    if (!m_lsp || !m_lsp->isAvailable()) {
        replyError(id, kServerError,
                   u"No language server available: Kate's LSP plugin is not loaded."_s);
        return;
    }
    QString path = params.value(u"path"_s).toString().trimmed();
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QString error;
    if (!pathAllowed(path, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const int line = qMax(1, jsonIntValue(params.value(u"line"_s), 1));
    const int column = qMax(1, jsonIntValue(params.value(u"column"_s), 1));
    const QList<LspLocation> defs = m_lsp->definitions(path, line, column);
    QStringList lines;
    if (defs.isEmpty()) {
        lines.append(u"No definition found at %1:%2:%3."_s.arg(path).arg(line).arg(column));
    } else {
        for (const LspLocation &loc : defs) {
            lines.append(u"%1:%2:%3"_s.arg(loc.path).arg(loc.line).arg(loc.column));
        }
    }
    QJsonObject result;
    result.insert(u"content"_s, lines.join(u'\n'));
    replyResult(id, result);
}

void AcpClient::handleLspReferences(const QJsonValue &id, const QJsonObject &params)
{
    if (!m_lsp || !m_lsp->isAvailable()) {
        replyError(id, kServerError,
                   u"No language server available: Kate's LSP plugin is not loaded."_s);
        return;
    }
    QString path = params.value(u"path"_s).toString().trimmed();
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QString error;
    if (!pathAllowed(path, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const int line = qMax(1, jsonIntValue(params.value(u"line"_s), 1));
    const int column = qMax(1, jsonIntValue(params.value(u"column"_s), 1));
    const bool includeDeclaration = params.value(u"include_declaration"_s).toBool(true);
    const QList<LspLocation> refs = m_lsp->references(path, line, column, includeDeclaration);
    QStringList lines;
    if (refs.isEmpty()) {
        lines.append(u"No references found at %1:%2:%3."_s.arg(path).arg(line).arg(column));
    } else {
        for (const LspLocation &loc : refs) {
            lines.append(u"%1:%2:%3"_s.arg(loc.path).arg(loc.line).arg(loc.column));
        }
    }
    QJsonObject result;
    result.insert(u"content"_s, lines.join(u'\n'));
    replyResult(id, result);
}

void AcpClient::handleLspCodeActions(const QJsonValue &id, const QJsonObject &params)
{
    if (!m_lsp || !m_lsp->isAvailable()) {
        replyError(id, kServerError,
                   u"No language server available: Kate's LSP plugin is not loaded."_s);
        return;
    }
    QString path = params.value(u"path"_s).toString().trimmed();
    if (!path.isEmpty() && !QFileInfo(path).isAbsolute()) {
        path = QDir(m_workspace).absoluteFilePath(path);
    }
    QString error;
    if (!pathAllowed(path, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const int line = qMax(1, jsonIntValue(params.value(u"line"_s), 1));
    const int column = qMax(1, jsonIntValue(params.value(u"column"_s), 1));
    const QStringList actions = m_lsp->codeActions(path, line, column);
    QStringList lines;
    if (actions.isEmpty()) {
        lines.append(u"No code actions offered at %1:%2:%3."_s.arg(path).arg(line).arg(column));
    } else {
        for (int i = 0; i < actions.size(); ++i) {
            lines.append(u"%1. %2"_s.arg(i + 1).arg(actions.at(i)));
        }
    }
    QJsonObject result;
    result.insert(u"content"_s, lines.join(u'\n'));
    replyResult(id, result);
}

void AcpClient::handleKateToolMethod(const QString &toolName, const QJsonValue &id, const QJsonObject &params)
{
    ToolRunner runner(currentSandbox(), m_bridge);
    runner.setTimeoutMs(m_settings.bashTimeoutMs);
    runner.setProjectGraph(m_projectGraph);
    runner.setLspProvider(m_lsp);
    runner.setDebugProvider(m_debug);
    runner.setEcosystemProvider(m_ecosystem);

    ToolCall call;
    call.id = u"acp-kate-%1"_s.arg(id.toVariant().toString());
    call.name = toolName;
    call.arguments = params;

    PermissionRequest req = runner.describe(call);
    if (req.risk != ToolRisk::Read) {
        const QString grantKey = u"kateai:%1:%2"_s.arg(toolName, req.summary);
        if (!ensureMethodPermission(grantKey, req, id, toolName, params)) {
            return;
        }
    }

    const ToolResult res = runner.run(call);
    if (!res.ok) {
        replyError(id, kServerError, res.output.isEmpty() ? u"Tool execution failed."_s : res.output);
        return;
    }
    QJsonObject payload;
    payload.insert(u"content"_s, res.output);
    payload.insert(u"ok"_s, true);
    replyResult(id, payload);
}

// Kate extension client method, mirroring the native question tool: the agent
// hands over the question and Kate parks the RPC until the user answers, which
// is what makes asking work over ACP — v1 has no other ask-user channel. The
// answer is the RPC result.
void AcpClient::handleAskUser(const QJsonValue &id, const QJsonObject &params)
{
    const QString question = questionTextFromToolArguments(params);
    const QStringList options = questionOptionsFromToolArguments(params);
    const bool allowFreeform = questionAllowFreeformFromToolArguments(params, true);
    askUser(id, question, options, allowFreeform);
}

// ACP v2 draft elicitation: same ask-user flow under the standardized method.
void AcpClient::handleElicitationCreate(const QJsonValue &id, const QJsonObject &params)
{
    // URL elicitation needs a completion callback and browser flow; Kate
    // implements the form variant, so decline the URL variant rather than
    // sending a free-text answer with the wrong shape.
    if (params.value(u"mode"_s).toString() == u"url"_s) {
        QJsonObject result;
        result.insert(u"action"_s, u"cancel"_s);
        replyResult(id, result);
        return;
    }
    const QString message = params.value(u"message"_s).toString().trimmed();
    QStringList options;
    QString firstProperty;
    QHash<QString, QString> valueMap;
    const QJsonObject schema = params.value(u"requestedSchema"_s).toObject();
    const QJsonObject properties = schema.value(u"properties"_s).toObject();
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (firstProperty.isEmpty()) {
            firstProperty = it.key();
        }
        const QJsonObject property = it.value().toObject();
        for (const QJsonValue &v : property.value(u"enum"_s).toArray()) {
            const QString option = v.toString();
            if (!option.isEmpty()) {
                options.append(option);
            }
        }
        // v2 also expresses single-select options as oneOf: [{const, title}].
        for (const QJsonValue &v : property.value(u"oneOf"_s).toArray()) {
            const QJsonObject option = v.toObject();
            const QString rawConst = option.value(u"const"_s).toString();
            const QString label = option.value(u"title"_s).toString(rawConst);
            if (!label.isEmpty() && !options.contains(label)) {
                options.append(label);
                if (!rawConst.isEmpty()) {
                    valueMap.insert(label, rawConst);
                }
            }
        }
    }
    askUser(id, message, options, true, true, firstProperty);
    m_questionElicitationValueMap = std::move(valueMap);
}

void AcpClient::askUser(const QJsonValue &id, const QString &question, const QStringList &options, bool allowFreeform,
                        bool elicitation, const QString &elicitationProperty)
{
    if (question.isEmpty()) {
        replyError(id, kInvalidParams, u"question must not be empty: state what you are asking the user."_s);
        return;
    }
    // A permission request and a question are both blocking UI cards. Only one
    // may be on screen; cancel the permission rather than leaving a stale
    // approval card that could be clicked later.
    if (hasPendingPermission()) {
        cancelPendingPermission();
    }
    // Only one question can be on screen at a time: answer a previous parked
    // question as cancelled before parking this one, or the agent blocks on it.
    cancelPendingQuestion();
    m_questionRpcId = id;
    m_questionIsElicitation = elicitation;
    m_questionElicitationProperty = elicitationProperty;
    PermissionRequest request;
    request.toolName = u"question"_s;
    request.toolCallId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.risk = ToolRisk::Read;
    request.questionText = question;
    request.questionOptions = options;
    request.allowFreeform = allowFreeform;
    request.summary = question.left(200);
    QJsonArray optionsJson;
    for (const QString &option : options) {
        optionsJson.append(option);
    }
    QJsonObject detailsObj;
    detailsObj.insert(u"question"_s, question);
    detailsObj.insert(u"options"_s, optionsJson);
    detailsObj.insert(u"allow_freeform"_s, allowFreeform);
    request.details = QString::fromUtf8(QJsonDocument(detailsObj).toJson(QJsonDocument::Compact));
    m_questionRequest = request;
    Q_EMIT questionAsked(request);
}

void AcpClient::handleTodoWrite(const QJsonValue &id, const QJsonObject &params)
{
    const QJsonArray todos = params.value(u"todos"_s).toArray();
    if (todos.isEmpty()) {
        replyError(id, kInvalidParams, u"todos must be a non-empty array of {content, status}."_s);
        return;
    }
    QJsonArray stored;
    int index = 0;
    for (const QJsonValue &value : todos) {
        const QJsonObject todo = value.toObject();
        const QString content = todo.value(u"content"_s).toString().trimmed();
        if (content.isEmpty()) {
            replyError(id, kInvalidParams, u"Every todo needs a content string (item %1 is empty)."_s.arg(index + 1));
            return;
        }
        QString status = todo.value(u"status"_s).toString().trimmed().toLower().replace(u'-', u'_');
        if (status.isEmpty()) {
            status = u"pending"_s;
        }
        if (status != u"pending"_s && status != u"in_progress"_s && status != u"completed"_s) {
            replyError(id, kInvalidParams,
                       u"Invalid status '%1' for todo %2: use pending, in_progress, or completed."_s.arg(status, content.left(40)));
            return;
        }
        QJsonObject step;
        step.insert(u"id"_s, QString::number(++index));
        step.insert(u"description"_s, content);
        step.insert(u"completed"_s, status == u"completed"_s);
        step.insert(u"inProgress"_s, status == u"in_progress"_s);
        stored.append(step);
    }
    if (m_todosSetter) {
        m_todosSetter(stored);
    }
    Q_EMIT todosChanged(stored);
    QJsonObject result;
    result.insert(u"content"_s, u"Task list updated (%1 item(s))."_s.arg(stored.size()));
    result.insert(u"todos"_s, stored);
    replyResult(id, result);
}

void AcpClient::handleTodoRead(const QJsonValue &id, const QJsonObject &params)
{
    Q_UNUSED(params)
    const QJsonArray todos = m_todosGetter ? m_todosGetter() : QJsonArray();
    QStringList lines;
    if (todos.isEmpty()) {
        lines.append(u"(no todos yet)"_s);
    } else {
        for (const QJsonValue &value : todos) {
            const QJsonObject todo = value.toObject();
            const QString status = todo.value(u"completed"_s).toBool() ? u"completed"_s
                                   : (todo.value(u"inProgress"_s).toBool() ? u"in_progress"_s : u"pending"_s);
            lines.append(u"- [%1] %2"_s.arg(status, todo.value(u"description"_s).toString()));
        }
    }
    QJsonObject result;
    result.insert(u"content"_s, lines.join(u'\n'));
    result.insert(u"todos"_s, todos);
    replyResult(id, result);
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

    // Same contract as the native bash tool: running a command needs an
    // approval for this exact command line first.
    PermissionRequest request;
    request.toolName = u"bash"_s;
    request.toolCallId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.risk = ToolRisk::Execute;
    request.summary = args.isEmpty() ? command : command + u' ' + args.join(u' ');
    request.details = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
    if (!ensureMethodPermission(execGrantKey(command, args), std::move(request), id, u"terminal/create"_s, params)) {
        return;
    }

    QString cwd = params.value(u"cwd"_s).toString().trimmed();
    if (cwd.isEmpty()) {
        cwd = sessionCwd();
    } else if (cwd.startsWith(u"file:"_s, Qt::CaseInsensitive)) {
        const QUrl url(cwd);
        if (url.isLocalFile()) {
            cwd = url.toLocalFile();
        }
    }
    QString error;
    if (!pathAllowed(cwd, false, &error)) {
        replyError(id, kServerError, error);
        return;
    }
    const QString resolvedCwd = currentSandbox().resolve(cwd, &error, false);
    if (resolvedCwd.isEmpty()) {
        replyError(id, kServerError, error);
        return;
    }
    const Sandbox sandbox = currentSandbox();

    auto *term = new Terminal;
    term->id = u"term_%1"_s.arg(m_nextTerminal++);
    // Honour the agent's requested limit, including a small one. Zero/negative
    // would otherwise disable the cap; use the documented 1 MiB default for
    // those, and a 4 MiB ceiling for memory safety.
    const int requestedLimit = params.value(u"outputByteLimit"_s).toInt(1024 * 1024);
    term->outputByteLimit = requestedLimit <= 0 ? 1024 * 1024 : qMin(requestedLimit, 4 * 1024 * 1024);
    term->process = new QProcess(this);
    term->process->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_UNIX
    // Same session trick as the native bash tool: terminal/kill has to take
    // down everything the command started, not just the shell we launched.
    term->process->setChildProcessModifier([]() {
        ::setsid();
    });
#endif
    term->process->setWorkingDirectory(resolvedCwd);
    // Terminal commands run on behalf of a remote agent. Start from the
    // sandbox's scrubbed environment so ambient provider credentials and
    // session channels are not inherited implicitly; the agent can request
    // specific variables explicitly through `env`.
    QProcessEnvironment env = sandbox.profile() == SandboxProfile::Off
        ? sanitizedProcessEnvironment()
        : sandbox.processEnvironment();
    const QJsonObject extra = envValueToObject(params.value(u"env"_s));
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        env.insert(it.key(), it.value().toString());
    }
    term->process->setProcessEnvironment(env);

    // The lambdas look the terminal up by id instead of capturing the raw
    // pointer: the process outlives the Terminal (it is parented to the
    // client), so after a release a late signal would dereference freed
    // memory. A released terminal is simply absent from the map.
    const QString termId = term->id;
    connect(term->process, &QProcess::readyRead, this, [this, termId] {
        Terminal *term = m_terminals.value(termId);
        if (!term || !term->process) {
            return;
        }
        appendTerminalOutput(term, term->process->readAll());
    });
    connect(term->process, &QProcess::finished, this, [this, termId](int exitCode, QProcess::ExitStatus) {
        markTerminalExited(m_terminals.value(termId), exitCode, QString());
    });
    connect(term->process, &QProcess::errorOccurred, this, [this, termId](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            markTerminalExited(m_terminals.value(termId), 127, QString());
        }
    });

    QString wrapError;
    QStringList wrapped;
    if (args.isEmpty()) {
        // A lone command string is a shell line. Pass it as one `-c` argument
        // so inner quotes stay part of the script.
        wrapped = sandbox.wrapCommand(command, &wrapError, resolvedCwd);
    } else {
        // ACP argv stays argv. wrapArgv never rewrites this into `bash -c '…'`.
        wrapped = sandbox.wrapArgv(command, args, &wrapError, resolvedCwd);
    }
    if (wrapped.isEmpty()) {
        replyError(id, kServerError, wrapError.isEmpty() ? u"Failed to wrap the terminal command."_s : wrapError);
        delete term->process;
        delete term;
        return;
    }

    // Agents are not required to call terminal/release. Bound the retained
    // terminals so a long session cannot accumulate processes and output
    // buffers indefinitely; reclaim the oldest exited one first.
    constexpr int kMaxRetainedTerminals = 24;
    while (m_terminals.size() >= kMaxRetainedTerminals) {
        auto victim = m_terminals.end();
        for (auto it = m_terminals.begin(); it != m_terminals.end(); ++it) {
            if (it.value() && it.value()->exited) {
                victim = it;
                break;
            }
        }
        if (victim == m_terminals.end()) {
            break;
        }
        Terminal *old = victim.value();
        m_terminals.erase(victim);
        const QString linkedTool = m_terminalTools.take(old->id);
        if (!linkedTool.isEmpty()) {
            m_toolTerminals.remove(linkedTool);
        }
        if (old->process) {
            old->process->deleteLater();
        }
        delete old;
    }
    if (m_terminals.size() >= kMaxRetainedTerminals) {
        replyError(id, kServerError, u"Too many terminals are open; release one before creating another."_s);
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
    if (!term || !term->process) {
        replyError(id, kServerError, u"Unknown terminal '%1'."_s.arg(terminalId));
        return;
    }
    appendTerminalOutput(term, term->process->readAll());
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
    if (!term || !term->process) {
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
    if (!term || !term->process) {
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
    if (!term || !term->process) {
        // The entry was taken out of the map, so it must be freed even when
        // the command has to fail.
        delete term;
        replyError(id, kServerError, u"Unknown terminal '%1'."_s.arg(terminalId));
        return;
    }
    killTerminal(term, true);
    term->process->deleteLater();
    delete term;
    const QString linkedTool = m_terminalTools.take(terminalId);
    if (!linkedTool.isEmpty()) {
        m_toolTerminals.remove(linkedTool);
    }
    replyResult(id, QJsonObject());
}

void AcpClient::handleTerminalSendInput(const QJsonValue &id, const QJsonObject &params)
{
    const QString terminalId = params.value(u"terminalId"_s).toString();
    Terminal *term = m_terminals.value(terminalId);
    if (!term || !term->process) {
        replyError(id, kServerError, u"Unknown terminal '%1'."_s.arg(terminalId));
        return;
    }
    if (term->exited || term->process->state() == QProcess::NotRunning) {
        replyError(id, kServerError, u"Terminal '%1' has already exited."_s.arg(terminalId));
        return;
    }
    const QString input = params.contains(u"input"_s)
        ? params.value(u"input"_s).toString()
        : params.value(u"data"_s).toString();
    const QByteArray bytes = input.toUtf8();
    qsizetype written = 0;
    while (written < bytes.size()) {
        const qint64 n = term->process->write(bytes.constData() + written, bytes.size() - written);
        if (n <= 0) {
            replyError(id, kServerError, u"Could not write to terminal '%1'."_s.arg(terminalId));
            return;
        }
        written += n;
    }
    if (boolishValue(params.value(u"eof"_s), false)) {
        term->process->closeWriteChannel();
    }
    replyResult(id, QJsonObject{{u"bytesWritten"_s, static_cast<qint64>(written)}});
}

void AcpClient::appendTerminalOutput(Terminal *term, const QByteArray &chunk)
{
    if (chunk.isEmpty()) {
        return;
    }
    term->output.append(chunk);
    if (term->outputByteLimit > 0
        && term->output.size() > term->outputByteLimit + 64 * 1024) {
        // Trim in blocks rather than calling right() on every chunk once the
        // cap is reached; a chatty command would otherwise copy the whole
        // retained buffer for every new fragment.
        term->truncated = true;
        int drop = term->output.size() - term->outputByteLimit;
        while (drop < term->output.size()
               && (static_cast<unsigned char>(term->output.at(drop)) & 0xC0) == 0x80) {
            ++drop;
        }
        term->output.remove(0, drop);
    }
    // A terminal embedded in a running tool call shows its output in that card
    // while the command runs. Decoding and emitting the whole accumulated
    // buffer on every chunk was quadratic; a small output still updates
    // immediately, a large one is coalesced to ~10 updates/second.
    const auto owner = m_terminalTools.constFind(term->id);
    if (owner != m_terminalTools.constEnd()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (term->output.size() <= 4096 || now - term->lastProgressMs >= 100) {
            term->lastProgressMs = now;
            const QString text = QString::fromUtf8(term->output);
            m_toolStreamed.insert(owner.value(), capToolStream(text));
            Q_EMIT toolProgress(owner.value(), text);
        }
    }
}

void AcpClient::linkToolTerminals(const QString &toolCallId, const QJsonValue &content)
{
    if (toolCallId.isEmpty() || (!content.isArray() && !content.isObject())) {
        return;
    }
    const QJsonArray blocks = content.isArray() ? content.toArray() : QJsonArray{content};
    for (const QJsonValue &item : blocks) {
        const QJsonObject block = item.toObject();
        if (block.value(u"type"_s).toString() != u"terminal"_s) {
            continue;
        }
        const QString terminalId = block.value(u"terminalId"_s).toString();
        if (terminalId.isEmpty() || m_terminalTools.contains(terminalId)) {
            continue;
        }
        // A tool can switch terminals; remove the reverse link of the old one
        // or it keeps streaming into a card that no longer owns it.
        const QString previousTerminal = m_toolTerminals.value(toolCallId);
        if (!previousTerminal.isEmpty() && previousTerminal != terminalId) {
            m_terminalTools.remove(previousTerminal);
        }
        m_terminalTools.insert(terminalId, toolCallId);
        m_toolTerminals.insert(toolCallId, terminalId);
        // The command may have printed before the agent announced the terminal
        // in a tool call; that output belongs in the card as well.
        if (const Terminal *term = m_terminals.value(terminalId)) {
            if (!term->output.isEmpty()) {
                const QString text = capToolStream(QString::fromUtf8(term->output));
                m_toolStreamed.insert(toolCallId, text);
                Q_EMIT toolProgress(toolCallId, text);
            }
        }
    }
}

void AcpClient::forgetToolTerminal(const QString &toolCallId)
{
    const QString terminalId = m_toolTerminals.take(toolCallId);
    if (!terminalId.isEmpty()) {
        m_terminalTools.remove(terminalId);
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
    // Always publish the final buffer, even when the throttle skipped the
    // last chunk.
    if (const auto owner = m_terminalTools.constFind(term->id); owner != m_terminalTools.constEnd()) {
        const QString text = QString::fromUtf8(term->output);
        m_toolStreamed.insert(owner.value(), capToolStream(text));
        Q_EMIT toolProgress(owner.value(), text);
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
    // Disconnect signals to prevent callbacks after process deletion
    term->process->disconnect(this);
    if (term->process->state() != QProcess::NotRunning) {
#ifdef Q_OS_UNIX
        // The process leads its own session (see handleTerminalCreate), so the
        // negative pid reaches the whole tree. If setsid() failed there is no
        // such group and this is a no-op, leaving terminate() below to reap
        // the child itself. The pid is captured before the kill: once the
        // process is reaped, processId() no longer reports it.
        const qint64 pid = term->process->processId();
        if (pid > 0) {
            ::kill(-static_cast<pid_t>(pid), SIGTERM);
        }
#endif
        term->process->terminate();
        if (!term->process->waitForFinished(200)) {
            term->process->kill();
            term->process->waitForFinished(300);
        }
#ifdef Q_OS_UNIX
        // A grandchild that ignored SIGTERM must not outlive the terminal.
        if (pid > 0) {
            ::kill(-static_cast<pid_t>(pid), SIGKILL);
        }
#endif
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
    // Callbacks below may try to issue follow-up requests (model fallback,
    // config retry). Reject those instead of letting them add a live request
    // after the map was cleared; such a request would outlive the process and
    // fire its timeout against the next session.
    m_failingAll = true;
    // A prompt queued while the handshake was still starting must not be
    // replayed into the next turn after a failure.
    m_queued.clear();
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
    m_failingAll = false;
}

void AcpClient::finishPrompt(const QString &stopReason, const QString &error)
{
    m_promptId = 0;
    // Once-grants are per-turn consent. Keeping them let an approval from an
    // earlier turn auto-approve a matching write/command later, before the
    // active mode ever got a chance to inspect it.
    m_onceGrants.clear();
    // A client method parked in this turn cannot be answered after it ended;
    // close it and its card instead of promoting it into the next turn.
    cancelParkedMethods();
    // The turn is over, so a pending cancellation grace window must not fire.
    if (m_cancelGraceTimer) {
        m_cancelGraceTimer->stop();
        m_cancelGraceTimer->deleteLater();
        m_cancelGraceTimer = nullptr;
    }
    if (!error.isEmpty()) {
        // Tool cards still open belong to the turn that just failed; close
        // them now instead of letting the next turn's finishLiveTools emit
        // them as orphan results.
        finishLiveTools(error);
        Q_EMIT failed(error);
        return;
    }
    if (!m_liveTools.isEmpty()) {
        finishLiveTools(u"The turn ended before this tool reported completion."_s);
    }
    Q_EMIT promptFinished(stopReason.isEmpty() ? u"end_turn"_s : stopReason, m_currentText, m_toolCalls);
    if (!m_queued.isEmpty() && m_state == State::Ready) {
        const QueuedPrompt next = m_queued.takeFirst();
        sendPromptNow(next.text);
    }
}

bool AcpClient::isTransientAgentError(const QString &error) const
{
    const QString lower = error.toLower();
    // Phrasings agents report when their own HTTP stream to the model
    // provider breaks mid-response. Rust agents surface hyper/reqwest
    // transport errors ("peer closed connection without sending complete
    // message body (incomplete chunked read)"), Node agents surface
    // undici/Socket errors. Auth and protocol failures are deliberately not
    // listed: retrying them only repeats the same failure.
    return lower.contains(u"peer closed connection"_s)
        || lower.contains(u"incomplete chunked read"_s)
        || lower.contains(u"incomplete message body"_s)
        || lower.contains(u"incomplete headers"_s)
        || lower.contains(u"connection reset"_s)
        || lower.contains(u"connection closed"_s)
        || lower.contains(u"connection error"_s)
        || lower.contains(u"broken pipe"_s)
        || lower.contains(u"epipe"_s)
        || lower.contains(u"socket hang up"_s)
        || lower.contains(u"fetch failed"_s)
        || lower.contains(u"unexpected eof"_s)
        || lower.contains(u"unexpected end of stream"_s)
        || lower.contains(u"timed out"_s)
        || lower.contains(u"timeout"_s)
        || lower.contains(u"network error"_s)
        || lower.contains(u"temporarily unavailable"_s)
        || lower.contains(u"overloaded"_s)
        || lower.contains(u"rate limit"_s)
        || lower.contains(u"too many requests"_s)
        || lower.contains(u"service unavailable"_s)
        || lower.contains(u"bad gateway"_s)
        || lower.contains(u"gateway timeout"_s)
        || lower.contains(u"model unreachable"_s)
        || lower.contains(u"remote end closed connection"_s)
        || lower.contains(u"error 502"_s)
        || lower.contains(u"error 503"_s)
        || lower.contains(u"error 504"_s)
        || lower.contains(u"internal server error"_s);
}

bool AcpClient::scheduleTurnRetry(const QString &error)
{
    if (!isTransientAgentError(error)) {
        return false;
    }
    m_retryIsOutputLimit = false;
    return scheduleTurnRetryWithMessage(u"The ACP agent hit a transient error (%1)."_s.arg(error.simplified().left(120)));
}

bool AcpClient::scheduleOutputLimitRetry()
{
    m_retryIsOutputLimit = true;
    return scheduleTurnRetryWithMessage(u"The ACP agent hit its model output token limit before finishing."_s);
}

bool AcpClient::scheduleTurnRetryWithMessage(const QString &reason)
{
    if (!m_settings.enableAutoRetry || m_shuttingDown || m_state != State::Ready || m_sessionId.isEmpty()) {
        return false;
    }
    const int maxAttempts = qMax(1, m_settings.maxRetryAttempts);
    if (m_turnRetryAttempt >= maxAttempts) {
        return false;
    }
    ++m_turnRetryAttempt;

    int delaySeconds = qMax(1, m_settings.baseRetryDelaySeconds);
    if (m_settings.retryStrategy.compare(u"fixed"_s, Qt::CaseInsensitive) != 0) {
        // Exponential backoff: base, 2*base, 4*base... The shift is bounded so
        // the doubling cannot overflow.
        delaySeconds = qMin(delaySeconds << qMin(m_turnRetryAttempt - 1, 8), 3600);
    }
    const int cap = m_settings.maxRetryDelaySeconds > 0 ? m_settings.maxRetryDelaySeconds : 300;
    delaySeconds = qMin(delaySeconds, cap);

    // The text streamed before the failure belongs to the attempt that
    // failed. The retry restarts the turn, so the transcript must drop it.
    if (!m_currentText.isEmpty()) {
        m_currentText.clear();
        Q_EMIT textReset();
    }
    m_promptId = 0;

    const QString message = u"%1 Retrying in %2s (attempt %3 of %4)."_s
                                .arg(reason)
                                .arg(delaySeconds)
                                .arg(m_turnRetryAttempt)
                                .arg(maxAttempts);
    Q_EMIT retryStatus(message, m_turnRetryAttempt, maxAttempts, delaySeconds);
    Q_EMIT logMessage(message);

    if (m_turnRetryTimer) {
        m_turnRetryTimer->stop();
        m_turnRetryTimer->deleteLater();
        m_turnRetryTimer = nullptr;
    }
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &AcpClient::executeTurnRetry);
    timer->start(delaySeconds * 1000);
    m_turnRetryTimer = timer;
    m_turnRetryPending = true;
    return true;
}

void AcpClient::executeTurnRetry()
{
    m_turnRetryPending = false;
    if (m_shuttingDown || m_state != State::Ready || m_sessionId.isEmpty() || m_promptId != 0) {
        // The process died or the turn was closed some other way (its failure
        // was already reported); a retry would inject a turn nobody asked for.
        return;
    }
    const int attempt = m_turnRetryAttempt;
    const QString retryPrompt = turnRetryPrompt();
    sendPromptNow(retryPrompt);
    // sendPromptNow resets the budget for a fresh turn; the retries spent on
    // this one must survive it.
    m_turnRetryAttempt = attempt;
}

void AcpClient::stopTurnRetry()
{
    if (m_turnRetryTimer) {
        m_turnRetryTimer->stop();
        m_turnRetryTimer->deleteLater();
        m_turnRetryTimer = nullptr;
    }
    m_turnRetryPending = false;
}

QString AcpClient::turnRetryPrompt() const
{
    // The failed turn is already part of the agent's session, so the retry asks
    // it to continue instead of replaying the whole request. Agents that roll
    // a failed turn back still see the task through the reminder.
    QString prompt;
    if (m_retryIsOutputLimit) {
        // The turn ended at the model output limit, typically while the agent
        // generated one oversized tool call — a whole large file in a single
        // write. The cut-off completion never ran, so the task is still open;
        // steer the continuation to bound each tool call so it can finish.
        prompt = u"[KateAI] Your previous turn was cut off because it reached the model output token limit "
                 u"before it finished — typically while producing a single oversized tool call, like a whole "
                 u"large file in one write. Continue the user's task from where it stopped; do not repeat "
                 u"steps that already completed. If the task involves writing a large file, do not send one "
                 u"full-file write in a single tool call: create the file with the first chunk, then append "
                 u"the remaining sections with several smaller write or edit tool calls, a few hundred lines "
                 u"at a time, verifying each chunk before continuing. If no file is involved, give a shorter, "
                 u"more focused answer. "_s;
    } else {
        prompt = u"[KateAI] Your previous turn was interrupted by a transient network error before it "
                 u"finished. Continue the user's task from the point where it stopped; do not repeat "
                 u"steps that already completed. "_s;
        if (acpAgentIsMistralVibe(m_settings)) {
            // Mistral Vibe can hit its model output limit while producing a single
            // large file-write tool call. On retry, steer it to bound each tool
            // call so it can make progress instead of generating the same oversized
            // payload again.
            prompt += u"For large files, split the work into small, incremental edits or writes. Do not send one "
                      u"full-file write in a single tool call; verify each chunk before continuing. "_s;
        }
    }
    const QString original = m_taskPrompt.simplified();
    if (original.length() > 1000) {
        prompt += u"If nothing was started yet, the request was: "_s + original.left(1000) + u"…"_s;
    } else if (!original.isEmpty()) {
        prompt += u"If nothing was started yet, the request was: "_s + original;
    }
    return prompt;
}

} // namespace KateAi
