/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "tools.h"

#include "harness/toolresults.h"
#include "mcp.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{
// A command that writes gigabytes must not fill the memory: the output is
// kept while the command runs, and the ceiling is applied here.
constexpr int kMaxBashOutputChars = 80000; // keep in sync with clip()
} // namespace

// Unified tool argument parsing - canonicalizes arguments from ToolCall
// Prefers pre-parsed `arguments` object, falls back to parsing `argumentsJson`
// Returns empty object on parse failure (error output parameter is set)
QJsonObject parseToolArgs(const ToolCall &call, QString *error)
{
    // First try to use the parsed arguments directly if available
    if (!call.arguments.isEmpty()) {
        return call.arguments;
    }
    
    // If no parsed arguments, try to parse from JSON string
    if (call.argumentsJson.trimmed().isEmpty()) {
        return {};
    }
    
    // Parse the JSON string and validate it's a proper object
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(call.argumentsJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = u"Invalid tool arguments: %1"_s.arg(parseError.errorString());
        }
        return {};
    }
    if (!doc.isObject()) {
        if (error) {
            *error = u"Invalid tool arguments: expected a JSON object."_s;
        }
        return {};
    }
    return doc.object();
}

static QString clip(const QString &text, int maxChars = 80000)
{
    if (text.size() <= maxChars) {
        return text;
    }
    // Keep both ends: the head carries the beginning of the output (headers,
    // first errors) and the tail carries what the command last said, which is
    // where a failure summary or a final total lives. A head-only clip throws
    // that half away, and it is the half the model most needs.
    const int half = maxChars / 2;
    return text.left(half) + u"\n... truncated %1 of %2 characters ...\n"_s.arg(text.size() - maxChars).arg(text.size())
        + text.right(half);
}

// Stable identity for the read ledger: the canonical path when it exists, and
// a cleaned absolute path otherwise. Both read and write resolve through the
// sandbox, so the two sides agree even when the model spells the path
// differently.
static QString ledgerKey(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return canonical;
    }
    return QDir::cleanPath(info.absoluteFilePath());
}

static bool jsonBool(const QJsonValue &value, bool fallback = false)
{
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isDouble()) {
        return value.toInt() != 0;
    }
    if (value.isString()) {
        const QString s = value.toString().trimmed().toLower();
        if (s == u"true"_s || s == u"1"_s || s == u"yes"_s) {
            return true;
        }
        if (s == u"false"_s || s == u"0"_s || s == u"no"_s) {
            return false;
        }
    }
    return fallback;
}

// An integer tool argument, accepting a JSON number or a numeric string:
// QJsonValue::toInt() silently yields the fallback for "5", which would
// quietly drop the model's own offset/limit.
static int jsonInt(const QJsonValue &value, int fallback)
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

static QString quoteShellArg(const QString &s)
{
    if (s.isEmpty()) {
        return {};
    }
    static const QRegularExpression safe(u"^[A-Za-z0-9_./:@%+=,-]+$"_s);
    if (safe.match(s).hasMatch()) {
        return s;
    }
    QString escaped = s;
    escaped.replace(u"'"_s, u"'\\''"_s);
    return u"'%1'"_s.arg(escaped);
}

QString shellCommandFor(const QString &toolName, const QJsonObject &args)
{
    const QString canonical = canonicalToolName(toolName);
    const QString key = canonical.trimmed().toLower();
    // OpenCode & Antigravity ACP aliases resolve to the same summaries as the native tools.
    auto argPath = [&args]() -> QString {
        for (const QString &k : {u"path"_s, u"Path"_s, u"file_path"_s, u"filePath"_s, u"file"_s, u"File"_s,
                                 u"target_file"_s, u"targetFile"_s, u"TargetFile"_s,
                                 u"absolute_path"_s, u"AbsolutePath"_s, u"DirectoryPath"_s,
                                 u"SearchPath"_s, u"SearchDirectory"_s, u"uri"_s}) {
            const QString p = args.value(k).toString();
            if (!p.isEmpty()) {
                return p;
            }
        }
        return {};
    };
    if (canonical == u"read_file"_s || key == u"read_file"_s || toolName == u"fs/read_text_file"_s || key == u"read"_s) {
        const QString path = quoteShellArg(argPath());
        const int startLine = args.value(u"StartLine"_s).toInt(args.value(u"start_line"_s).toInt(0));
        const int endLine = args.value(u"EndLine"_s).toInt(args.value(u"end_line"_s).toInt(0));
        const int offset = startLine > 0 ? startLine : args.value(u"offset"_s).toInt(args.value(u"line"_s).toInt(1));
        const int limit = (startLine > 0 && endLine >= startLine)
            ? (endLine - startLine + 1)
            : args.value(u"limit"_s).toInt(0);
        if (limit > 0) {
            const int end = offset + limit - 1;
            if (!path.isEmpty()) {
                return u"sed -n '%1,%2p' %3"_s.arg(offset).arg(end).arg(path);
            }
            return u"sed -n '%1,%2p'"_s.arg(offset).arg(end);
        }
        if (offset > 1) {
            if (!path.isEmpty()) {
                return u"tail -n +%1 %2"_s.arg(offset).arg(path);
            }
            return u"tail -n +%1"_s.arg(offset);
        }
        if (!path.isEmpty()) {
            return u"cat %1"_s.arg(path);
        }
        return u"read_file"_s;
    }
    if (canonical == u"write_file"_s || toolName == u"fs/write_text_file"_s || key == u"write"_s) {
        const QString path = argPath();
        if (!path.isEmpty()) {
            return u"cat > %1"_s.arg(quoteShellArg(path));
        }
        return u"write_file"_s;
    }
    if (canonical == u"edit_file"_s || key == u"edit"_s || key == u"patch"_s || key == u"str_replace_editor"_s || key == u"apply_patch"_s) {
        const QString path = argPath();
        if (!path.isEmpty()) {
            return u"patch %1"_s.arg(quoteShellArg(path));
        }
        return u"edit_file"_s;
    }
    if (canonical == u"multi_edit_file"_s || toolName == u"multi_replace_file_content"_s || key == u"multi_edit"_s) {
        const QString rawPath = args.value(u"path"_s).toString().isEmpty()
            ? args.value(u"TargetFile"_s).toString()
            : args.value(u"path"_s).toString();
        const QString path = quoteShellArg(rawPath);
        QJsonArray edits = args.value(u"edits"_s).toArray();
        if (edits.isEmpty()) {
            edits = args.value(u"chunks"_s).toArray();
        }
        if (edits.isEmpty()) {
            edits = args.value(u"ReplacementChunks"_s).toArray();
        }
        if (path.isEmpty()) {
            return u"patch (%1 edits)"_s.arg(edits.size());
        }
        return u"patch %1 (%2 edits)"_s.arg(path).arg(edits.size());
    }
    if (canonical == u"list_dir"_s || toolName == u"fs/list_directory"_s || key == u"list"_s || key == u"readdir"_s) {
        const QString path = argPath();
        return path.isEmpty() ? u"ls"_s : u"ls %1"_s.arg(quoteShellArg(path));
    }
    if (canonical == u"grep"_s || toolName == u"fs/grep"_s || key == u"search"_s) {
        QString cmd = u"grep -n"_s;
        if (jsonBool(args.value(u"case_insensitive"_s))) {
            cmd += u" -i"_s;
        }
        const int context = args.value(u"context"_s).toInt(0);
        if (context > 0) {
            cmd += u" -C %1"_s.arg(context);
        }
        QString pattern = args.value(u"pattern"_s).toString();
        if (pattern.isEmpty()) pattern = args.value(u"Query"_s).toString();
        if (pattern.isEmpty()) pattern = args.value(u"query"_s).toString();
        cmd += u" %1"_s.arg(quoteShellArg(pattern));
        const QString path = argPath();
        if (!path.isEmpty()) {
            cmd += u" %1"_s.arg(quoteShellArg(path));
        }
        const QString glob = args.value(u"glob"_s).toString();
        if (!glob.isEmpty()) {
            cmd += u" --include=%1"_s.arg(quoteShellArg(glob));
        }
        return cmd;
    }
    if (canonical == u"glob"_s || toolName == u"fs/glob"_s) {
        QString pattern = args.value(u"pattern"_s).toString();
        if (pattern.isEmpty()) {
            pattern = args.value(u"Pattern"_s).toString();
        }
        if (pattern.isEmpty()) {
            pattern = args.value(u"glob"_s).toString();
        }
        if (pattern.isEmpty()) {
            return u"find ."_s;
        }
        return u"find . -name %1"_s.arg(quoteShellArg(pattern));
    }
    if (canonical == u"bash"_s || key == u"execute"_s || key == u"run_command"_s || toolName == u"terminal/exec"_s || toolName == u"shell/exec"_s) {
        QString command = args.value(u"command"_s).toString();
        if (command.isEmpty()) {
            command = args.value(u"CommandLine"_s).toString();
        }
        if (command.isEmpty()) {
            command = args.value(u"cmd"_s).toString();
        }
        if (command.isEmpty()) {
            command = args.value(u"script"_s).toString();
        }
        return command;
    }
    if (canonical == u"web_search"_s) {
        QString q = args.value(u"query"_s).toString();
        if (q.isEmpty()) q = args.value(u"Query"_s).toString();
        return q.isEmpty() ? u"web_search"_s : u"web_search %1"_s.arg(quoteShellArg(q.left(120)));
    }
    if (canonical == u"web_fetch"_s) {
        QString url = args.value(u"url"_s).toString();
        if (url.isEmpty()) url = args.value(u"Url"_s).toString();
        return url.isEmpty() ? u"web_fetch"_s : u"web_fetch %1"_s.arg(quoteShellArg(url.left(120)));
    }
    if (canonical == u"query_project_graph"_s) {
        const QString type = args.value(u"query_type"_s).toString();
        QString cmd = u"query_project_graph"_s;
        if (!type.isEmpty()) {
            cmd += u" %1"_s.arg(type);
        }
        const QString target = args.value(u"target"_s).toString();
        if (!target.isEmpty()) {
            cmd += u" %1"_s.arg(quoteShellArg(target));
        }
        return cmd;
    }
    if (canonical == u"new_task"_s) {
        QString mode = args.value(u"mode"_s).toString();
        if (mode.isEmpty()) mode = args.value(u"TypeName"_s).toString();
        QString description = args.value(u"description"_s).toString();
        if (description.isEmpty()) description = args.value(u"Role"_s).toString();
        const QString label = mode.isEmpty() ? QStringLiteral("code") : mode;
        // A delegation without a description still reads as a delegation, not
        // as a bare bracket pair with nothing behind it.
        return description.isEmpty() ? u"[%1]"_s.arg(label) : u"[%1] %2"_s.arg(label, description);
    }
    if (key == u"manage_task"_s) {
        const QString action = args.value(u"Action"_s).toString();
        const QString taskId = args.value(u"TaskId"_s).toString();
        if (!action.isEmpty() && !taskId.isEmpty()) {
            return u"manage_task %1 %2"_s.arg(action, taskId);
        }
        return action.isEmpty() ? u"manage_task"_s : u"manage_task %1"_s.arg(action);
    }
    if (key == u"run_workflow"_s) {
        QString desc = args.value(u"Description"_s).toString();
        if (desc.isEmpty()) desc = args.value(u"ScriptPath"_s).toString();
        return desc.isEmpty() ? u"run_workflow"_s : u"run_workflow %1"_s.arg(quoteShellArg(desc.left(80)));
    }
    if (key == u"swarm"_s) {
        const QString action = args.value(u"action"_s).toString();
        const QString name = args.value(u"name"_s).toString();
        if (!action.isEmpty() && !name.isEmpty()) {
            return u"swarm %1 %2"_s.arg(action, name);
        }
        return action.isEmpty() ? u"swarm"_s : u"swarm %1"_s.arg(action);
    }
    if (canonical == u"write_plan_file"_s) {
        return u"write plan file"_s;
    }
    if (canonical == u"suggest"_s) {
        // The first line of the advice reads better in a card header than a
        // bare tool name, so show that, capped.
        const QString firstLine = suggestionTextFromToolArguments(args).section(u'\n', 0, 0).trimmed();
        return firstLine.isEmpty() ? u"suggest"_s : u"suggest %1"_s.arg(quoteShellArg(firstLine.left(80)));
    }
    if (canonical == u"question"_s) {
        // The question is the whole point of the call, so it goes in the
        // header verbatim (capped) rather than being summarised as "question".
        const QString asked = questionTextFromToolArguments(args).section(u'\n', 0, 0).trimmed();
        return asked.isEmpty() ? u"question"_s : u"question %1"_s.arg(quoteShellArg(asked.left(120)));
    }
    if (canonical == u"debug_status"_s) {
        return u"debug status"_s;
    }
    if (canonical == u"debug_stack"_s) {
        const int tid = args.value(u"thread_id"_s).toInt(-1);
        return tid >= 0 ? u"debug stack (thread %1)"_s.arg(tid) : u"debug stack"_s;
    }
    if (canonical == u"debug_locals"_s) {
        const int lvl = args.value(u"frame_level"_s).toInt(-1);
        return lvl >= 0 ? u"debug locals (frame #%1)"_s.arg(lvl) : u"debug locals"_s;
    }
    if (canonical == u"debug_breakpoints"_s) {
        return u"debug breakpoints"_s;
    }
    if (canonical == u"debug_select_frame"_s) {
        return u"debug select frame #%1"_s.arg(args.value(u"frame_level"_s).toInt(0));
    }
    if (canonical == u"debug_select_thread"_s) {
        return u"debug select thread #%1"_s.arg(args.value(u"thread_id"_s).toInt(0));
    }
    if (canonical == u"debug_snapshot"_s) {
        const QString act = args.value(u"action"_s).toString(u"capture"_s);
        const QString label = args.value(u"label"_s).toString();
        return label.isEmpty() ? u"debug snapshot %1"_s.arg(act) : u"debug snapshot %1 %2"_s.arg(act, quoteShellArg(label));
    }
    if (canonical == u"debug_set_breakpoint"_s) {
        if (jsonBool(args.value(u"clear_all"_s))) {
            return u"debug clear all breakpoints"_s;
        }
        const QString verb = jsonBool(args.value(u"remove"_s)) ? u"remove"_s : u"set"_s;
        const QString bpPath = argPath();
        const int line = args.value(u"line"_s).toInt(0);
        return bpPath.isEmpty() ? u"debug %1 breakpoint"_s.arg(verb)
                                : u"debug %1 breakpoint %2:%3"_s.arg(verb, quoteShellArg(bpPath)).arg(line);
    }
    if (canonical == u"debug_step"_s) {
        const QString act = args.value(u"action"_s).toString();
        if (act == u"run_to_cursor"_s) {
            return u"debug run_to_cursor %1:%2"_s.arg(quoteShellArg(argPath())).arg(args.value(u"line"_s).toInt(0));
        }
        return act.isEmpty() ? u"debug step"_s : u"debug %1"_s.arg(act);
    }
    if (canonical == u"debug_eval"_s) {
        const QString expr = args.value(u"expression"_s).toString().left(100);
        return expr.isEmpty() ? u"debug eval"_s : u"debug eval %1"_s.arg(quoteShellArg(expr));
    }
    if (canonical == u"debug_session"_s) {
        const QString act = args.value(u"action"_s).toString();
        const QString target = args.value(u"target"_s).toString();
        if (!target.isEmpty()) {
            return u"debug %1 %2"_s.arg(act, quoteShellArg(target));
        }
        return act.isEmpty() ? u"debug session"_s : u"debug %1"_s.arg(act);
    }
    if (canonical == u"debug_console"_s) {
        const QString cmd = args.value(u"command"_s).toString().left(100);
        return cmd.isEmpty() ? u"debug console"_s : u"debug console %1"_s.arg(quoteShellArg(cmd));
    }
    if (canonical == u"build_targets"_s) {
        return u"build targets"_s;
    }
    if (canonical == u"build_run"_s) {
        const QString target = args.value(u"target"_s).toString();
        return target.isEmpty() ? u"kate build (default target)"_s : u"kate build %1"_s.arg(quoteShellArg(target));
    }
    if (canonical == u"terminal_run"_s) {
        const QString cmd = args.value(u"command"_s).toString().left(120);
        return cmd.isEmpty() ? u"terminal_run"_s : u"konsole: %1"_s.arg(cmd);
    }
    if (toolName.startsWith(u"mcp__"_s)) {
        // MCP tools carry their own argument shapes, so show them verbatim.
        return QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    }
    return toolName;
}

QString planFilePath(const QString &workspaceRoot)
{
    // A digest rather than the raw path: the temp name must be stable across
    // runs, must not leak the user's directory layout, and must be a legal
    // filename on every platform. qHash is not stable enough to build on.
    QString root = workspaceRoot;
    if (!root.isEmpty()) {
        const QString canonical = QFileInfo(root).canonicalFilePath();
        if (!canonical.isEmpty()) {
            root = canonical;
        }
    }
    const QByteArray digest = QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha1).toHex();
    // The shared temp directory is world-readable/writable. Put the plan in a
    // per-user directory with owner-only permissions so another account
    // cannot read the notes (which quote source) or pre-plant a symlink at
    // the predictable file name.
    const QByteArray user = QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString dir = QDir::tempPath() + u"/kateai-plans-"_s + QString::fromLatin1(user.left(10));
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return dir + u"/"_s + QString::fromLatin1(digest.left(16)) + u".md"_s;
}

static bool isNoisySearchPath(const QString &relativePath)
{
    // Match whole path segments, not substrings: "cmake-build/x" or
    // "src/mybuild/x" contain "build/" but are ordinary source directories.
    // Only directory segments are checked — the final segment is the file
    // name, and a file called "build" is worth searching.
    static const QSet<QString> noisy = {
        u".git"_s, u"node_modules"_s, u"build"_s, u"CMakeFiles"_s,
        u".kateai"_s, u".cache"_s, u"__pycache__"_s,
    };
    const QStringList segments = relativePath.split(u'/');
    for (int i = 0; i + 1 < segments.size(); ++i) {
        if (noisy.contains(segments.at(i))) {
            return true;
        }
    }
    return false;
}

static GraphNode *resolveGraphNode(ProjectGraph *graph, const QString &key)
{
    if (!graph || key.isEmpty()) {
        return nullptr;
    }
    if (GraphNode *direct = graph->getNode(key)) {
        return direct;
    }
    if (GraphNode *byPath = graph->findNodeByPath(key)) {
        return byPath;
    }
    GraphNode *byName = nullptr;
    int nameHits = 0;
    for (GraphNode *node : graph->getAllNodes()) {
        if (!node) {
            continue;
        }
        if (node->path == key || node->id.endsWith(key)) {
            return node;
        }
        if (node->name == key) {
            byName = node;
            ++nameHits;
        }
    }
    return nameHits == 1 ? byName : nullptr;
}

namespace
{
struct DiffOp {
    QChar kind; // ' ' equal, '-' removed (from before), '+' added (in after)
    int oldNo;  // 1-based line number in `before`, or 0
    int newNo;  // 1-based line number in `after`, or 0
    QString text;
};

// Longest-common-subsequence line diff, equivalent output to a Myers diff.
// Returns an edit script with 1-based line numbers so callers can render real
// unified hunks instead of a misleading positional comparison.
QList<DiffOp> computeLineDiff(const QStringList &a, const QStringList &b)
{
    const int n = a.size();
    const int m = b.size();
    QVector<QVector<int>> lcs(n + 1, QVector<int>(m + 1, 0));
    for (int i = n - 1; i >= 0; --i) {
        for (int j = m - 1; j >= 0; --j) {
            lcs[i][j] = (a.at(i) == b.at(j)) ? lcs[i + 1][j + 1] + 1
                                             : qMax(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }
    QList<DiffOp> ops;
    ops.reserve(n + m);
    int i = 0;
    int j = 0;
    while (i < n && j < m) {
        if (a.at(i) == b.at(j)) {
            ops.append({u' ', i + 1, j + 1, a.at(i)});
            ++i;
            ++j;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            ops.append({u'-', i + 1, 0, a.at(i)});
            ++i;
        } else {
            ops.append({u'+', 0, j + 1, b.at(j)});
            ++j;
        }
    }
    while (i < n) {
        ops.append({u'-', i + 1, 0, a.at(i)});
        ++i;
    }
    while (j < m) {
        ops.append({u'+', 0, j + 1, b.at(j)});
        ++j;
    }
    return ops;
}
} // namespace

namespace
{

// A memory note uses a fixed directory and a validated slug. The model cannot
// name a path, so it cannot overwrite AGENTS.md or escape the memory folder.
QString memoryNotePath(const QJsonObject &args, const QString &workspace, QString *error)
{
    QString topic = args.value(u"topic"_s).toString().trimmed();
    static const QRegularExpression valid(u"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"_s);
    if (!valid.match(topic).hasMatch() || topic == u"."_s || topic == u".."_s) {
        if (error) {
            *error = u"topic must be a short slug of letters, digits, '.', '_' or '-'."_s;
        }
        return {};
    }
    if (!topic.endsWith(u".md"_s)) {
        topic += u".md"_s;
    }
    const QString scope = args.value(u"scope"_s).toString(u"project"_s);
    QString directory;
    if (scope == u"user"_s) {
        QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        if (config.isEmpty()) {
            config = QDir::home().filePath(u".config"_s);
        }
        directory = config + u"/kateai/memory"_s;
    } else if (scope == u"project"_s) {
        if (workspace.isEmpty()) {
            if (error) {
                *error = u"no workspace is open, so there is no project memory directory."_s;
            }
            return {};
        }
        directory = workspace + u"/.kateai/memory"_s;
    } else {
        if (error) {
            *error = u"scope must be 'project' or 'user'."_s;
        }
        return {};
    }
    return QDir(directory).filePath(topic);
}

QString yamlQuoted(QString value)
{
    value.replace(u'\\', u"\\\\"_s);
    value.replace(u'"', u"\\\""_s);
    value.replace(u'\r', u' ');
    value.replace(u'\n', u' ');
    return u"\""_s + value + u"\""_s;
}

QString memoryNoteBody(const QJsonObject &args)
{
    const QString topic = args.value(u"topic"_s).toString().trimmed();
    QString content = args.value(u"content"_s).toString().trimmed();
    if (content.size() > 32 * 1024) {
        content = content.left(32 * 1024) + u"\n... (memory note truncated)"_s;
    }
    const QString description = content.section(u'\n', 0, 0).simplified().left(160);
    return u"---\ndescription: "_s + yamlQuoted(description)
        + u"\nalwaysApply: true\n---\n# "_s + topic
        + u"\n\n"_s + content + u"\n"_s;
}

// Resolve and policy-check a memory target. Project notes go through the
// sandbox's normal workspace confinement. User notes live outside the
// workspace by design, but still obey read-only and deny rules.
QString memoryWriteTarget(const Sandbox &sandbox, const QJsonObject &args, QString *error)
{
    const QString scope = args.value(u"scope"_s).toString(u"project"_s);
    const QString path = memoryNotePath(args, sandbox.workspaceRoot(), error);
    if (path.isEmpty()) {
        return {};
    }
    if (scope == u"project"_s) {
        return sandbox.resolve(path, error, true);
    }
    if (sandbox.profile() == SandboxProfile::ReadOnly) {
        if (error) {
            *error = u"The sandbox is read-only; memory notes cannot be written."_s;
        }
        return {};
    }
    if (sandbox.isDenied(path)) {
        if (error) {
            *error = u"Access to this memory path is blocked by a deny rule: %1"_s.arg(path);
        }
        return {};
    }
    return path;
}

bool readMemoryFile(const QString &path, const DocumentBridge *bridge, QString *contents)
{
    if (bridge) {
        return bridge->readDocument(path, contents);
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    if (contents) {
        *contents = QString::fromUtf8(file.readAll());
    }
    return true;
}

// --- edit canonicalisation --------------------------------------------------
//
// Exact matching stays the primary contract. When it fails, a second pass
// compares text with smart quotes/punctuation folded, CRLF normalised,
// trailing whitespace removed, and (optionally) runs of spaces/tabs collapsed.
// The map keeps byte fidelity: the replacement is applied to the original
// string at the mapped original offsets, so the rest of the file is untouched.

QChar foldPunctuation(QChar c)
{
    switch (c.unicode()) {
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x201B:
        return u'\'';
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x201F:
        return u'"';
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2015:
    case 0x2212:
        return u'-';
    case 0x00A0:
        return u' ';
    default:
        return c;
    }
}

struct CanonicalText {
    QString text;
    QVector<int> map;
};

CanonicalText canonicalizeForMatch(const QString &input, bool collapseWhitespace)
{
    CanonicalText out;
    out.text.reserve(input.size());
    out.map.reserve(input.size());
    bool previousWasSpace = false;
    for (int i = 0; i < input.size(); ++i) {
        const QChar c = input.at(i);
        if (c == u'\r') {
            if (i + 1 < input.size() && input.at(i + 1) == u'\n') {
                continue; // CRLF reads as LF
            }
            out.text += u'\n';
            out.map.append(i);
            previousWasSpace = false;
            continue;
        }
        if (c == u'\n') {
            out.text += u'\n';
            out.map.append(i);
            previousWasSpace = false;
            continue;
        }
        if (c == u' ' || c == u'\t') {
            int look = i;
            while (look < input.size() && (input.at(look) == u' ' || input.at(look) == u'\t')) {
                ++look;
            }
            if (look >= input.size() || input.at(look) == u'\n' || input.at(look) == u'\r') {
                continue; // trailing whitespace is not significant
            }
            if (collapseWhitespace && previousWasSpace) {
                continue;
            }
            previousWasSpace = true;
            out.text += u' ';
            out.map.append(i);
            continue;
        }
        previousWasSpace = false;
        const QChar folded = foldPunctuation(c);
        // NFKC per character keeps the mapping exact for the common cases
        // (compatibility forms that expand are rare in code and rules).
        const QString normalized = QString(folded).normalized(QString::NormalizationForm_KC);
        for (const QChar n : normalized) {
            out.text += n;
            out.map.append(i);
        }
    }
    return out;
}

QList<QPair<int, int>> canonicalMatches(const QString &contents, const QString &oldString)
{
    QList<QPair<int, int>> matches;
    const CanonicalText canonicalContents = canonicalizeForMatch(contents, true);
    const CanonicalText canonicalOld = canonicalizeForMatch(oldString, true);
    if (canonicalOld.text.isEmpty()) {
        return matches;
    }
    int from = 0;
    while (from <= canonicalContents.text.size() - canonicalOld.text.size()) {
        const int at = canonicalContents.text.indexOf(canonicalOld.text, from);
        if (at < 0) {
            break;
        }
        const int normalizedEnd = at + canonicalOld.text.size();
        const int originalStart = canonicalContents.map.value(at, 0);
        const int originalEnd = normalizedEnd < canonicalContents.map.size()
            ? canonicalContents.map.at(normalizedEnd)
            : contents.size();
        if (originalEnd > originalStart) {
            matches.append({originalStart, originalEnd});
        }
        from = at + qMax(1, canonicalOld.text.size());
    }
    return matches;
}

bool applyCanonicalEdit(const QString &contents, const QString &oldString,
                        const QString &newString, bool replaceAll,
                        QString *updated, int *replacements, QString *error)
{
    const QList<QPair<int, int>> matches = canonicalMatches(contents, oldString);
    if (matches.isEmpty()) {
        if (error) {
            *error = u"no match"_s;
        }
        return false;
    }
    if (matches.size() > 1 && !replaceAll) {
        if (error) {
            *error = u"ambiguous"_s;
        }
        return false;
    }
    QString result = contents;
    for (int i = matches.size() - 1; i >= 0; --i) {
        result.replace(matches.at(i).first, matches.at(i).second - matches.at(i).first, newString);
    }
    if (updated) {
        *updated = result;
    }
    if (replacements) {
        *replacements = matches.size();
    }
    return true;
}


} // namespace

QString unifiedDiff(const QString &path, const QString &before, const QString &after)
{    QStringList oldLines = before.split(u'\n');
    QStringList newLines = after.split(u'\n');
    // Text that ends with a newline splits into one extra empty element: that
    // element is the line terminator, not a line. Keeping it inflates every
    // count by one and appends an empty context line to each hunk.
    if (!oldLines.isEmpty() && oldLines.last().isEmpty()) {
        oldLines.removeLast();
    }
    if (!newLines.isEmpty() && newLines.last().isEmpty()) {
        newLines.removeLast();
    }
    // "".split('\n') invents a phantom line 1, so a new file would render with a
    // hunk header claiming the old file had one line. An empty side is zero
    // lines, which is also what the "<line before>,0" range below describes.
    if (before.isEmpty()) {
        oldLines.clear();
    }
    if (after.isEmpty()) {
        newLines.clear();
    }
    QString out = u"--- a/%1\n+++ b/%1\n"_s.arg(path);

    // The diff is O(n*m) in time and memory; a whole-file comparison of two
    // large files would allocate a huge matrix, so fall back to a summary.
    const qint64 cells = qint64(oldLines.size() + 1) * qint64(newLines.size() + 1);
    if (cells > 1000000) {
        out += u"@@ (file changed: %1 -> %2 lines) @@\n"_s.arg(oldLines.size()).arg(newLines.size());
        return out;
    }

    const QList<DiffOp> ops = computeLineDiff(oldLines, newLines);

    QList<int> changed;
    for (int k = 0; k < ops.size(); ++k) {
        if (ops.at(k).kind != u' ') {
            changed.append(k);
        }
    }
    if (changed.isEmpty()) {
        out += u"(no line-level changes)\n"_s;
        return out;
    }

    const int context = 3;
    const int maxOutputLines = 600;
    int emitted = 0;
    int idx = 0;
    while (idx < changed.size() && emitted < maxOutputLines) {
        // One hunk spans changes separated by no more than 2*context equal lines.
        const int first = changed.at(idx);
        int last = changed.at(idx);
        int nxt = idx + 1;
        while (nxt < changed.size() && changed.at(nxt) - last - 1 <= 2 * context) {
            last = changed.at(nxt);
            ++nxt;
        }
        const int from = qMax(0, first - context);
        const int to = qMin(ops.size(), last + context + 1);

        int oldStart = 0;
        int newStart = 0;
        int oldCount = 0;
        int newCount = 0;
        for (int k = from; k < to; ++k) {
            const DiffOp &op = ops.at(k);
            if (op.oldNo > 0) {
                if (oldStart == 0) {
                    oldStart = op.oldNo;
                }
                ++oldCount;
            }
            if (op.newNo > 0) {
                if (newStart == 0) {
                    newStart = op.newNo;
                }
                ++newCount;
            }
        }
        auto range = [](int start, int count) {
            // A zero-length range is written "<line before>,0": the count may
            // only be dropped when it is 1, and the line before an insertion at
            // the very top of a file is 0.
            if (count == 0) {
                return u"%1,0"_s.arg(qMax(0, start));
            }
            return count == 1 ? QString::number(start) : u"%1,%2"_s.arg(start).arg(count);
        };
        out += u"@@ -%1 +%2 @@\n"_s.arg(range(oldStart, oldCount), range(newStart, newCount));
        ++emitted;
        for (int k = from; k < to && emitted < maxOutputLines; ++k) {
            const DiffOp &op = ops.at(k);
            out += QString(op.kind) + op.text + u"\n"_s;
            ++emitted;
        }
        idx = nxt;
    }
    if (emitted >= maxOutputLines) {
        out += u"... (diff truncated)\n"_s;
    }
    return out;
}

ToolRunner::ToolRunner(Sandbox sandbox, DocumentBridge *bridge, QObject *parent)
    : QObject(parent)
    , m_sandbox(std::move(sandbox))
    , m_bridge(bridge)
{
    // Initialize the tool runner with a sandbox for security and a document bridge for file operations
}

PermissionRequest ToolRunner::describe(const ToolCall &call) const
{
    QString parseError;
    const QJsonObject args = parseToolArgs(call, &parseError);
    PermissionRequest req;
    req.toolName = call.name;
    req.toolCallId = call.id;
    req.path = args.value(u"path"_s).toString();

    req.summary = shellCommandFor(call.name, args);

    if (call.name == u"write_file"_s) {
        req.risk = ToolRisk::Write;
        req.mutatesFiles = true;
        QString existing;
        QString error;
        const QString resolved = m_sandbox.resolve(req.path, &error, true);
        req.resolvedPath = resolved;
        // Do not preview deny-listed content: resolve() checked write access;
        // read access is separate and the preview is shown in the approval card.
        if (!resolved.isEmpty() && m_bridge) {
            QString readError;
            if (m_sandbox.allowsRead(resolved, &readError)) {
                m_bridge->readDocument(resolved, &existing);
            } else {
                existing = u"(preview unavailable: read blocked by sandbox)"_s;
            }
        }
        req.describeDiff = unifiedDiff(req.path, existing, args.value(u"content"_s).toString());
        req.details = req.describeDiff;
    } else if (call.name == u"edit_file"_s) {
        req.risk = ToolRisk::Write;
        req.mutatesFiles = true;
        {
            QString error;
            req.resolvedPath = m_sandbox.resolve(req.path, &error, true);
        }
        const QString oldString = args.value(u"old_string"_s).toString();
        const QString newString = args.value(u"new_string"_s).toString();
        req.describeDiff = unifiedDiff(req.path, oldString, newString);
        req.details = u"Replace:\n%1\n\nWith:\n%2"_s.arg(oldString, newString);
    } else if (call.name == u"multi_edit_file"_s || call.name == u"multi_replace_file_content"_s) {
        req.risk = ToolRisk::Write;
        req.mutatesFiles = true;
        if (req.path.isEmpty()) {
            req.path = args.value(u"TargetFile"_s).toString();
        }
        QString existing;
        QString error;
        const QString resolved = m_sandbox.resolve(req.path, &error, true);
        req.resolvedPath = resolved;
        if (!resolved.isEmpty() && m_bridge) {
            m_bridge->readDocument(resolved, &existing);
        }
        QJsonArray edits = args.value(u"edits"_s).toArray();
        if (edits.isEmpty()) {
            edits = args.value(u"chunks"_s).toArray();
        }
        if (edits.isEmpty()) {
            edits = args.value(u"ReplacementChunks"_s).toArray();
        }
        QString simulated = existing;
        for (const QJsonValue &val : edits) {
            const QJsonObject chunk = val.toObject();
            const QString oldStr = chunk.contains(u"old_string"_s)
                ? chunk.value(u"old_string"_s).toString()
                : (chunk.contains(u"TargetContent"_s) ? chunk.value(u"TargetContent"_s).toString()
                                                      : chunk.value(u"targetContent"_s).toString());
            QString newStr;
            if (chunk.contains(u"new_string"_s)) {
                newStr = chunk.value(u"new_string"_s).toString();
            } else if (chunk.contains(u"ReplacementContent"_s)) {
                newStr = chunk.value(u"ReplacementContent"_s).toString();
            } else {
                newStr = chunk.value(u"replacementContent"_s).toString();
            }
            if (oldStr.isEmpty()) {
                continue;
            }
            // Match the tool: replace only the first occurrence unless the
            // chunk explicitly asks for all, otherwise the preview can show a
            // different file from the one the user approves.
            const bool replaceAll = chunk.contains(u"replace_all"_s)
                ? jsonBool(chunk.value(u"replace_all"_s))
                : (chunk.contains(u"AllowMultiple"_s) ? jsonBool(chunk.value(u"AllowMultiple"_s))
                                                      : jsonBool(chunk.value(u"allowMultiple"_s)));
            const int at = simulated.indexOf(oldStr);
            if (at >= 0) {
                if (replaceAll) {
                    simulated.replace(oldStr, newStr);
                } else {
                    simulated.replace(at, oldStr.size(), newStr);
                }
            }
        }
        req.describeDiff = unifiedDiff(req.path, existing, simulated);
        req.details = req.describeDiff;
    } else if (call.name == u"bash"_s) {
        req.risk = ToolRisk::Execute;
        req.details = args.value(u"command"_s).toString();
    } else if (call.name == u"memory_write"_s) {
        req.risk = ToolRisk::Write;
        req.mutatesFiles = true;
        QString pathError;
        const QString resolved = memoryWriteTarget(m_sandbox, args, &pathError);
        if (resolved.isEmpty()) {
            req.summary = pathError;
            req.details = pathError;
            return req;
        }
        const bool projectScope =
            args.value(u"scope"_s).toString(u"project"_s) == u"project"_s;
        req.path = (projectScope && !m_sandbox.workspaceRoot().isEmpty())
            ? QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved)
            : resolved;
        req.resolvedPath = resolved;
        QString existing;
        readMemoryFile(resolved, m_bridge, &existing);
        const QString updated = memoryNoteBody(args);
        req.describeDiff = unifiedDiff(req.path, existing, updated);
        req.details = req.describeDiff;
    } else if (call.name == u"write_plan_file"_s) {
        // Read risk on purpose: the plan file is a Kate-chosen temp file, not a
        // workspace mutation, so it never prompts and never enters the tracker.
        req.risk = ToolRisk::Read;
        req.details = args.value(u"content"_s).toString();
    } else if (call.name == u"question"_s) {
        // Read risk for the same reason: asking the user something neither
        // reads the workspace nor changes it.
        req.risk = ToolRisk::Read;
        req.questionText = questionTextFromToolArguments(args);
        req.questionOptions = questionOptionsFromToolArguments(args);
        req.allowFreeform = questionAllowFreeformFromToolArguments(args, true);
        req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    } else if (call.name == u"suggest"_s) {
        // Advice, not work: read risk so it never prompts, and the full
        // arguments as details so the card can be built while the tool runs
        // and the history can rebuild it later.
        req.risk = ToolRisk::Read;
        req.path = args.value(u"path"_s).toString().trimmed();
        const QString advice = suggestionTextFromToolArguments(args);
        if (!advice.isEmpty()) {
            req.summary = advice.left(200);
        }
        req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    } else if (call.name == u"apply_patch"_s) {
        req.risk = ToolRisk::Write;
        req.mutatesFiles = true;
        // The patch names every file it touches; the card shows the first as
        // the primary path and the rest as additional diffs, so multi-file
        // patches are reviewable per file like any other edit.
        const QList<PatchFile> files = parseUnifiedPatch(args.value(u"patch"_s).toString());
        QStringList paths;
        QStringList diffs;
        for (const PatchFile &file : files) {
            if (file.path.isEmpty()) {
                continue;
            }
            paths.append(file.path);
            QStringList hunkText;
            for (const PatchHunk &hunk : file.hunks) {
                hunkText.append(u"@@ -%1,%2 +%3,%4 @@"_s.arg(hunk.oldStart).arg(hunk.oldCount).arg(hunk.newStart).arg(hunk.newCount));
                hunkText.append(hunk.lines);
            }
            diffs.append(u"--- a/%1\n+++ b/%2\n"_s.arg(file.path, file.path) + hunkText.join(u'\n'));
        }
        if (!paths.isEmpty()) {
            req.path = paths.first();
            QString error;
            req.resolvedPath = m_sandbox.resolve(req.path, &error, true);
            for (int i = 1; i < paths.size(); ++i) {
                req.additionalPaths.append(paths.at(i));
                QString err;
                m_sandbox.resolve(paths.at(i), &err, true);
            }
            req.additionalDiffs = diffs.mid(1);
        }
        req.describeDiff = diffs.isEmpty() ? args.value(u"patch"_s).toString() : diffs.first();
        req.details = req.describeDiff;
    } else if (call.name == u"todo_write"_s || call.name == u"todo_read"_s || call.name == u"think"_s
               || call.name == u"exit_plan_mode"_s) {
        // Checklist, reasoning and plan-mode bookkeeping touch no file.
        req.risk = ToolRisk::Read;
        req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    } else if (call.name == u"debug_status"_s || call.name == u"debug_stack"_s
               || call.name == u"debug_locals"_s || call.name == u"debug_breakpoints"_s
               || call.name == u"debug_select_frame"_s || call.name == u"debug_select_thread"_s
               || call.name == u"debug_snapshot"_s || call.name == u"build_targets"_s) {
        req.risk = ToolRisk::Read;
        req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    } else if (call.name == u"debug_set_breakpoint"_s) {
        req.risk = ToolRisk::Write;
        if (!req.path.isEmpty()) {
            QString error;
            req.resolvedPath = m_sandbox.resolve(req.path, &error, true);
        }
        req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    } else if (call.name == u"debug_step"_s || call.name == u"debug_eval"_s
               || call.name == u"debug_session"_s || call.name == u"debug_console"_s
               || call.name == u"build_run"_s || call.name == u"terminal_run"_s) {
        req.risk = ToolRisk::Execute;
        if (call.name == u"terminal_run"_s) {
            req.details = args.value(u"command"_s).toString();
        } else if (call.name == u"debug_eval"_s) {
            req.details = args.value(u"expression"_s).toString();
        } else if (call.name == u"debug_console"_s) {
            req.details = args.value(u"command"_s).toString();
        } else {
            req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
        }
    } else {
        // Unknown tools reach the permission card from here. A delegation or
        // an MCP call can run code, so they must not be presented with the
        // read-risk styling that auto-approval affordances key off.
        req.risk = (call.name == u"new_task"_s || isMcpToolName(call.name)) ? ToolRisk::Execute : ToolRisk::Read;
        req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    }
    return req;
}

void ToolRunner::resetReadLedger()
{
    m_readSnapshots.clear();
}

ToolResult ToolRunner::run(const ToolCall &call)
{
    QString parseError;
    const QJsonObject args = parseToolArgs(call, &parseError);
    ToolResult result;
    result.toolCallId = call.id;
    result.name = call.name;
    if (!parseError.isEmpty() && args.isEmpty() && !call.argumentsJson.isEmpty()) {
        result.ok = false;
        result.output = parseError;
        return result;
    }

    if (call.name == u"read_file"_s) {
        return readFile(args);
    }
    if (call.name == u"read_tool_result"_s) {
        return readToolResult(args);
    }
    if (call.name == u"write_file"_s) {
        return writeFile(args);
    }
    if (call.name == u"edit_file"_s) {
        return editFile(args);
    }
    if (call.name == u"multi_edit_file"_s || call.name == u"multi_replace_file_content"_s) {
        return multiEditFile(args);
    }
    if (call.name == u"list_dir"_s) {
        return listDir(args);
    }
    if (call.name == u"grep"_s) {
        return grep(args);
    }
    if (call.name == u"glob"_s) {
        return glob(args);
    }
    if (call.name == u"bash"_s) {
        return bash(args);
    }
    if (call.name == u"query_project_graph"_s) {
        return queryProjectGraph(args);
    }
    if (call.name == u"lsp_diagnostics"_s) {
        return lspDiagnostics(args);
    }
    if (call.name == u"lsp_symbols"_s) {
        return lspSymbols(args);
    }
    if (call.name == u"lsp_hover"_s) {
        return lspHover(args);
    }
    if (call.name == u"lsp_definition"_s) {
        return lspDefinition(args);
    }
    if (call.name == u"lsp_references"_s) {
        return lspReferences(args);
    }
    if (call.name == u"lsp_code_actions"_s) {
        return lspCodeActions(args);
    }
    if (call.name == u"debug_status"_s) {
        return debugStatus(args);
    }
    if (call.name == u"debug_stack"_s) {
        return debugStack(args);
    }
    if (call.name == u"debug_locals"_s) {
        return debugLocals(args);
    }
    if (call.name == u"debug_breakpoints"_s) {
        return debugBreakpoints(args);
    }
    if (call.name == u"debug_select_frame"_s) {
        return debugSelectFrame(args);
    }
    if (call.name == u"debug_select_thread"_s) {
        return debugSelectThread(args);
    }
    if (call.name == u"debug_snapshot"_s) {
        return debugSnapshot(args);
    }
    if (call.name == u"debug_set_breakpoint"_s) {
        return debugSetBreakpoint(args);
    }
    if (call.name == u"debug_step"_s) {
        return debugStep(args);
    }
    if (call.name == u"debug_eval"_s) {
        return debugEval(args);
    }
    if (call.name == u"debug_session"_s) {
        return debugSession(args);
    }
    if (call.name == u"debug_console"_s) {
        return debugConsole(args);
    }
    if (call.name == u"build_targets"_s) {
        return buildTargets(args);
    }
    if (call.name == u"build_run"_s) {
        return buildRun(args);
    }
    if (call.name == u"terminal_run"_s) {
        return terminalRun(args);
    }
    if (call.name == u"write_plan_file"_s) {
        return writePlanFile(args);
    }
    if (call.name == u"suggest"_s) {
        return suggest(args);
    }
    if (call.name == u"question"_s) {
        return question(args);
    }
    if (call.name == u"apply_patch"_s) {
        return applyPatch(args);
    }
    if (call.name == u"memory_write"_s) {
        return memoryWrite(args);
    }
    if (call.name == u"todo_write"_s) {
        return todoWrite(args);
    }
    if (call.name == u"todo_read"_s) {
        return todoRead(args);
    }
    if (call.name == u"think"_s) {
        return think(args);
    }
    if (call.name == u"exit_plan_mode"_s) {
        return exitPlanMode(args);
    }

    result.ok = false;
    result.output = u"Unknown tool: %1"_s.arg(call.name);
    return result;
}

ToolResult ToolRunner::readToolResult(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"read_tool_result"_s;

    const QString id = args.value(u"id"_s).toString();
    if (id.isEmpty()) {
        result.ok = false;
        result.output = u"id is required (the value shown in the truncation marker)."_s;
        return result;
    }
    const qint64 offset = jsonInt(args.value(u"offset"_s), 0);
    int limit = jsonInt(args.value(u"limit"_s), 20000);
    limit = qBound(1, limit, 80000);

    QString page;
    qint64 total = 0;
    QString error;
    if (!ToolResults::read(id, offset, limit, &page, &total, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    result.ok = true;
    result.output = page;
    const qint64 next = offset + page.size();
    if (next < total) {
        result.output += u"\n\n[showing characters %1-%2 of %3; continue with offset=%4]"_s
                             .arg(offset).arg(next).arg(total).arg(next);
    } else {
        result.output += u"\n\n[end of stored output; %1 characters total]"_s.arg(total);
    }
    return result;
}

ToolResult ToolRunner::readFile(const QJsonObject &args) const
{
    // Initialize the result structure to track success/failure
    ToolResult result;
    result.name = u"read_file"_s;
    
    // Extract the file path from the tool arguments
    QString error;
    const QString path = args.value(u"path"_s).toString();
    
    // Check if the sandbox allows reading from this path
    if (!m_sandbox.allowsRead(path, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    
    // Resolve the path relative to the sandbox workspace
    const QString resolved = m_sandbox.resolve(path, &error);
    const QString readPath = resolved.isEmpty() ? QDir(m_sandbox.workspaceRoot()).absoluteFilePath(path) : resolved;
    
    // In strict mode, reject paths that can't be resolved
    if (resolved.isEmpty() && m_sandbox.profile() == SandboxProfile::Strict) {
        result.ok = false;
        result.output = error;
        return result;
    }

    // Remember the state of the file the agent is looking at. write_file uses
    // this to refuse an overwrite after the file changed underneath the agent.
    // The cap keeps a multi-hundred-megabyte read from sitting in memory just
    // for the conflict check.
    if (m_bridge) {
        QString full;
        constexpr qsizetype kMaxLedgerChars = 4 * 1024 * 1024;
        if (m_bridge->readDocument(readPath, &full) && full.size() <= kMaxLedgerChars) {
            m_readSnapshots.insert(ledgerKey(readPath), full);
        }
    }

    // A binary file cannot be read as numbered lines; say so instead of
    // handing the model a wall of mojibake. The probe reads the first 8 KiB
    // straight from disk; a document that exists only in the editor is not
    // probed and keeps the previous behaviour.
    QFile probe(readPath);
    if (probe.open(QIODevice::ReadOnly)) {
        const QByteArray head = probe.read(8192);
        probe.close();
        if (head.contains('\0')) {
            result.ok = true;
            result.output = u"(binary file, %1 bytes)"_s.arg(QFileInfo(readPath).size());
            return result;
        }
    }

    // Apply offset and limit parameters to paginate the file contents.
    // Reject non-positive values explicitly instead of silently clamping:
    // offset=0/-5 or limit<0 previously read from the top / unlimited.
    // "line" is accepted as an alias of "offset": ACP's fs/read_text_file uses
    // line/limit, and agents carry that shape over to the native tool.
    int offset = 1;
    if (args.contains(u"offset"_s)) {
        offset = jsonInt(args.value(u"offset"_s), 0);
    } else if (args.contains(u"line"_s)) {
        offset = jsonInt(args.value(u"line"_s), 1);
    }
    const int limit = jsonInt(args.value(u"limit"_s), 0);
    if (offset < 1) {
        result.ok = false;
        result.output = u"offset must be >= 1."_s;
        return result;
    }
    if (limit < 0) {
        result.ok = false;
        result.output = u"limit must be >= 0."_s;
        return result;
    }
    constexpr int kMaxReadLines = 10000;
    const int cappedLimit = limit > 0 ? qMin(limit, kMaxReadLines) : 0;
    int start = std::max(0, offset - 1);
    QStringList slice;
    int totalLines = 0;
    if (!m_bridge || !m_bridge->readDocumentLines(readPath, start + 1, cappedLimit > 0 ? cappedLimit : kMaxReadLines,
                                                  &slice, &totalLines)) {
        result.ok = false;
        result.output = u"Failed to read %1"_s.arg(readPath);
        return result;
    }
    if (totalLines == 0) {
        // A zero-byte file is not an error: the agent reads files it just
        // created. Saying "offset is past the end" would make a valid read
        // look like a failed tool call.
        result.ok = true;
        result.output = u"(empty file)"_s;
        return result;
    }
    if (start >= totalLines) {
        result.ok = false;
        result.output = u"File has %1 lines; offset is past the end."_s.arg(totalLines);
        return result;
    }

    // Format the output with line numbers for easier reading. Stop adding lines
    // once the observation budget is reached: clip() would otherwise cut the
    // body mid-way while the "N more line(s)" note below still claims that the
    // whole slice was shown, which tells the model it saw a file it did not.
    constexpr int kMaxReadOutputChars = 80000; // keep in sync with clip()
    QString numbered;
    int emitted = 0;
    for (; emitted < slice.size(); ++emitted) {
        const QString line = u"%1|%2\n"_s.arg(start + emitted + 1, 6).arg(slice.at(emitted));
        if (emitted > 0 && numbered.size() + line.size() > kMaxReadOutputChars) {
            break;
        }
        numbered += line;
    }
    result.output = clip(numbered);
    const int remaining = totalLines - (start + emitted);
    if (remaining > 0) {
        if (limit > 0 && limit > cappedLimit) {
            result.output += u"\n[Requested %1 line(s) but a read is capped at %2; %3 line(s) remain. Call again with offset=%4 to continue.]"_s
                                 .arg(limit).arg(cappedLimit).arg(remaining).arg(start + emitted + 1);
        } else {
            result.output += u"\n[%1 more line(s) not shown. Call again with offset=%2 to continue.]"_s
                                 .arg(remaining).arg(start + emitted + 1);
        }
    }
    return result;
}

ToolResult ToolRunner::writeFile(const QJsonObject &args)
{
    // Initialize the result structure to track success/failure
    ToolResult result;
    result.name = u"write_file"_s;
    
    // Extract the file path and content from the tool arguments
    QString error;
    const QString path = args.value(u"path"_s).toString();
    
    // Check if the sandbox allows writing to this path
    if (!m_sandbox.allowsWrite(path, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    
    // Resolve the path relative to the sandbox workspace with write permissions
    const QString resolved = m_sandbox.resolve(path, &error, true);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }
    
    // A missing content key must not truncate an existing file: a truncated
    // stream or a malformed model call would otherwise silently empty it. An
    // explicit empty string is still a valid write.
    if (!args.contains(u"content"_s)) {
        result.ok = false;
        result.output = u"content is required (pass an empty string to write an empty file)."_s;
        return result;
    }
    const QString content = args.value(u"content"_s).toString();

    // If the agent read this file earlier in the turn, only write when it still
    // holds exactly what the agent saw. This is optimistic concurrency: a user
    // edit, a formatter, or another agent between the read and the write is a
    // conflict, not something to silently clobber. A file never read this turn
    // keeps the historical create/overwrite behaviour.
    const QString key = ledgerKey(resolved);
    const auto snapshot = m_readSnapshots.constFind(key);
    if (snapshot != m_readSnapshots.constEnd()) {
        QString current;
        if (!m_bridge || !m_bridge->readDocument(resolved, &current)) {
            result.ok = false;
            result.output = u"%1 changed or disappeared after it was read. Re-read it before writing."_s.arg(path);
            return result;
        }
        if (current != snapshot.value()) {
            result.ok = false;
            result.output = u"%1 changed after it was read. Re-read it, then apply the intended edit to the current content."_s.arg(path);
            return result;
        }
        if (!m_bridge->writeDocumentIfUnchanged(resolved, current, content, &error)) {
            result.ok = false;
            result.output = error.isEmpty() ? u"Write failed."_s : error;
            return result;
        }
    } else if (!m_bridge || !m_bridge->writeDocument(resolved, content, &error)) {
        result.ok = false;
        result.output = error.isEmpty() ? u"Write failed."_s : error;
        return result;
    }

    // The file now holds the new content; the old snapshot is stale.
    m_readSnapshots.remove(key);
    
    // Report successful write with file path and byte count
    result.output = u"Wrote %1 (%2 bytes)"_s.arg(resolved).arg(content.toUtf8().size());
    return result;
}

ToolResult ToolRunner::editFile(const QJsonObject &args)
{
    // Initialize the result structure to track success/failure
    ToolResult result;
    result.name = u"edit_file"_s;
    
    // Extract the file path from the tool arguments
    QString error;
    const QString path = args.value(u"path"_s).toString();
    
    // Check if the sandbox allows writing to this path
    if (!m_sandbox.allowsWrite(path, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    
    // Resolve the path relative to the sandbox workspace with write permissions
    const QString resolved = m_sandbox.resolve(path, &error, true);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }

    // Read the current file contents through the document bridge
    QString contents;
    if (!m_bridge || !m_bridge->readDocument(resolved, &contents)) {
        result.ok = false;
        result.output = u"Failed to read %1"_s.arg(resolved);
        return result;
    }

    const QString oldString = args.value(u"old_string"_s).toString();
    const QString newString = args.value(u"new_string"_s).toString();
    const bool replaceAll = jsonBool(args.value(u"replace_all"_s));
    const QString originalContents = contents;

    if (oldString.isEmpty()) {
        result.ok = false;
        result.output = u"old_string must not be empty."_s;
        return result;
    }

    int count = contents.count(oldString);
    bool canonicalMatch = false;
    int canonicalCount = 0;
    if (count == 0) {
        QString updated;
        QString canonicalError;
        if (applyCanonicalEdit(contents, oldString, newString, replaceAll,
                               &updated, &canonicalCount, &canonicalError)) {
            contents = updated;
            canonicalMatch = true;
        } else {
            result.ok = false;
            QString hint;
            const QString needle = oldString.section(u'\n', 0, 0).trimmed();
            if (!needle.isEmpty()) {
                const QStringList lines = contents.split(u'\n');
                QStringList nearby;
                for (int i = 0; i < lines.size() && nearby.size() < 5; ++i) {
                    if (lines.at(i).contains(needle)) {
                        nearby.append(u"%1:%2"_s.arg(i + 1).arg(lines.at(i).left(200)));
                    }
                }
                if (!nearby.isEmpty()) {
                    hint = u" Nearby lines:\n"_s + nearby.join(u'\n');
                }
            }
            if (canonicalError == u"ambiguous"_s) {
                result.output = u"old_string matched multiple locations after normalising whitespace and punctuation in %1; "
                                u"set replace_all=true or include more context."_s.arg(resolved) + hint;
            } else {
                result.output = u"old_string was not found in %1. Read the file and copy the exact text, including whitespace."_s.arg(resolved) + hint;
            }
            return result;
        }
    }
    if (!canonicalMatch && count > 1 && !replaceAll) {
        result.ok = false;
        result.output = u"old_string matched %1 times; it must be unique, or set replace_all=true."_s.arg(count);
        return result;
    }

    if (!canonicalMatch) {
        contents.replace(oldString, newString);
    }
    if (!m_bridge->writeDocumentIfUnchanged(resolved, originalContents, contents, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }

    if (canonicalMatch) {
        result.output = u"Updated %1 (%2 canonical match%3; exact text differed in whitespace or punctuation)"_s
                            .arg(resolved)
                            .arg(canonicalCount)
                            .arg(canonicalCount > 1 ? u"s"_s : u""_s);
    } else {
        result.output = replaceAll && count > 1
            ? u"Updated %1 (%2 replacements)"_s.arg(resolved).arg(count)
            : u"Updated %1"_s.arg(resolved);
    }
    return result;
}

ToolResult ToolRunner::multiEditFile(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"multi_edit_file"_s;

    QString error;
    QString path = args.value(u"path"_s).toString();
    if (path.isEmpty()) {
        path = args.value(u"TargetFile"_s).toString();
    }

    if (path.isEmpty()) {
        result.ok = false;
        result.output = u"File path is required."_s;
        return result;
    }

    if (!m_sandbox.allowsWrite(path, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }

    const QString resolved = m_sandbox.resolve(path, &error, true);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }

    QJsonArray edits = args.value(u"edits"_s).toArray();
    if (edits.isEmpty()) {
        edits = args.value(u"chunks"_s).toArray();
    }
    if (edits.isEmpty()) {
        edits = args.value(u"ReplacementChunks"_s).toArray();
    }

    if (edits.isEmpty()) {
        result.ok = false;
        result.output = u"No edit chunks provided in 'edits'."_s;
        return result;
    }

    QString contents;
    if (!m_bridge || !m_bridge->readDocument(resolved, &contents)) {
        result.ok = false;
        result.output = u"Failed to read %1"_s.arg(resolved);
        return result;
    }

    // Validate all edits before applying any (atomicity)
    QStringList allOldStrings;
    QStringList allNewStrings;
    QList<bool> allReplaceAll;
    
    const int numChunks = edits.size();
    for (int i = 0; i < numChunks; ++i) {
        const QJsonObject chunk = edits.at(i).toObject();
        QString oldStr = chunk.value(u"old_string"_s).toString();
        if (oldStr.isEmpty()) {
            oldStr = chunk.value(u"TargetContent"_s).toString().isEmpty()
                ? chunk.value(u"targetContent"_s).toString()
                : chunk.value(u"TargetContent"_s).toString();
        }

        QString newStr = chunk.value(u"new_string"_s).toString();
        if (newStr.isEmpty() && !chunk.contains(u"new_string"_s)) {
            newStr = chunk.value(u"ReplacementContent"_s).toString().isEmpty()
                ? chunk.value(u"replacementContent"_s).toString()
                : chunk.value(u"ReplacementContent"_s).toString();
        }

        bool replaceAll = false;
        if (chunk.contains(u"replace_all"_s)) {
            replaceAll = jsonBool(chunk.value(u"replace_all"_s));
        } else if (chunk.contains(u"AllowMultiple"_s)) {
            replaceAll = jsonBool(chunk.value(u"AllowMultiple"_s));
        } else if (chunk.contains(u"allowMultiple"_s)) {
            replaceAll = jsonBool(chunk.value(u"allowMultiple"_s));
        }

        if (oldStr.isEmpty()) {
            result.ok = false;
            result.output = u"Chunk %1 of %2: old_string must not be empty. No edits were applied."_s
                .arg(i + 1).arg(numChunks);
            return result;
        }

        const int count = contents.count(oldStr);
        if (count == 0) {
            result.ok = false;
            QString hint;
            const QString needle = oldStr.section(u'\n', 0, 0).trimmed();
            if (!needle.isEmpty()) {
                const QStringList lines = contents.split(u'\n');
                QStringList nearby;
                for (int l = 0; l < lines.size() && nearby.size() < 5; ++l) {
                    if (lines.at(l).contains(needle)) {
                        nearby.append(u"%1:%2"_s.arg(l + 1).arg(lines.at(l).left(200)));
                    }
                }
                if (!nearby.isEmpty()) {
                    hint = u" Nearby lines:\n"_s + nearby.join(u'\n');
                }
            }
            result.output = u"Chunk %1 of %2: old_string was not found in %3. No edits were applied. Read the file again and copy the exact text including whitespace."_s
                .arg(i + 1).arg(numChunks).arg(resolved) + hint;
            return result;
        }

        if (count > 1 && !replaceAll) {
            result.ok = false;
            result.output = u"Chunk %1 of %2: old_string matched %3 times; it must be unique, or set replace_all=true. No edits were applied."_s
                .arg(i + 1).arg(numChunks).arg(count);
            return result;
        }

        allOldStrings.append(oldStr);
        allNewStrings.append(newStr);
        allReplaceAll.append(replaceAll);
    }

    // Apply all edits atomically. Each edit must target text none of the
    // earlier edits consumed or produced: the matches are located in the
    // working string after those edits, so an edit aimed at text an earlier
    // one rewrote silently lands somewhere else than the caller meant (or
    // matches twice where it used to match once). Rejecting the whole batch
    // is the only behaviour the caller can reason about, so overlaps fail
    // the same way a missing old_string does: nothing is written.
    QString workingContents = contents;
    int totalReplacements = 0;
    // Ranges of the working string written or consumed by earlier edits, in
    // ascending, disjoint order.
    QList<QPair<int, int>> claimed;
    for (int i = 0; i < numChunks; ++i) {
        const QString &oldStr = allOldStrings[i];
        const QString &newStr = allNewStrings[i];
        const bool replaceAll = allReplaceAll[i];

        if (workingContents.count(oldStr) == 0) {
            // This shouldn't happen since we validated, but check anyway
            result.ok = false;
            result.output = u"Chunk %1 of %2: old_string no longer matches after previous edits. No edits were applied."_s
                .arg(i + 1).arg(numChunks);
            return result;
        }

        // Walk the matches left to right, non-overlapping, the way
        // QString::replace does, so span arithmetic stays in step with it.
        int searchFrom = 0;
        QList<QPair<int, int>> matches;
        while (true) {
            const int at = workingContents.indexOf(oldStr, searchFrom);
            if (at < 0) {
                break;
            }
            matches.append({at, at + oldStr.size()});
            searchFrom = at + oldStr.size();
        }

        if (matches.size() > 1 && !replaceAll) {
            result.ok = false;
            result.output = u"Chunk %1 of %2: old_string matched %3 times after previous edits; it must be unique, or set replace_all=true. No edits were applied."_s
                .arg(i + 1).arg(numChunks).arg(matches.size());
            return result;
        }

        for (const auto &match : matches) {
            for (const auto &range : claimed) {
                if (match.first < range.second && range.first < match.second) {
                    result.ok = false;
                    result.output = u"Chunk %1 of %2: old_string overlaps text an earlier edit already changed. No edits were applied. Split the edits so each one targets text untouched by the others, re-reading the file if needed."_s
                        .arg(i + 1).arg(numChunks);
                    return result;
                }
            }
        }

        // Rebuild with the new text in place of every match, tracking where
        // each edit's output lands and where the untouched ranges moved to.
        // Matches are ascending and disjoint and none of them overlap
        // `claimed`, so a single left-to-right walk covers both.
        QString rebuilt;
        rebuilt.reserve(workingContents.size());
        QList<QPair<int, int>> newClaimed;
        int cursor = 0;
        int shift = 0;
        int ci = 0;
        for (const auto &match : matches) {
            // Claimed ranges that end at or before this match sit in the
            // preserved gap ahead of it and move by the accumulated shift.
            while (ci < claimed.size() && claimed[ci].second <= match.first) {
                newClaimed.append({claimed[ci].first + shift, claimed[ci].second + shift});
                ++ci;
            }
            rebuilt += workingContents.mid(cursor, match.first - cursor);
            rebuilt += newStr;
            newClaimed.append({rebuilt.size() - newStr.size(), rebuilt.size()});
            cursor = match.second;
            shift += newStr.size() - oldStr.size();
        }
        while (ci < claimed.size()) {
            newClaimed.append({claimed[ci].first + shift, claimed[ci].second + shift});
            ++ci;
        }
        rebuilt += workingContents.mid(cursor);
        workingContents = rebuilt;
        claimed = newClaimed;

        totalReplacements += matches.size();
    }

    if (!m_bridge->writeDocumentIfUnchanged(resolved, contents, workingContents, &error)) {
        result.ok = false;
        result.output = error.isEmpty() ? u"Failed to write modified content."_s : error;
        return result;
    }

    result.output = u"Updated %1 (%2 edit chunks applied, %3 total replacements)"_s
        .arg(resolved).arg(numChunks).arg(totalReplacements);
    return result;
}

ToolResult ToolRunner::listDir(const QJsonObject &args) const
{
    // Initialize the result structure to track success/failure
    ToolResult result;
    result.name = u"list_dir"_s;
    
    // Extract the directory path from the tool arguments
    QString error;
    const QString path = args.value(u"path"_s).toString();
    
    // Determine the directory to list (use workspace root if path is empty)
    const QString targetPath = path.isEmpty() ? m_sandbox.workspaceRoot() : path;
    
    // Check if the sandbox allows reading from this directory
    if (!m_sandbox.allowsRead(targetPath, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    
    // Resolve the path relative to the sandbox workspace
    const QString resolved = m_sandbox.resolve(targetPath, &error);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }
    
    // Verify the directory exists
    QDir dir(resolved);
    if (!dir.exists()) {
        result.ok = false;
        result.output = u"Directory does not exist: %1"_s.arg(resolved);
        return result;
    }
    
    constexpr int kMaxDirectoryEntries = 1000;
    constexpr int kMaxDirectoryOutputChars = 80000;
    QVector<QPair<QString, QString>> visibleEntries;
    QDirIterator iterator(resolved, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    bool truncated = false;
    int skippedDenied = 0;
    while (iterator.hasNext()) {
        if (readCancelled()) {
            result.ok = false;
            result.cancelled = true;
            result.output = u"Read cancelled because the turn ended."_s;
            return result;
        }
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (m_sandbox.isDenied(info.absoluteFilePath())) {
            ++skippedDenied;
            continue;
        }
        if (visibleEntries.size() >= kMaxDirectoryEntries) {
            truncated = true;
            break;
        }
        visibleEntries.append({info.fileName(), (info.isDir() ? u"dir  "_s : u"file "_s) + info.fileName()});
    }
    std::sort(visibleEntries.begin(), visibleEntries.end(), [](const auto &left, const auto &right) {
        return left.first < right.first;
    });
    QStringList lines;
    int outputChars = 0;
    for (const auto &entry : visibleEntries) {
        const int addedChars = entry.second.size() + (lines.isEmpty() ? 0 : 1);
        if (outputChars + addedChars > kMaxDirectoryOutputChars) {
            truncated = true;
            break;
        }
        lines.append(entry.second);
        outputChars += addedChars;
    }
    result.output = lines.isEmpty() ? u"(empty)"_s : lines.join(u'\n');
    if (skippedDenied > 0) {
        result.output += u"\n[%1 entry(s) hidden by sandbox deny rules.]"_s.arg(skippedDenied);
    }
    if (truncated) {
        result.output += u"\n[Listing truncated; refine the directory path to see more entries.]"_s;
    }
    return result;
}

ToolResult ToolRunner::grep(const QJsonObject &args) const
{
    // Initialize the result structure to track success/failure
    ToolResult result;
    result.name = u"grep"_s;
    
    // Extract search parameters from the tool arguments
    const QString pattern = args.value(u"pattern"_s).toString();
    const QString globFilter = args.value(u"glob"_s).toString();
    const bool caseInsensitive = jsonBool(args.value(u"case_insensitive"_s));
    const int context = qBound(0, jsonInt(args.value(u"context"_s), 0), 5);

    QString error;
    const QString path = args.value(u"path"_s).toString();
    const QString target = path.isEmpty() ? m_sandbox.workspaceRoot() : path;
    // The search root is checked first. Without it a directory outside the
    // workspace would be searched and every file in it skipped, and the
    // model would read "no matches" instead of "not allowed".
    if (!m_sandbox.allowsRead(target, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    const QString resolved = m_sandbox.resolve(target, &error);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }

    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
    if (caseInsensitive) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    QRegularExpression re(pattern, options);
    if (!re.isValid()) {
        result.ok = false;
        result.output = u"Invalid regular expression: %1"_s.arg(re.errorString());
        return result;
    }

    QStringList hits;
    int skippedFiles = 0;
    auto searchFile = [&](const QString &filePath) {
        if (readCancelled()) {
            return;
        }
        if (m_sandbox.isDenied(filePath) || !m_sandbox.allowsRead(filePath, nullptr)) {
            return;
        }
        const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(filePath);
        if (isNoisySearchPath(rel)) {
            return;
        }
        if (!globFilter.isEmpty() && !QDir::match(globFilter, QFileInfo(filePath).fileName())
            && !Sandbox::globMatch(globFilter, filePath) && !Sandbox::globMatch(globFilter, rel)) {
            return;
        }

        // Searching reads each candidate fully into memory; skip pathologically
        // large files (build artifacts, bundles, minified output) so one of them
        // cannot blow up the turn's memory.
        constexpr qint64 kMaxSearchBytes = 4 * 1024 * 1024;
        if (QFileInfo(filePath).size() > kMaxSearchBytes) {
            ++skippedFiles;
            return;
        }

        QString contents;
        if (!m_bridge || !m_bridge->readDocument(filePath, &contents)) {
            return;
        }

        // A NUL byte in the first 8 KiB marks the file as binary: regex hits
        // in it are mojibake, and they would flood the transcript.
        if (contents.left(8192).contains(QChar(u'\0'))) {
            ++skippedFiles;
            return;
        }

        const QStringList lines = contents.split(u'\n');
        int lastMatchEnd = -1; // Track end of last context block to avoid duplicate separators
        for (int i = 0; i < lines.size(); ++i) {
            if ((i & 0x3ff) == 0 && readCancelled()) {
                return;
            }
            if (!re.match(lines.at(i)).hasMatch()) {
                continue;
            }
            if (context == 0) {
                // Concatenation, not a chained arg(): arg() replaces the
                // lowest-numbered remaining marker, so a "%2" inside the
                // substituted path would be rewritten by the next call.
                hits.append(rel + u':' + QString::number(i + 1) + u':' + lines.at(i));
            } else {
                const int from = qMax(0, i - context);
                const int to = qMin(lines.size() - 1, i + context);
                // Only add separator if this match group doesn't overlap with the previous one
                if (lastMatchEnd >= 0 && from > lastMatchEnd) {
                    hits.append(u"--"_s);
                }
                for (int j = from; j <= to; ++j) {
                    const QChar mark = (j == i) ? u':' : u'-';
                    hits.append(rel + mark + QString::number(j + 1) + mark + lines.at(j));
                }
                lastMatchEnd = to;
            }
            if (hits.size() >= 200) {
                return;
            }
        }
    };

    // Search either a single file or all files in a directory recursively
    const QFileInfo info(resolved);
    if (info.isFile()) {
        searchFile(resolved);
    } else {
        QDirIterator it(resolved, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext() && hits.size() < 200) {
            if (readCancelled()) {
                break;
            }
            searchFile(it.next());
        }
    }

    if (readCancelled()) {
        result.ok = false;
        result.cancelled = true;
        result.output = u"Read cancelled because the turn ended."_s;
        return result;
    }

    // Format the search results
    result.output = hits.isEmpty() ? u"No matches."_s : clip(hits.join(u'\n'));
    if (skippedFiles > 0) {
        // The skips are silent otherwise, and the model would read "no
        // matches" as evidence instead of an incomplete search.
        result.output += u"\n[%1 file(s) were skipped: binary or larger than 4 MiB. Matches in them, if any, are not visible here.]"_s
                             .arg(skippedFiles);
    }
    return result;
}

ToolResult ToolRunner::glob(const QJsonObject &args) const
{
    // Initialize the result structure to track success/failure
    ToolResult result;
    result.name = u"glob"_s;

    // Extract the file pattern from the tool arguments
    const QString pattern = args.value(u"pattern"_s).toString();

    // Optional directory to search; defaults to the workspace root. Resolved
    // through the sandbox so the active profile is still honoured.
    QString base = m_sandbox.workspaceRoot();
    const QString dirArg = args.value(u"path"_s).toString().trimmed();
    if (!dirArg.isEmpty()) {
        QString error;
        // The search root is checked before it is walked, so a directory
        // outside the workspace is refused instead of silently yielding
        // nothing.
        if (!m_sandbox.allowsRead(dirArg, &error)) {
            result.ok = false;
            result.output = error;
            return result;
        }
        const QString resolved = m_sandbox.resolve(dirArg, &error);
        if (resolved.isEmpty()) {
            result.ok = false;
            result.output = error;
            return result;
        }
        QDir baseDir(resolved);
        if (!baseDir.exists()) {
            result.ok = false;
            result.output = u"Directory does not exist: %1"_s.arg(resolved);
            return result;
        }
        base = resolved;
    }

    // Collect all matching files by recursively searching from `base`
    QStringList matches;
    bool capped = false;
    int skippedDenied = 0;
    QDirIterator it(base, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (readCancelled()) {
            result.ok = false;
            result.cancelled = true;
            result.output = u"Read cancelled because the turn ended."_s;
            return result;
        }
        if (matches.size() >= 400) {
            capped = true;
            break;
        }
        const QString filePath = it.next();

        // Skip files that are denied by the sandbox for security
        if (m_sandbox.isDenied(filePath)) {
            ++skippedDenied;
            continue;
        }

        // Get the relative path from the workspace root for user-friendly display
        const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(filePath);
        if (isNoisySearchPath(rel)) {
            continue;
        }

        if (Sandbox::globMatch(pattern, rel) || QDir::match(pattern, rel) || QDir::match(pattern, QFileInfo(rel).fileName())) {
            matches.append(rel);
        }
    }

    // Deterministic order so results are comparable across identical calls.
    matches.sort();

    // Format the output with either the matched files or a "no matches" message
    result.output = matches.isEmpty() ? u"No files matched."_s : matches.join(u'\n');
    if (skippedDenied > 0) {
        result.output += u"\n[%1 file(s) hidden by sandbox deny rules.]"_s.arg(skippedDenied);
    }
    if (capped) {
        result.output += u"\n[Listing capped at 400 matches; narrow the pattern or set a path to see more.]"_s;
    }
    return result;
}

bool ToolRunner::startBashProcess(const QJsonObject &args, QString *program, QStringList *programArgs, QString *error) const
{
    // Extract the command to execute from the tool arguments
    const QString command = args.value(u"command"_s).toString();

    // Wrap the command with sandbox security restrictions
    const QStringList wrapped = m_sandbox.wrapCommand(command, error);
    if (wrapped.isEmpty()) {
        return false;
    }
    *program = wrapped.first();
    *programArgs = wrapped.mid(1);
    return true;
}

ToolResult ToolRunner::finishBashOutput(const QString &output, int exitCode, bool normalExit) const
{
    ToolResult result;
    result.name = u"bash"_s;
    result.ok = normalExit && exitCode == 0;
    // Format the output with appropriate truncation and exit code information
    result.output = clip(output.isEmpty() ? u"(no output, exit %1)"_s.arg(exitCode) : output);
    if (!result.ok) {
        result.output += u"\nexit code %1"_s.arg(exitCode);
    }
    return result;
}

ToolResult ToolRunner::bash(const QJsonObject &args)
{
    // Initialize the result structure to track success/failure

    // Create and configure the process for command execution
    QString program;
    QStringList programArgs;
    QString error;
    if (!startBashProcess(args, &program, &programArgs, &error)) {
        ToolResult failed;
        failed.name = u"bash"_s;
        failed.ok = false;
        failed.output = error;
        return failed;
    }

    QProcess process;
    process.setWorkingDirectory(m_sandbox.workspaceRoot());
    process.setProcessChannelMode(QProcess::MergedChannels);
    // Ambient provider credentials and desktop session channels are not
    // inherited by a sandboxed command. Off deliberately keeps the old
    // environment behaviour.
    process.setProcessEnvironment(m_sandbox.processEnvironment());
#ifdef Q_OS_UNIX
    // Give the command its own session so a timeout can take down everything it
    // started. Otherwise kill() only reaps the shell we launched and whatever it
    // backgrounded keeps running and writing after the turn was cancelled.
    process.setChildProcessModifier([]() {
        ::setsid();
    });
#endif

    // Start the process with the wrapped command
    process.start(program, programArgs);

    // Check if the process started successfully
    if (!process.waitForStarted(5000)) {
        ToolResult failed;
        failed.name = u"bash"_s;
        failed.ok = false;
        failed.output = u"Failed to start command: %1"_s.arg(process.errorString());
        return failed;
    }

    // Wait for the process to complete with a timeout, polling in short
    // slices and draining the pipe on every pass. One long blocking
    // waitForFinished() left the draining to Qt's internals; this way a
    // command that writes more than the OS pipe buffer can never wedge both
    // ends of the pipe.
    // Bound memory: a command writing gigabytes would otherwise fill RAM
    // before the timeout kills it (async path caps at kMaxBashOutputChars).
    constexpr qint64 kMaxSyncBashBytes = 4 * 1024 * 1024;
    QByteArray output;
    QElapsedTimer elapsed;
    elapsed.start();
    bool timedOut = false;
    bool outputCapped = false;
    while (true) {
        output += process.readAll();
        if (output.size() > kMaxSyncBashBytes) {
            outputCapped = true;
            break;
        }
        if (process.waitForFinished(50)) {
            break;
        }
        if (process.state() == QProcess::NotRunning) {
            break;
        }
        if (elapsed.elapsed() >= m_timeoutMs) {
            timedOut = true;
            break;
        }
    }
    if (outputCapped) {
#ifdef Q_OS_UNIX
        if (process.processId() > 0) {
            ::kill(-static_cast<pid_t>(process.processId()), SIGKILL);
        }
#endif
        process.kill();
        process.waitForFinished(2000);
        // Keep head+tail like clip(): tail holds the failure summary.
        const QString full = QString::fromUtf8(output.left(kMaxSyncBashBytes));
        ToolResult capped = finishBashOutput(full, -1, false);
        capped.ok = false;
        capped.output = u"Output exceeded %1 MB and the command was killed.\n"_s.arg(
                            kMaxSyncBashBytes / (1024 * 1024))
            + capped.output;
        return capped;
    }
    output += process.readAll();

    if (timedOut) {
        // Kill the process if it times out
#ifdef Q_OS_UNIX
        // The child leads its own session, so the negative pid reaches the whole
        // tree. If setsid() failed there is no such group and this is a no-op,
        // leaving the plain kill() below to reap the shell itself.
        if (process.processId() > 0) {
            ::kill(-static_cast<pid_t>(process.processId()), SIGKILL);
        }
#endif
        process.kill();
        process.waitForFinished(2000);
        ToolResult failed;
        failed.name = u"bash"_s;
        failed.ok = false;
        failed.output = u"Command timed out after %1 ms."_s.arg(m_timeoutMs);
        return failed;
    }

    return finishBashOutput(QString::fromUtf8(output), process.exitCode(),
                            process.exitStatus() == QProcess::NormalExit);
}

void ToolRunner::BashBuffer::append(const QString &chunk, int cap)
{
    if (chunk.isEmpty()) {
        return;
    }
    // Room for the dropped-output marker is reserved in the halves, so the
    // assembled view fits the ceiling: clip() would otherwise cut the middle
    // of the view and take the marker (and the tail's first lines) with it.
    const int half = cap / 2 - 64;
    if (head.size() < half) {
        const int room = half - head.size();
        head += chunk.left(room);
        const QString rest = chunk.mid(room);
        tail += rest;
    } else {
        tail += chunk;
    }
    if (tail.size() > half) {
        droppedChars += tail.size() - half;
        tail = tail.right(half);
    }
}

QString ToolRunner::BashBuffer::view() const
{
    if (tail.isEmpty()) {
        return head;
    }
    if (droppedChars == 0) {
        return head + tail;
    }
    return head + u"\n... [%1 characters of streamed output were dropped] ...\n"_s.arg(droppedChars) + tail;
}

bool ToolRunner::isAsyncReadTool(const QString &toolName)
{
    // Only pure disk tools are offloaded. read_file is deliberately excluded:
    // it must see an open editor buffer, and KTextEditor documents cannot be
    // touched from the worker thread. Tools that use the project graph or LSP
    // provider also stay on the caller's thread.
    static const QSet<QString> kAsyncReadTools = {
        u"list_dir"_s, u"grep"_s, u"glob"_s,
    };
    return kAsyncReadTools.contains(toolName);
}

bool ToolRunner::readCancelled() const
{
    return m_cancelFlag && m_cancelFlag->load();
}

void ToolRunner::cancelReadTools()
{
    for (const auto &flag : std::as_const(m_readJobs)) {
        if (flag) {
            flag->store(true);
        }
    }
    m_readJobs.clear();
}

ToolRunner::~ToolRunner()
{
    // Stop the worker thread here, while the QApplication is still alive:
    // a worker thread that outlives the application crashes teardown in some
    // KF6 builds (KDirWatch's destructor calls deleteLater on a dead thread).
    if (m_workerThread) {
        cancelReadTools();
        m_workerThread->quit();
        // The jobs check the cancellation flag at every file/line boundary.
        // Wait for them rather than destroying a running QThread; the bounded
        // wait covers the common case, and a long filesystem read is allowed
        // to finish because terminating a thread mid-I/O is not safe.
        if (!m_workerThread->wait(5000)) {
            m_workerThread->wait();
        }
        delete m_workerContext;
        m_workerContext = nullptr;
    }
}

bool ToolRunner::runReadToolAsync(const ToolCall &call)
{
    if (!isAsyncReadTool(call.name)) {
        return false;
    }
    const Sandbox sandbox = m_sandbox;
    Q_EMIT bashProgress(call.id, u"Working…"_s);
    if (!m_workerThread) {
        m_workerThread = new QThread(this);
        // A plain QObject moved into the worker thread is the invoke target;
        // the job itself is a lambda, so no moc'd worker class is needed.
        m_workerContext = new QObject();
        m_workerContext->moveToThread(m_workerThread);
        m_workerThread->start();
    }
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_readJobs.append(cancelFlag);
    const QPointer<ToolRunner> guard(this);
    QMetaObject::invokeMethod(m_workerContext, [guard, call, sandbox, cancelFlag]() {
        // The worker must never touch an open KTextEditor document: that is
        // GUI-thread state. setUseOpenDocuments(false) makes the bridge read
        // straight from disk, which is the only safe thing to do here.
        DiskDocumentBridge diskBridge;
        diskBridge.setUseOpenDocuments(false);
        ToolRunner worker(sandbox, &diskBridge);
        worker.m_cancelFlag = cancelFlag;
        ToolResult result = worker.run(call);
        // The per-tool runners build a fresh ToolResult and leave the id empty;
        // the async path keys cards on it, so stamp it here.
        if (result.toolCallId.isEmpty()) {
            result.toolCallId = call.id;
        }
        if (result.name.isEmpty()) {
            result.name = call.name;
        }
        // The runner may be gone by the time the search finishes (turn aborted,
        // loop destroyed); then the result is simply dropped. Post through the
        // application object rather than a raw ToolRunner pointer captured on
        // this thread: reading QPointer::data() here races with destruction on
        // the GUI thread.
        if (QCoreApplication::instance()) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, cancelFlag, result]() {
                if (guard) {
                    guard->m_readJobs.removeAll(cancelFlag);
                    Q_EMIT guard->bashFinished(result);
                }
            }, Qt::QueuedConnection);
        }
    }, Qt::QueuedConnection);
    return true;
}

void ToolRunner::runBashAsync(const ToolCall &call)
{
    const QString callId = call.id;
    QString program;
    QStringList programArgs;
    QString error;
    // The arguments are parsed like in the synchronous path: a call whose
    // arguments only exist as raw JSON would otherwise run an empty command
    // and report success.
    QString parseError;
    const QJsonObject args = parseToolArgs(call, &parseError);
    if (!parseError.isEmpty() && args.isEmpty() && !call.argumentsJson.isEmpty()) {
        ToolResult failed;
        failed.toolCallId = callId;
        failed.name = u"bash"_s;
        failed.ok = false;
        failed.output = parseError;
        Q_EMIT bashFinished(failed);
        return;
    }
    if (!startBashProcess(args, &program, &programArgs, &error)) {
        ToolResult failed;
        failed.toolCallId = callId;
        failed.name = u"bash"_s;
        failed.ok = false;
        failed.output = error;
        Q_EMIT bashFinished(failed);
        return;
    }

    auto *process = new QProcess(this);
    process->setWorkingDirectory(m_sandbox.workspaceRoot());
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setProcessEnvironment(m_sandbox.processEnvironment());
#ifdef Q_OS_UNIX
    // Same session trick as the synchronous path: a timeout must be able to
    // reap the command and anything it started, not just the shell.
    process->setChildProcessModifier([]() {
        ::setsid();
    });
#endif
    // Model-supplied ids are not guaranteed unique (retries/subtasks reuse
    // ids). Overwriting would leak the old QProcess and lose its kill handle,
    // with two finished handlers firing for one id.
    if (m_bashProcesses.contains(callId)) {
        process->deleteLater();
        ToolResult failed;
        failed.toolCallId = callId;
        failed.name = u"bash"_s;
        failed.ok = false;
        failed.output = u"Duplicate tool call id: a bash command with this id is already running."_s;
        Q_EMIT bashFinished(failed);
        return;
    }
    m_bashProcesses.insert(callId, process);

    // Reads whatever has arrived since the last visit and returns the
    // accumulated buffer, so both the progress and the timeout path drain the
    // pipe exactly once.
    auto drain = [this, callId]() -> QString {
        auto *process = m_bashProcesses.value(callId);
        if (!process) {
            return {};
        }
        const QString chunk = QString::fromUtf8(process->readAll());
        if (!chunk.isEmpty()) {
            // The output is accumulated until the command finishes, and the
            // clipping happens only then. A command that writes gigabytes
            // would fill the memory in the meantime, so the buffer has a
            // ceiling that keeps both ends, like clip() does for a finished
            // result: the tail is where a failure summary lives.
            m_bashAccumulated[callId].append(chunk, kMaxBashOutputChars);
        }
        return m_bashAccumulated.value(callId).view();
    };

    connect(process, &QProcess::readyReadStandardOutput, this, [this, callId, drain] {
        const QString soFar = drain();
        if (!soFar.isEmpty()) {
            Q_EMIT bashProgress(callId, soFar);
        }
    });

    // Same timeout the synchronous path honours, on a timer rather than a
    // blocking wait, so the UI thread stays free to render the stream. The
    // settings already clamp m_timeoutMs to at least a second; the bound here
    // keeps a stray zero from turning into "no timeout" on the timer path.
    const int timeoutMs = qMax(1000, m_timeoutMs);
    QTimer *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    timeout->setInterval(timeoutMs);
    connect(timeout, &QTimer::timeout, this, [this, callId, drain] {
        // The timer is parented to the process and is destroyed with it, so it
        // must not be deleted here: the finished handler below still calls
        // stop() on it, and a deferred delete can be processed before the
        // process reports its exit, which would leave that handler holding a
        // dangling pointer.
        auto *process = m_bashProcesses.value(callId);
        if (!process) {
            return;
        }
        // Remember that this call was cut short, so the finished result can
        // say "timed out" instead of reporting an unexplained kill.
        m_bashTimedOut.insert(callId);
        const QString soFar = drain();
        if (!soFar.isEmpty()) {
            Q_EMIT bashProgress(callId, soFar);
        }
#ifdef Q_OS_UNIX
        if (process->processId() > 0) {
            ::kill(-static_cast<pid_t>(process->processId()), SIGKILL);
        }
#endif
        process->kill();
    });

    // A process that fails to start never reports "finished": Qt emits
    // errorOccurred(FailedToStart) and nothing else. Without this handler the
    // tool call was never settled, the agent loop kept its in-flight counter
    // raised and the turn waited for a result that could not arrive.
    connect(process, &QProcess::errorOccurred, this, [this, callId, timeout](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        timeout->stop();
        auto *process = m_bashProcesses.take(callId);
        if (!process) {
            return;
        }
        m_bashAccumulated.remove(callId);
        m_bashTimedOut.remove(callId);
        ToolResult result;
        result.toolCallId = callId;
        result.name = u"bash"_s;
        result.ok = false;
        result.output = u"The command could not be started: %1"_s.arg(process->errorString());
        process->deleteLater();
        Q_EMIT bashFinished(result);
    });

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, callId, drain, timeout, timeoutMs](int, QProcess::ExitStatus) {
        // The command ended on its own: stop the timeout timer, or a process
        // that finished right at the deadline is reported as timed out.
        timeout->stop();
        auto *process = m_bashProcesses.take(callId);
        if (!process) {
            return;
        }
        const bool timedOut = m_bashTimedOut.remove(callId);
        const QString soFar = drain();
        if (!soFar.isEmpty()) {
            // A final progress emission, so the live text in the card is exactly
            // what the finished result reports.
            Q_EMIT bashProgress(callId, soFar);
        }
        ToolResult result = finishBashOutput(m_bashAccumulated.take(callId).view(), process->exitCode(),
                                             process->exitStatus() == QProcess::NormalExit);
        result.toolCallId = callId;
        if (timedOut) {
            // The timer killed the process; without this line the model sees a
            // crashed command and may retry the same thing forever.
            result.ok = false;
            result.output += u"\nCommand timed out after %1 ms."_s.arg(timeoutMs);
        }
        process->deleteLater();
        Q_EMIT bashFinished(result);
    });

    process->start(program, programArgs);
    timeout->start();
}

void ToolRunner::cancelBash(const QString &toolCallId)
{
    auto *process = m_bashProcesses.take(toolCallId);
    if (!process) {
        return;
    }
    // The process is removed from the map, so its "finished" handler would
    // never emit a result. The tool call has to be settled here, otherwise
    // the caller waits for a result that never comes.
    m_bashAccumulated.remove(toolCallId);
    m_bashTimedOut.remove(toolCallId);
    ToolResult cancelled;
    cancelled.toolCallId = toolCallId;
    cancelled.name = u"bash"_s;
    cancelled.ok = false;
    cancelled.cancelled = true;
    cancelled.output = u"The command was cancelled."_s;
    Q_EMIT bashFinished(cancelled);
#ifdef Q_OS_UNIX
    if (process->processId() > 0) {
        ::kill(-static_cast<pid_t>(process->processId()), SIGKILL);
    }
#endif
    process->kill();
    // SIGKILL is delivered at once, but QProcess only notices when the exit
    // notification is read. Waiting briefly reaps the child here, so the
    // runner is not left holding a process that is dead on the kernel side
    // and "running" on the Qt side until teardown.
    process->waitForFinished(500);
    process->deleteLater();
}

ToolResult ToolRunner::queryProjectGraph(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"query_project_graph"_s;

    if (!m_projectGraph || m_projectGraph->getNodeCount() == 0) {
        result.ok = false;
        result.output = u"Project graph is not available yet. Use glob/list_dir/grep instead."_s;
        return result;
    }

    const QString queryType = args.value(u"query_type"_s).toString().trimmed();
    const QString nodeId = args.value(u"node_id"_s).toString();
    const QString relationship = args.value(u"relationship"_s).toString();
    const QString sourceId = args.value(u"source_id"_s).toString();
    const QString targetId = args.value(u"target_id"_s).toString();

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
        GraphNode *node = resolveGraphNode(m_projectGraph, nodeId);
        if (!node) {
            result.ok = false;
            result.output = u"Unknown node: %1"_s.arg(nodeId);
            return result;
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
        GraphNode *source = resolveGraphNode(m_projectGraph, sourceId);
        GraphNode *target = resolveGraphNode(m_projectGraph, targetId);
        if (!source || !target) {
            result.ok = false;
            result.output = u"Need valid source_id and target_id."_s;
            return result;
        }
        if (type == u"find_path"_s) {
            const QList<QString> path = m_projectGraph->findImportPaths(source->id, target->id);
            if (path.isEmpty()) {
                lines.append(u"No import path found."_s);
            } else {
                QStringList labels;
                for (const QString &id : path) {
                    const GraphNode *node = m_projectGraph->getNode(id);
                    labels.append(node ? m_projectGraph->nodeLabel(node) : id);
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
        result.ok = false;
        result.output = u"Unknown query_type. Use summary, nodes, edges, dependencies, dependents, find_related, find_path, or dependency_chain."_s;
        return result;
    }

    result.ok = true;
    result.output = clip(lines.isEmpty() ? u"(no results)"_s : lines.join(u'\n'));
    return result;
}

namespace
{

// Shared read-path resolution for the LSP tools: sandbox check first, then
// resolve against the workspace, exactly like read_file.
bool resolveReadPath(const Sandbox &sandbox, const QString &path, QString *resolved, QString *error)
{
    if (path.isEmpty()) {
        *resolved = QString();
        return true;
    }
    if (!sandbox.allowsRead(path, error)) {
        return false;
    }
    const QString r = sandbox.resolve(path, error);
    if (r.isEmpty() && sandbox.profile() == SandboxProfile::Strict) {
        return false;
    }
    *resolved = r.isEmpty() ? QDir(sandbox.workspaceRoot()).absoluteFilePath(path) : r;
    return true;
}

// The identifier at a 1-based line/column of a file, read through the bridge
// so an open buffer is seen. Empty when the position is not on a word.
QString wordAtPosition(const DocumentBridge *bridge, const QString &resolvedPath, int line, int column)
{
    if (!bridge || line < 1) {
        return {};
    }
    QStringList page;
    if (!bridge->readDocumentLines(resolvedPath, line, 1, &page, nullptr) || page.isEmpty()) {
        return {};
    }
    const QString text = page.first();
    static const QRegularExpression wordRe(u"\\w+"_s);
    const int col0 = qBound(0, column - 1, text.size());
    QRegularExpressionMatchIterator it = wordRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedStart() <= col0 && col0 < m.capturedEnd()) {
            return m.captured(0);
        }
    }
    return {};
}

// Text-scan fallback for references/definitions when no language server
// answers: every word-boundary hit of `word` in the workspace, same guards as
// grep (deny globs, sandbox read check, noisy paths, size and binary caps).
QStringList scanWordReferences(const Sandbox &sandbox, const DocumentBridge *bridge,
                               const QString &word, int maxHits,
                               const std::atomic_bool *cancel = nullptr)
{
    QStringList hits;
    if (word.isEmpty()) {
        return hits;
    }
    const QRegularExpression re(u"\\b%1\\b"_s.arg(QRegularExpression::escape(word)));
    const QString root = sandbox.workspaceRoot();
    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && hits.size() < maxHits) {
        if (cancel && cancel->load()) {
            return hits;
        }
        const QString filePath = it.next();
        const QString rel = QDir(root).relativeFilePath(filePath);
        if (isNoisySearchPath(rel) || sandbox.isDenied(filePath) || !sandbox.allowsRead(filePath, nullptr)) {
            continue;
        }
        if (QFileInfo(filePath).size() > 4 * 1024 * 1024) {
            continue;
        }
        QString contents;
        if (!bridge || !bridge->readDocument(filePath, &contents)) {
            continue;
        }
        if (contents.left(8192).contains(u'\0')) {
            continue;
        }
        const QStringList lines = contents.split(u'\n');
        for (int i = 0; i < lines.size() && hits.size() < maxHits; ++i) {
            if (cancel && cancel->load()) {
                return hits;
            }
            if (re.match(lines.at(i)).hasMatch()) {
                hits.append(rel + u':' + QString::number(i + 1) + u':' + lines.at(i).trimmed().left(200));
            }
        }
    }
    return hits;
}

QString severityName(int severity)
{
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
}

} // namespace

ToolResult ToolRunner::lspDiagnostics(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"lsp_diagnostics"_s;

    if (!m_lsp || !m_lsp->isAvailable()) {
        result.ok = false;
        result.output = u"No language server available: Kate's LSP plugin (lspclientplugin) is not loaded. "
                        u"Run a build, test, or lint command with bash to get diagnostics instead."_s;
        return result;
    }

    QString error;
    QString resolved;
    if (!resolveReadPath(m_sandbox, args.value(u"path"_s).toString(), &resolved, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    const QString severityFilter = args.value(u"severity"_s).toString().trimmed().toLower();

    QList<LspDiagnostic> diags = m_lsp->diagnostics(resolved);
    int minSeverity = 4;
    if (severityFilter == u"error"_s) {
        minSeverity = 1;
    } else if (severityFilter == u"warning"_s) {
        minSeverity = 2;
    } else if (severityFilter == u"info"_s || severityFilter == u"information"_s) {
        minSeverity = 3;
    }
    // Severity filters keep everything at least as severe as the threshold:
    // "warning" means warnings and errors, like an IDE's filter.
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
        const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(d.path);
        lines.append(u"%1:%2:%3: %4: %5%6"_s
                         .arg(rel.isEmpty() ? d.path : rel)
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
                      .arg(resolved.isEmpty() ? u" in the workspace"_s : u" in %1"_s.arg(QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved))));
    if (resolved.isEmpty()) {
        // Workspace-wide: the recent server messages are the closest thing to
        // a build log the LSP surface offers.
        const QStringList messages = m_lsp->recentMessages(5);
        if (!messages.isEmpty()) {
            lines.append(u"Recent LSP messages:"_s);
            for (const QString &m : messages) {
                lines.append(u"  "_s + m);
            }
        }
    }
    result.ok = true;
    result.output = clip(lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::lspSymbols(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"lsp_symbols"_s;

    QString error;
    QString resolved;
    if (!resolveReadPath(m_sandbox, args.value(u"path"_s).toString(), &resolved, &error) || resolved.isEmpty()) {
        result.ok = false;
        result.output = resolved.isEmpty() ? u"path is required."_s : error;
        return result;
    }

    QList<LspSymbol> symbols;
    if (m_lsp && m_lsp->isAvailable()) {
        symbols = m_lsp->documentSymbols(resolved);
    }
    if (symbols.isEmpty() && m_projectGraph) {
        // Fallback: the project graph's own symbol index.
        for (const GraphSymbol &s : m_projectGraph->symbolsInFile(resolved)) {
            LspSymbol sym;
            sym.name = s.name;
            sym.kind = s.kind;
            sym.line = s.line;
            sym.column = s.column;
            sym.endLine = s.endLine;
            symbols.append(sym);
        }
    }
    if (symbols.isEmpty()) {
        result.ok = false;
        result.output = u"No symbols indexed for %1. The file may not be in the project graph yet; read_file it first or use grep."_s
                            .arg(QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved));
        return result;
    }

    std::sort(symbols.begin(), symbols.end(), [](const LspSymbol &a, const LspSymbol &b) {
        return a.line < b.line;
    });
    QStringList lines;
    for (const LspSymbol &s : symbols) {
        lines.append(u"%1:%2: %3 %4%5"_s
                         .arg(s.line)
                         .arg(s.column > 0 ? s.column : 1)
                         .arg(s.kind)
                         .arg(s.name)
                         .arg(s.endLine > s.line ? u"  (lines %1-%2)"_s.arg(s.line).arg(s.endLine) : QString()));
    }
    result.ok = true;
    result.output = clip(lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::lspHover(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"lsp_hover"_s;

    QString error;
    QString resolved;
    if (!resolveReadPath(m_sandbox, args.value(u"path"_s).toString(), &resolved, &error) || resolved.isEmpty()) {
        result.ok = false;
        result.output = resolved.isEmpty() ? u"path is required."_s : error;
        return result;
    }
    const int line = qMax(1, jsonInt(args.value(u"line"_s), 1));
    const int column = qMax(1, jsonInt(args.value(u"column"_s), 1));

    if (m_lsp && m_lsp->isAvailable()) {
        const QString text = m_lsp->hover(resolved, line, column);
        if (!text.trimmed().isEmpty()) {
            result.ok = true;
            result.output = clip(text);
            return result;
        }
    }

    // Fallback: the enclosing symbol from the project graph plus its
    // declaration lines, which is most of what a hover shows.
    if (m_projectGraph) {
        const GraphSymbol sym = m_projectGraph->enclosingSymbol(resolved, line);
        if (!sym.name.isEmpty()) {
            QStringList lines;
            lines.append(u"%1 %2 at %3:%4"_s
                             .arg(sym.kind, sym.name,
                                  QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved))
                             .arg(sym.line));
            if (m_bridge) {
                const int last = sym.endLine > sym.line ? qMin(sym.endLine, sym.line + 4) : sym.line;
                QStringList page;
                if (m_bridge->readDocumentLines(resolved, sym.line, last - sym.line + 1, &page, nullptr)) {
                    for (int i = 0; i < page.size(); ++i) {
                        lines.append(u"%1: %2"_s.arg(sym.line + i).arg(page.at(i)));
                    }
                }
            }
            result.ok = true;
            result.output = clip(lines.join(u'\n'));
            return result;
        }
    }

    const QString word = wordAtPosition(m_bridge, resolved, line, column);
    if (word.isEmpty()) {
        result.ok = false;
        result.output = u"No symbol or word at %1:%2:%3."_s
                            .arg(QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved))
                            .arg(line)
                            .arg(column);
        return result;
    }
    result.ok = true;
    result.output = clip(u"%1 at %2:%3:%4 (no language server; word from the text)"_s
                             .arg(word, QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved))
                             .arg(line)
                             .arg(column));
    return result;
}

ToolResult ToolRunner::lspDefinition(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"lsp_definition"_s;

    QString error;
    QString resolved;
    if (!resolveReadPath(m_sandbox, args.value(u"path"_s).toString(), &resolved, &error) || resolved.isEmpty()) {
        result.ok = false;
        result.output = resolved.isEmpty() ? u"path is required."_s : error;
        return result;
    }
    const int line = qMax(1, jsonInt(args.value(u"line"_s), 1));
    const int column = qMax(1, jsonInt(args.value(u"column"_s), 1));

    if (m_lsp && m_lsp->isAvailable()) {
        const QList<LspLocation> locations = m_lsp->definitions(resolved, line, column);
        if (!locations.isEmpty()) {
            QStringList lines;
            for (const LspLocation &loc : locations) {
                lines.append(u"%1:%2:%3"_s
                                 .arg(QDir(m_sandbox.workspaceRoot()).relativeFilePath(loc.path).isEmpty()
                                          ? loc.path
                                          : QDir(m_sandbox.workspaceRoot()).relativeFilePath(loc.path))
                                 .arg(loc.line)
                                 .arg(loc.column > 0 ? loc.column : 1));
            }
            result.ok = true;
            result.output = clip(lines.join(u'\n'));
            return result;
        }
    }

    // Fallback: the declaration is the symbol's indexed definition line.
    const QString word = args.value(u"symbol"_s).toString().trimmed().isEmpty()
        ? wordAtPosition(m_bridge, resolved, line, column)
        : args.value(u"symbol"_s).toString().trimmed();
    if (word.isEmpty()) {
        result.ok = false;
        result.output = u"No symbol at %1:%2:%3."_s
                            .arg(QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved))
                            .arg(line)
                            .arg(column);
        return result;
    }
    QStringList lines;
    if (m_projectGraph) {
        for (const GraphSymbol &s : m_projectGraph->symbolsInFile(resolved)) {
            if (s.name == word) {
                lines.append(u"%1:%2: %3 %4 (declaration)"_s
                                 .arg(QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved))
                                 .arg(s.line)
                                 .arg(s.kind, s.name));
            }
        }
    }
    // And where the name is defined/used elsewhere, textually.
    const QStringList refs = scanWordReferences(m_sandbox, m_bridge, word, 60, m_cancelFlag.get());
    for (const QString &ref : refs) {
        lines.append(ref + u"  (text match)"_s);
    }
    if (lines.isEmpty()) {
        result.ok = false;
        result.output = u"No definition found for %1 (no language server, no indexed declaration, no text matches)."_s.arg(word);
        return result;
    }
    result.ok = true;
    result.output = clip(lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::lspReferences(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"lsp_references"_s;

    QString error;
    QString resolved;
    if (!resolveReadPath(m_sandbox, args.value(u"path"_s).toString(), &resolved, &error) || resolved.isEmpty()) {
        result.ok = false;
        result.output = resolved.isEmpty() ? u"path is required."_s : error;
        return result;
    }
    const int line = qMax(1, jsonInt(args.value(u"line"_s), 1));
    const int column = qMax(1, jsonInt(args.value(u"column"_s), 1));
    const bool includeDeclaration = jsonBool(args.value(u"include_declaration"_s), true);

    if (m_lsp && m_lsp->isAvailable()) {
        const QList<LspLocation> locations = m_lsp->references(resolved, line, column, includeDeclaration);
        if (!locations.isEmpty()) {
            QStringList lines;
            for (const LspLocation &loc : locations) {
                const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(loc.path);
                lines.append(u"%1:%2:%3"_s.arg(rel.isEmpty() ? loc.path : rel).arg(loc.line).arg(loc.column > 0 ? loc.column : 1));
            }
            result.ok = true;
            result.output = clip(lines.join(u'\n'));
            return result;
        }
    }

    const QString word = args.value(u"symbol"_s).toString().trimmed().isEmpty()
        ? wordAtPosition(m_bridge, resolved, line, column)
        : args.value(u"symbol"_s).toString().trimmed();
    if (word.isEmpty()) {
        result.ok = false;
        result.output = u"No symbol at %1:%2:%3."_s
                            .arg(QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved))
                            .arg(line)
                            .arg(column);
        return result;
    }
    const QStringList hits = scanWordReferences(m_sandbox, m_bridge, word, 200, m_cancelFlag.get());
    if (hits.isEmpty()) {
        result.ok = false;
        result.output = u"No references to %1 found (no language server; text scan found nothing)."_s.arg(word);
        return result;
    }
    QStringList lines;
    lines.append(u"%1 reference(s) to %2 (text scan, no language server):"_s.arg(hits.size()).arg(word));
    lines.append(hits);
    result.ok = true;
    result.output = clip(lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::lspCodeActions(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"lsp_code_actions"_s;

    QString error;
    QString resolved;
    if (!resolveReadPath(m_sandbox, args.value(u"path"_s).toString(), &resolved, &error) || resolved.isEmpty()) {
        result.ok = false;
        result.output = resolved.isEmpty() ? u"path is required."_s : error;
        return result;
    }
    const int line = qMax(1, jsonInt(args.value(u"line"_s), 1));
    const int column = qMax(1, jsonInt(args.value(u"column"_s), 1));

    QStringList actions;
    if (m_lsp && m_lsp->isAvailable()) {
        actions = m_lsp->codeActions(resolved, line, column);
    }
    if (actions.isEmpty()) {
        result.ok = false;
        result.output = u"No code actions offered at %1:%2:%3 (no language server running for this file, or none at that position)."_s
                            .arg(QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved))
                            .arg(line)
                            .arg(column);
        return result;
    }
    result.ok = true;
    QStringList lines;
    lines.append(u"Code actions at %1:%2:"_s
                     .arg(QDir(m_sandbox.workspaceRoot()).relativeFilePath(resolved))
                     .arg(line));
    for (int i = 0; i < actions.size(); ++i) {
        lines.append(u"%1. %2"_s.arg(i + 1).arg(actions.at(i)));
    }
    result.output = clip(lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::question(const QJsonObject &args)
{
    Q_UNUSED(args)
    ToolResult result;
    result.name = u"question"_s;
    result.ok = false;
    result.output = u"The question tool needs an interactive user to answer it. "
                    u"Choose your next action from what you already know, or explain what you need and stop."_s;
    return result;
}

ToolResult ToolRunner::suggest(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"suggest"_s;

    const QString text = suggestionTextFromToolArguments(args).trimmed();
    if (text.isEmpty()) {
        result.ok = false;
        result.output = u"suggestion must not be empty: describe what you are suggesting."_s;
        return result;
    }

    // The anchor is optional: advice without a place to look is still valid.
    // The card is built from the tool call itself (ToolRunner::describe carries
    // path and line), so the line is validated here — an impossible anchor is
    // an error the model can act on — but not carried in the result.
    const QJsonValue lineValue = args.value(u"line"_s);
    if (!lineValue.isUndefined() && !lineValue.isNull()) {
        // The model often sends numbers as strings ("5"); QJsonValue::toInt()
        // silently yields 0 for those, which would drop the anchor. jsonInt
        // accepts both forms.
        const int parsed = jsonInt(lineValue, -1);
        if (parsed < 1) {
            result.ok = false;
            result.output = u"line must be a 1-based line number, or leave it out."_s;
            return result;
        }
    }

    // Nothing is executed here: the card the user sees is built from the tool
    // call itself, and this confirmation is what goes back to the model.
    result.ok = true;
    result.output = u"Suggestion posted for the user to accept or dismiss."_s;
    return result;
}

ToolResult ToolRunner::writePlanFile(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"write_plan_file"_s;

    const QString content = args.value(u"content"_s).toString();
    if (content.trimmed().isEmpty()) {
        result.ok = false;
        result.output = u"content must not be empty: pass the full plan as markdown."_s;
        return result;
    }
    if (!m_bridge) {
        result.ok = false;
        result.output = u"No document bridge is available."_s;
        return result;
    }

    // Kate owns the destination, so the sandbox is deliberately not consulted:
    // a read-only or strict profile must not stop the agent from writing down
    // its plan, and the model never chooses the path.
    const QString path = planFilePath(m_sandbox.workspaceRoot());
    QString error;
    if (!m_bridge->writeDocument(path, content, &error)) {
        result.ok = false;
        result.output = error.isEmpty() ? u"Could not write the plan file."_s : error;
        return result;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    result.ok = true;
    result.output = u"Plan written to %1 (%2 bytes). The user can open that file to follow the implementation; "
                      "later calls overwrite it, so keep it current as the work moves."_s
                      .arg(path)
                      .arg(content.toUtf8().size());
    return result;
}

ToolResult ToolRunner::applyPatch(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"apply_patch"_s;

    const QString patch = args.value(u"patch"_s).toString();
    if (patch.trimmed().isEmpty()) {
        result.ok = false;
        result.output = u"patch must not be empty: pass a unified diff with ---/+++ file headers and @@ hunks."_s;
        return result;
    }

    const QList<PatchFile> files = parseUnifiedPatch(patch);
    if (files.isEmpty()) {
        result.ok = false;
        result.output = u"No file sections found: the patch needs '--- a/<path>' / '+++ b/<path>' headers and @@ hunks."_s;
        return result;
    }

    // Phase 1: resolve, read and compute every new file content. Nothing is
    // written yet, so a failing hunk anywhere leaves the workspace untouched.
    struct PlannedWrite {
        QString resolved;
        QString displayPath;
        QString newContent;
        QString oldContent;
        bool existed = false;
        bool isDelete = false;
    };
    QList<PlannedWrite> planned;
    QSet<QString> plannedPaths;
    for (const PatchFile &file : files) {
        // A deletion may arrive without hunks (the whole file goes away); an
        // edit must have at least one.
        if (file.path.isEmpty() || (!file.isDelete && file.hunks.isEmpty())) {
            result.ok = false;
            result.output = u"Patch section without a path or without hunks: %1"_s.arg(file.oldPath.isEmpty() ? file.path : file.oldPath);
            return result;
        }
        QString error;
        if (!m_sandbox.allowsWrite(file.path, &error)) {
            result.ok = false;
            result.output = error;
            return result;
        }
        const QString resolved = m_sandbox.resolve(file.path, &error, true);
        if (resolved.isEmpty()) {
            result.ok = false;
            result.output = error;
            return result;
        }
        if (plannedPaths.contains(resolved)) {
            result.ok = false;
            result.output = u"Patch names %1 more than once; split it into one section per file so the changes cannot overwrite each other."_s.arg(file.path);
            return result;
        }
        plannedPaths.insert(resolved);

        QString current;
        bool existed = false;
        if (m_bridge && m_bridge->readDocument(resolved, &current)) {
            existed = true;
        } else {
            QFile disk(resolved);
            if (disk.open(QIODevice::ReadOnly | QIODevice::Text)) {
                current = QString::fromUtf8(disk.readAll());
                disk.close();
                existed = true;
            }
        }
        if (!existed && file.isDelete) {
            result.ok = false;
            result.output = u"Patch deletes %1, but that file does not exist."_s.arg(file.path);
            return result;
        }
        if (!existed && !file.isNew) {
            result.ok = false;
            result.output = u"Cannot read %1: file not found."_s.arg(file.path);
            return result;
        }
        if (file.isDelete) {
            // +++ /dev/null: the whole file goes away; no hunks to apply.
            planned.append({resolved, file.path, QString(), current, existed, true});
            continue;
        }
        QString applyError;
        const QString newContent = applyPatchToContent(current, file.hunks, &applyError);
        if (!applyError.isEmpty()) {
            result.ok = false;
            result.output = u"Patch does not apply to %1: %2"_s.arg(file.path, applyError);
            return result;
        }
        planned.append({resolved, file.path, newContent, current, existed, false});
    }

    // Phase 2: write everything. If a write fails, undo the files already
    // written so the tool's documented all-or-nothing contract holds even when
    // the failure happens late (a permission error, an open modified buffer,
    // a full disk).
    const auto writeContent = [this](const QString &resolved, const QString &expected, const QString &content,
                                     bool existed, QString *error) {
        if (!m_bridge) {
            if (error) {
                *error = u"No document bridge is available."_s;
            }
            return false;
        }
        if (!existed) {
            return m_bridge->writeDocumentIfMissing(resolved, content, error);
        }
        return m_bridge->writeDocumentIfUnchanged(resolved, expected, content, error);
    };
    const auto removeFile = [this](const QString &resolved, QString *error) {
        if (m_bridge) {
            return m_bridge->removeDocument(resolved, error);
        }
        if (!QFileInfo::exists(resolved)) {
            if (error) {
                *error = u"File does not exist."_s;
            }
            return false;
        }
        if (!QFile::remove(resolved)) {
            if (error) {
                *error = u"Could not delete the file."_s;
            }
            return false;
        }
        return true;
    };

    QStringList written;
    int applied = 0;
    for (const PlannedWrite &w : planned) {
        QString error;
        bool ok = false;
        if (w.isDelete) {
            // Do not delete a file that changed after phase 1 read it. The
            // delete path used to skip this check entirely.
            QString current;
            if (!m_bridge || !m_bridge->readDocument(w.resolved, &current)) {
                error = u"The file could not be read before deleting; it may have been removed or made unreadable."_s;
            } else if (current != w.oldContent) {
                error = u"The file changed after it was read, so it was not deleted."_s;
            } else {
                ok = removeFile(w.resolved, &error);
            }
        } else {
            ok = writeContent(w.resolved, w.oldContent, w.newContent, w.existed, &error);
        }
        if (!ok) {
            // Roll back in reverse order. A failure here cannot be hidden: the
            // caller must know the workspace may be partially changed.
            int rolledBack = 0;
            QStringList rollbackErrors;
            for (int i = applied - 1; i >= 0; --i) {
                const PlannedWrite &undo = planned.at(i);
                QString undoError;
                bool undone = false;
                if (undo.isDelete) {
                    // The file was removed, so re-create it. Going through
                    // writeDocumentIfUnchanged() would re-read a file that no
                    // longer exists and always fail.
                    undone = m_bridge && m_bridge->writeDocumentIfMissing(undo.resolved, undo.oldContent, &undoError);
                } else if (undo.existed) {
                    undone = writeContent(undo.resolved, undo.newContent, undo.oldContent, true, &undoError);
                } else {
                    undone = removeFile(undo.resolved, &undoError);
                }
                if (undone) {
                    ++rolledBack;
                } else {
                    rollbackErrors.append(u"%1 (%2)"_s.arg(undo.displayPath, undoError));
                }
            }
            result.ok = false;
            result.output = u"Patch computed cleanly but %1 %2 failed: %3"_s
                                .arg(w.isDelete ? u"deleting"_s : u"writing"_s, w.displayPath, error);
            result.output += u"\nRolled back %1 of %2 already-applied file(s)."_s.arg(rolledBack).arg(applied);
            if (!rollbackErrors.isEmpty()) {
                result.output += u"\nRollback failed for: %1. Check these files before continuing."_s.arg(rollbackErrors.join(u", "_s));
            }
            return result;
        }
        written.append(w.isDelete ? u"deleted %1"_s.arg(w.displayPath) : u"patched %1"_s.arg(w.displayPath));
        ++applied;
    }

    result.ok = true;
    result.output = u"Applied patch to %1 file(s):\n%2"_s.arg(planned.size()).arg(written.join(u'\n'));
    result.rawOutput = result.output;
    return result;
}

ToolResult ToolRunner::memoryWrite(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"memory_write"_s;
    const QString content = args.value(u"content"_s).toString().trimmed();
    if (content.isEmpty()) {
        result.ok = false;
        result.output = u"memory_write needs non-empty content."_s;
        return result;
    }

    QString error;
    const QString resolved = memoryWriteTarget(m_sandbox, args, &error);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }
    if (!QDir().mkpath(QFileInfo(resolved).absolutePath())) {
        result.ok = false;
        result.output = u"Could not create the memory directory for %1."_s.arg(resolved);
        return result;
    }

    const bool existed = QFileInfo::exists(resolved);
    QString existing;
    if (existed) {
        readMemoryFile(resolved, m_bridge, &existing);
    }
    const QString body = memoryNoteBody(args);
    const QByteArray payload = body.toUtf8();
    bool written = false;
    if (m_bridge) {
        written = existed
            ? m_bridge->writeDocumentIfUnchanged(resolved, existing, body, &error)
            : m_bridge->writeDocumentIfMissing(resolved, body, &error);
    } else {
        QSaveFile file(resolved);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (file.write(payload) == payload.size() && file.commit()) {
                written = true;
            } else {
                error = file.errorString();
            }
        } else {
            error = file.errorString();
        }
    }
    if (!written) {
        result.ok = false;
        result.output = u"Could not write the memory note: %1"_s.arg(error);
        return result;
    }

    result.ok = true;
    result.output = u"Remembered '%1' in the %2 memory scope. The note is loaded on future turns."_s
                        .arg(args.value(u"topic"_s).toString(),
                             args.value(u"scope"_s).toString(u"project"_s))
        + u"\nPath: "_s + resolved;
    result.rawOutput = result.output;
    return result;
}

ToolResult ToolRunner::todoWrite(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"todo_write"_s;

    const QJsonArray todos = args.value(u"todos"_s).toArray();
    if (todos.isEmpty()) {
        result.ok = false;
        result.output = u"todos must be a non-empty array of {content, status}."_s;
        return result;
    }
    QJsonArray stored;
    int index = 0;
    for (const QJsonValue &value : todos) {
        const QJsonObject todo = value.toObject();
        const QString content = todo.value(u"content"_s).toString().trimmed();
        if (content.isEmpty()) {
            result.ok = false;
            result.output = u"Every todo needs a content string (item %1 is empty)."_s.arg(index + 1);
            return result;
        }
        QString status = todo.value(u"status"_s).toString().trimmed().toLower().replace(u'-', u'_');
        if (status.isEmpty()) {
            status = u"pending"_s;
        }
        if (status != u"pending"_s && status != u"in_progress"_s && status != u"completed"_s) {
            result.ok = false;
            result.output = u"Invalid status '%1' for todo %2: use pending, in_progress, or completed."_s.arg(status, content.left(40));
            return result;
        }
        QJsonObject step;
        step.insert(u"id"_s, QString::number(++index));
        step.insert(u"description"_s, content);
        step.insert(u"completed"_s, status == u"completed"_s);
        step.insert(u"inProgress"_s, status == u"in_progress"_s);
        stored.append(step);
    }
    m_todos = stored;
    result.ok = true;
    result.output = u"Task list updated (%1 item(s))."_s.arg(stored.size());
    // The UI reads the list from the store; carry it in the result as well so
    // the ACP path (which has no ToolRunner) can forward the same array.
    result.rawOutput = QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact));
    return result;
}

ToolResult ToolRunner::todoRead(const QJsonObject &args)
{
    Q_UNUSED(args)
    ToolResult result;
    result.name = u"todo_read"_s;
    result.ok = true;
    if (m_todos.isEmpty()) {
        result.output = u"(no todos yet)"_s;
    } else {
        QStringList lines;
        for (const QJsonValue &value : m_todos) {
            const QJsonObject todo = value.toObject();
            const QString status = todo.value(u"completed"_s).toBool() ? u"completed"_s
                                   : (todo.value(u"inProgress"_s).toBool() ? u"in_progress"_s : u"pending"_s);
            lines.append(u"- [%1] %2"_s.arg(status, todo.value(u"description"_s).toString()));
        }
        result.output = lines.join(u'\n');
    }
    result.rawOutput = QString::fromUtf8(QJsonDocument(m_todos).toJson(QJsonDocument::Compact));
    return result;
}

ToolResult ToolRunner::think(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"think"_s;
    const QString thought = args.value(u"thought"_s).toString().trimmed();
    if (thought.isEmpty()) {
        result.ok = false;
        result.output = u"thought must not be empty."_s;
        return result;
    }
    result.ok = true;
    result.output = u"Recorded %1 character(s) of reasoning."_s.arg(thought.size());
    result.rawOutput = thought;
    return result;
}

ToolResult ToolRunner::exitPlanMode(const QJsonObject &args)
{
    Q_UNUSED(args)
    ToolResult result;
    result.name = u"exit_plan_mode"_s;
    // The runner only validates; flipping plan mode is the loop's/UI's call,
    // because the setting belongs to the chat, not to the tool.
    result.ok = true;
    result.output = u"Plan mode exit requested."_s;
    return result;
}


namespace
{

QString formatDebugVarLines(const QList<DebugVariable> &vars, const QString &filterLower, int indent = 0)
{
    QStringList out;
    const QString pad(indent * 2, u' ');
    for (const DebugVariable &v : vars) {
        if (indent == 0 && !filterLower.isEmpty() && !v.name.toLower().contains(filterLower)) {
            continue;
        }
        QString line = pad + v.name;
        if (!v.type.isEmpty()) {
            line += u" : "_s + v.type;
        }
        line += u" = "_s + (v.value.isEmpty() ? u"<unavailable>"_s : v.value);
        out.append(line);
        if (!v.children.isEmpty() && indent < 2) {
            const QString childText = formatDebugVarLines(v.children, QString(), indent + 1);
            if (!childText.isEmpty()) {
                out.append(childText);
            }
        }
    }
    return out.join(u'\n');
}

QString formatDebugUnavailable()
{
    return u"Kate's Debugger plugin (kategdbplugin) is not loaded or unavailable in this window. "
           u"Enable Settings -> Configure Kate -> Plugins -> Debugger (GDB / DAP) or run a debugger via bash."_s;
}

} // namespace

ToolResult ToolRunner::debugStatus(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"debug_status"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    QStringList lines;
    const DebugSessionState st = m_debug->sessionState();
    lines.append(u"State: %1"_s.arg(DebugProvider::stateName(st)));
    const QString target = m_debug->activeTarget();
    if (!target.isEmpty()) {
        lines.append(u"Active target: %1"_s.arg(target));
    }

    const DebugStopInfo stop = m_debug->currentStop();
    if (st == DebugSessionState::Paused || !stop.reason.isEmpty() || !stop.file.isEmpty()) {
        QString stopLine = u"Stop: %1"_s.arg(stop.reason.isEmpty() ? u"paused"_s : stop.reason);
        if (!stop.description.isEmpty()) {
            stopLine += u" (%1)"_s.arg(stop.description);
        }
        if (!stop.file.isEmpty()) {
            const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(stop.file);
            stopLine += u" at %1:%2"_s.arg(rel.startsWith(u".."_s) ? stop.file : rel).arg(stop.line);
        }
        if (stop.threadId >= 0) {
            stopLine += u" [thread %1]"_s.arg(stop.threadId);
        }
        if (stop.isCrashOrException) {
            stopLine += u" [CRASH/EXCEPTION]"_s;
        }
        lines.append(stopLine);
    }

    const QList<DebugThread> threads = m_debug->threads();
    if (!threads.isEmpty()) {
        lines.append(u"Threads (%1):"_s.arg(threads.size()));
        for (const DebugThread &t : threads) {
            lines.append(u"  %1#%2 %3 (%4)"_s
                             .arg(t.active ? u"* "_s : u"  "_s)
                             .arg(t.id)
                             .arg(t.name.isEmpty() ? u"thread"_s : t.name,
                                  t.state.isEmpty() ? u"unknown"_s : t.state));
        }
    }

    const QList<DebugTarget> targets = m_debug->targets();
    if (!targets.isEmpty()) {
        lines.append(u"Configured targets (%1):"_s.arg(targets.size()));
        for (const DebugTarget &t : targets) {
            lines.append(u"  %1%2%3"_s
                             .arg(t.active ? u"* "_s : u"  "_s,
                                  t.name,
                                  t.executable.isEmpty() ? QString() : u" -> "_s + t.executable));
        }
    }

    const QList<DebugBreakpoint> bps = m_debug->breakpoints();
    lines.append(u"Breakpoints: %1"_s.arg(bps.size()));

    if (jsonBool(args.value(u"include_output"_s))) {
        const QString out = m_debug->recentOutput(4000).trimmed();
        if (!out.isEmpty()) {
            lines.append(u"\nRecent debugger output:\n"_s + out);
        }
    }

    result.ok = true;
    result.output = clip(lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::debugStack(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"debug_stack"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    const int threadId = args.value(u"thread_id"_s).toInt(-1);
    const int maxFrames = qBound(1, args.value(u"max_frames"_s).toInt(40), 200);
    const QList<DebugStackFrame> frames = m_debug->stackFrames(threadId);
    if (frames.isEmpty()) {
        result.ok = true;
        result.output = u"No stack frames available (state: %1)."_s.arg(DebugProvider::stateName(m_debug->sessionState()));
        return result;
    }

    QStringList lines;
    const int count = qMin(frames.size(), maxFrames);
    for (int i = 0; i < count; ++i) {
        const DebugStackFrame &f = frames.at(i);
        QString loc;
        if (!f.file.isEmpty()) {
            const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(f.file);
            loc = u"%1:%2"_s.arg(rel.startsWith(u".."_s) ? f.file : rel).arg(f.line);
        }
        lines.append(u"%1#%2 %3%4"_s
                         .arg(f.active ? u"-> "_s : u"   "_s)
                         .arg(f.level)
                         .arg(f.function.isEmpty() ? u"<unknown>"_s : f.function)
                         .arg(loc.isEmpty() ? QString() : u" at "_s + loc));
    }
    if (frames.size() > count) {
        lines.append(u"... (%1 more frames)"_s.arg(frames.size() - count));
    }

    result.ok = true;
    result.output = clip(lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::debugLocals(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"debug_locals"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    const int frameLevel = args.value(u"frame_level"_s).toInt(-1);
    const QString filter = args.value(u"filter"_s).toString().trimmed().toLower();
    const QList<DebugVariable> vars = m_debug->locals(frameLevel);
    if (vars.isEmpty()) {
        result.ok = true;
        result.output = u"No local variables available (state: %1)."_s.arg(DebugProvider::stateName(m_debug->sessionState()));
        return result;
    }

    const QString formatted = formatDebugVarLines(vars, filter);
    result.ok = true;
    result.output = formatted.isEmpty()
        ? u"No local variables matched filter '%1'."_s.arg(filter)
        : clip(formatted);
    return result;
}

ToolResult ToolRunner::debugBreakpoints(const QJsonObject &args) const
{
    Q_UNUSED(args)
    ToolResult result;
    result.name = u"debug_breakpoints"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    const QList<DebugBreakpoint> bps = m_debug->breakpoints();
    if (bps.isEmpty()) {
        result.ok = true;
        result.output = u"No breakpoints set."_s;
        return result;
    }

    QStringList lines;
    for (const DebugBreakpoint &bp : bps) {
        const QString rel = bp.file.isEmpty() ? bp.functionName
                                              : QDir(m_sandbox.workspaceRoot()).relativeFilePath(bp.file);
        const QString loc = bp.file.isEmpty()
            ? rel
            : u"%1:%2"_s.arg(rel.startsWith(u".."_s) ? bp.file : rel).arg(bp.line);
        QString line = u"[%1] %2 (%3%4)"_s
                           .arg(bp.id >= 0 ? QString::number(bp.id) : u"*"_s,
                                loc,
                                bp.enabled ? u"enabled"_s : u"disabled"_s,
                                bp.verified ? QString() : u", unverified"_s);
        if (!bp.condition.isEmpty()) {
            line += u" if (%1)"_s.arg(bp.condition);
        }
        if (!bp.hitCondition.isEmpty()) {
            line += u" hit(%1)"_s.arg(bp.hitCondition);
        }
        if (!bp.logMessage.isEmpty()) {
            line += u" log(%1)"_s.arg(bp.logMessage);
        }
        lines.append(line);
    }
    result.ok = true;
    result.output = clip(lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::debugSelectFrame(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"debug_select_frame"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }
    if (!args.contains(u"frame_level"_s)) {
        result.ok = false;
        result.output = u"frame_level is required."_s;
        return result;
    }
    const int level = args.value(u"frame_level"_s).toInt(-1);
    const DebugActionResult action = m_debug->selectFrame(level);
    result.ok = action.ok;
    result.output = clip(action.output);
    return result;
}

ToolResult ToolRunner::debugSelectThread(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"debug_select_thread"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }
    if (!args.contains(u"thread_id"_s)) {
        result.ok = false;
        result.output = u"thread_id is required."_s;
        return result;
    }
    const int tid = args.value(u"thread_id"_s).toInt(-1);
    const DebugActionResult action = m_debug->selectThread(tid);
    result.ok = action.ok;
    result.output = clip(action.output);
    return result;
}

ToolResult ToolRunner::debugSnapshot(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"debug_snapshot"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    const QString act = args.value(u"action"_s).toString(u"capture"_s).trimmed().toLower();
    if (act == u"clear"_s) {
        m_debug->clearSnapshots();
        result.ok = true;
        result.output = u"Cleared all debugger snapshots."_s;
        return result;
    }
    if (act == u"list"_s) {
        const QList<DebugSnapshot> snaps = m_debug->snapshots();
        if (snaps.isEmpty()) {
            result.ok = true;
            result.output = u"No debugger snapshots captured yet."_s;
            return result;
        }
        QStringList lines;
        for (int i = 0; i < snaps.size(); ++i) {
            const DebugSnapshot &s = snaps.at(i);
            lines.append(u"#%1 %2 @ %3:%4 (%5 frames, %6 locals)"_s
                             .arg(i + 1)
                             .arg(s.label,
                                  s.stopInfo.file.isEmpty() ? u"<unknown>"_s : s.stopInfo.file)
                             .arg(s.stopInfo.line)
                             .arg(s.frames.size())
                             .arg(s.locals.size()));
        }
        result.ok = true;
        result.output = lines.join(u'\n');
        return result;
    }
    if (act == u"diff"_s) {
        const QList<DebugSnapshot> snaps = m_debug->snapshots();
        if (snaps.size() < 2) {
            result.ok = false;
            result.output = u"Need at least 2 snapshots to diff (currently have %1)."_s.arg(snaps.size());
            return result;
        }
        const int secondIdx = args.contains(u"second"_s) ? (args.value(u"second"_s).toInt(snaps.size()) - 1) : (snaps.size() - 1);
        const int firstIdx = args.contains(u"first"_s) ? (args.value(u"first"_s).toInt(snaps.size() - 1) - 1) : (snaps.size() - 2);
        if (firstIdx < 0 || firstIdx >= snaps.size() || secondIdx < 0 || secondIdx >= snaps.size()) {
            result.ok = false;
            result.output = u"Snapshot index out of range (1..%1)."_s.arg(snaps.size());
            return result;
        }
        result.ok = true;
        result.output = clip(DebugProvider::diffSnapshots(snaps.at(firstIdx), snaps.at(secondIdx)));
        return result;
    }

    const QString label = args.value(u"label"_s).toString().trimmed();
    const DebugSnapshot snap = m_debug->captureSnapshot(label);
    result.ok = true;
    result.output = u"Captured debugger snapshot #%1 '%2' (%3 frames, %4 locals)."_s
                        .arg(m_debug->snapshots().size())
                        .arg(snap.label)
                        .arg(snap.frames.size())
                        .arg(snap.locals.size());
    return result;
}

ToolResult ToolRunner::debugSetBreakpoint(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"debug_set_breakpoint"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    if (jsonBool(args.value(u"clear_all"_s))) {
        const DebugActionResult action = m_debug->clearAllBreakpoints();
        result.ok = action.ok;
        result.output = clip(action.output);
        return result;
    }

    const QString rawPath = args.value(u"path"_s).toString().trimmed();
    const int line = args.value(u"line"_s).toInt(0);
    if (rawPath.isEmpty() || line <= 0) {
        result.ok = false;
        result.output = u"Both path and positive 1-based line are required (unless clear_all is true)."_s;
        return result;
    }

    QString err;
    const QString resolved = m_sandbox.resolve(rawPath, &err, true);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = err;
        return result;
    }

    const DebugActionResult action = m_debug->setBreakpoint(
        resolved,
        line,
        args.value(u"condition"_s).toString(),
        args.value(u"hit_condition"_s).toString(),
        args.value(u"log_message"_s).toString(),
        jsonBool(args.value(u"remove"_s)));
    result.ok = action.ok;
    result.output = clip(action.output);
    return result;
}

ToolResult ToolRunner::debugStep(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"debug_step"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    const QString actionName = args.value(u"action"_s).toString().trimmed().toLower();
    if (actionName.isEmpty()) {
        result.ok = false;
        result.output = u"action is required (continue, step_over, step_into, step_out, run_to_cursor, pause)."_s;
        return result;
    }

    QString resolved;
    const int line = args.value(u"line"_s).toInt(0);
    if (actionName == u"run_to_cursor"_s) {
        const QString rawPath = args.value(u"path"_s).toString().trimmed();
        if (rawPath.isEmpty() || line <= 0) {
            result.ok = false;
            result.output = u"run_to_cursor requires path and positive 1-based line."_s;
            return result;
        }
        QString err;
        resolved = m_sandbox.resolve(rawPath, &err, false);
        if (resolved.isEmpty()) {
            result.ok = false;
            result.output = err;
            return result;
        }
    }

    const int timeoutMs = qBound(1000, args.value(u"timeout_ms"_s).toInt(15000), 60000);
    const DebugActionResult action = m_debug->step(actionName, resolved, line, timeoutMs);
    result.ok = action.ok;
    QString out = action.output;
    if (action.ok && m_debug->sessionState() == DebugSessionState::Paused) {
        const QString summary = m_debug->contextSummary(6, 16, 400);
        if (!summary.isEmpty()) {
            out += u"\n\n"_s + summary;
        }
    }
    result.output = clip(out);
    return result;
}

ToolResult ToolRunner::debugEval(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"debug_eval"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    const QString expr = args.value(u"expression"_s).toString().trimmed();
    if (expr.isEmpty()) {
        result.ok = false;
        result.output = u"expression must not be empty."_s;
        return result;
    }
    const int timeoutMs = qBound(500, args.value(u"timeout_ms"_s).toInt(5000), 30000);
    const DebugActionResult action = m_debug->evaluate(expr, timeoutMs);
    result.ok = action.ok;
    result.output = clip(action.output);
    return result;
}

ToolResult ToolRunner::debugSession(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"debug_session"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    const QString actionName = args.value(u"action"_s).toString().trimmed().toLower();
    if (actionName.isEmpty()) {
        result.ok = false;
        result.output = u"action is required (start, restart, stop, hot_reload, hot_restart, reload_launch_json)."_s;
        return result;
    }

    if (actionName == u"reload_launch_json"_s) {
        const bool ok = m_debug->reloadLaunchJsonTargets();
        result.ok = ok;
        result.output = ok ? u"Reloaded .vscode/launch.json targets in Kate's debugger."_s
                           : u"Could not reload .vscode/launch.json targets."_s;
        return result;
    }

    const QString target = args.value(u"target"_s).toString().trimmed();
    const int timeoutMs = qBound(1000, args.value(u"timeout_ms"_s).toInt(15000), 60000);
    const DebugActionResult action = m_debug->controlSession(actionName, target, timeoutMs);
    result.ok = action.ok;
    result.output = clip(action.output);
    return result;
}

ToolResult ToolRunner::debugConsole(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"debug_console"_s;
    if (!m_debug || !m_debug->isAvailable()) {
        result.ok = false;
        result.output = formatDebugUnavailable();
        return result;
    }

    const QString cmd = args.value(u"command"_s).toString().trimmed();
    if (cmd.isEmpty()) {
        result.ok = false;
        result.output = u"command must not be empty."_s;
        return result;
    }

    const DebugActionResult action = m_debug->sendConsoleCommand(cmd);
    result.ok = action.ok;
    result.output = clip(action.output);
    return result;
}

ToolResult ToolRunner::buildTargets(const QJsonObject &args) const
{
    Q_UNUSED(args)
    ToolResult result;
    result.name = u"build_targets"_s;

    const QString workspace = m_sandbox.workspaceRoot();
    const QList<BuildTargetInfo> targets = m_ecosystem
        ? m_ecosystem->buildTargets(workspace)
        : EcosystemProvider::discoverWorkspaceBuildTargets(workspace);
    const QString compileCommands = m_ecosystem
        ? m_ecosystem->compileCommandsPath(workspace)
        : EcosystemProvider::findCompileCommands(workspace);

    QStringList lines;
    lines.append(u"Kate Build plugin: %1"_s.arg((m_ecosystem && m_ecosystem->isBuildPluginAvailable()) ? u"available"_s : u"not loaded (filesystem discovery only)"_s));
    if (!compileCommands.isEmpty()) {
        lines.append(u"compile_commands.json: %1"_s.arg(QDir(workspace).relativeFilePath(compileCommands)));
    }
    if (targets.isEmpty()) {
        lines.append(u"No build targets discovered in %1."_s.arg(workspace));
    } else {
        lines.append(u"Targets (%1):"_s.arg(targets.size()));
        for (const BuildTargetInfo &t : targets) {
            QString line = u"  %1%2 [%3]"_s.arg(t.isDefault ? u"* "_s : u"  "_s, t.name, t.type);
            if (!t.command.isEmpty()) {
                line += u" cmd=`%1`"_s.arg(t.command);
            }
            if (!t.artifactPath.isEmpty()) {
                line += u" artifact=%1"_s.arg(QDir(workspace).relativeFilePath(t.artifactPath));
            }
            lines.append(line);
        }
    }

    result.ok = true;
    result.output = clip(lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::buildRun(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"build_run"_s;
    if (!m_ecosystem || !m_ecosystem->isBuildPluginAvailable()) {
        result.ok = false;
        result.output = u"Kate's Build plugin (katebuildplugin) is not loaded. Use bash to run your build command instead."_s;
        return result;
    }

    const QString target = args.value(u"target"_s).toString().trimmed();
    const EcosystemActionResult action = m_ecosystem->triggerBuild(target);
    result.ok = action.ok;
    result.output = clip(action.output);
    return result;
}

ToolResult ToolRunner::terminalRun(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"terminal_run"_s;
    if (!m_ecosystem || !m_ecosystem->isTerminalAvailable()) {
        result.ok = false;
        result.output = u"Kate's Konsole plugin (katekonsoleplugin) is not loaded. Use bash instead."_s;
        return result;
    }

    const QString command = args.value(u"command"_s).toString().trimmed();
    if (command.isEmpty()) {
        result.ok = false;
        result.output = u"command must not be empty."_s;
        return result;
    }
    if (m_sandbox.isAlwaysDeniedCommand(command) || m_sandbox.commandTouchesDeniedPath(command)) {
        result.ok = false;
        result.output = u"Command blocked by sandbox policy."_s;
        return result;
    }

    QString resolvedDir;
    const QString rawDir = args.value(u"working_dir"_s).toString().trimmed();
    if (!rawDir.isEmpty()) {
        QString err;
        resolvedDir = m_sandbox.resolve(rawDir, &err, false);
        if (resolvedDir.isEmpty()) {
            result.ok = false;
            result.output = err;
            return result;
        }
    }

    const EcosystemActionResult action = m_ecosystem->runInTerminal(command, resolvedDir);
    result.ok = action.ok;
    result.output = clip(action.output);
    return result;
}

} // namespace KateAi
