/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "tools.h"

#include "mcp.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QElapsedTimer>
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
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = u"Invalid tool arguments: %1"_s.arg(parseError.errorString());
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
    if (toolName == u"read_file"_s || toolName == u"fs/read_text_file"_s) {
        const QString path = quoteShellArg(args.value(u"path"_s).toString());
        const int offset = args.value(u"offset"_s).toInt(args.value(u"line"_s).toInt(1));
        const int limit = args.value(u"limit"_s).toInt(args.value(u"limit"_s).toInt(0));
        if (limit > 0) {
            const int end = offset + limit - 1;
            return u"sed -n '%1,%2p' %3"_s.arg(offset).arg(end).arg(path);
        }
        if (offset > 1) {
            return u"tail -n +%1 %2"_s.arg(offset).arg(path);
        }
        return u"cat %1"_s.arg(path);
    }
    if (toolName == u"write_file"_s || toolName == u"fs/write_text_file"_s) {
        return u"cat > %1"_s.arg(quoteShellArg(args.value(u"path"_s).toString()));
    }
    if (toolName == u"edit_file"_s) {
        return u"patch %1"_s.arg(quoteShellArg(args.value(u"path"_s).toString()));
    }
    if (toolName == u"multi_edit_file"_s || toolName == u"multi_replace_file_content"_s) {
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
        return u"patch %1 (%2 edits)"_s.arg(path).arg(edits.size());
    }
    if (toolName == u"list_dir"_s) {
        const QString path = args.value(u"path"_s).toString();
        return path.isEmpty() ? u"ls"_s : u"ls %1"_s.arg(quoteShellArg(path));
    }
    if (toolName == u"grep"_s) {
        QString cmd = u"grep -n"_s;
        if (jsonBool(args.value(u"case_insensitive"_s))) {
            cmd += u" -i"_s;
        }
        const int context = args.value(u"context"_s).toInt(0);
        if (context > 0) {
            cmd += u" -C %1"_s.arg(context);
        }
        cmd += u" %1"_s.arg(quoteShellArg(args.value(u"pattern"_s).toString()));
        const QString path = args.value(u"path"_s).toString();
        if (!path.isEmpty()) {
            cmd += u" %1"_s.arg(quoteShellArg(path));
        }
        const QString glob = args.value(u"glob"_s).toString();
        if (!glob.isEmpty()) {
            cmd += u" --include=%1"_s.arg(quoteShellArg(glob));
        }
        return cmd;
    }
    if (toolName == u"glob"_s) {
        const QString pattern = args.value(u"pattern"_s).toString();
        return u"find . -name %1"_s.arg(quoteShellArg(pattern));
    }
    if (toolName == u"bash"_s) {
        return args.value(u"command"_s).toString();
    }
    if (toolName == u"query_project_graph"_s) {
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
    if (toolName == u"new_task"_s) {
        const QString mode = args.value(u"mode"_s).toString();
        const QString description = args.value(u"description"_s).toString();
        const QString label = mode.isEmpty() ? QStringLiteral("code") : mode;
        // A delegation without a description still reads as a delegation, not
        // as a bare bracket pair with nothing behind it.
        return description.isEmpty() ? u"[%1]"_s.arg(label) : u"[%1] %2"_s.arg(label, description);
    }
    if (toolName == u"write_plan_file"_s) {
        return u"write plan file"_s;
    }
    if (toolName == u"suggest"_s) {
        // The first line of the advice reads better in a card header than a
        // bare tool name, so show that, capped.
        const QString firstLine = args.value(u"suggestion"_s).toString().section(u'\n', 0, 0).trimmed();
        return firstLine.isEmpty() ? u"suggest"_s : u"suggest %1"_s.arg(quoteShellArg(firstLine.left(80)));
    }
    if (toolName == u"question"_s) {
        // The question is the whole point of the call, so it goes in the
        // header verbatim (capped) rather than being summarised as "question".
        const QString asked = args.value(u"question"_s).toString().section(u'\n', 0, 0).trimmed();
        return asked.isEmpty() ? u"question"_s : u"question %1"_s.arg(quoteShellArg(asked.left(120)));
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
    const QByteArray digest = QCryptographicHash::hash(workspaceRoot.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QDir::tempPath() + u"/kateai-plan-"_s + QString::fromUtf8(digest.left(12)) + u".md"_s;
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

QString unifiedDiff(const QString &path, const QString &before, const QString &after)
{
    QStringList oldLines = before.split(u'\n');
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
        if (!resolved.isEmpty() && m_bridge) {
            m_bridge->readDocument(resolved, &existing);
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
            const QString oldStr = chunk.value(u"old_string"_s).toString().isEmpty()
                ? (chunk.value(u"TargetContent"_s).toString().isEmpty() ? chunk.value(u"targetContent"_s).toString() : chunk.value(u"TargetContent"_s).toString())
                : chunk.value(u"old_string"_s).toString();
            const QString newStr = chunk.value(u"new_string"_s).toString().isEmpty()
                ? (chunk.value(u"ReplacementContent"_s).toString().isEmpty() ? chunk.value(u"replacementContent"_s).toString() : chunk.value(u"ReplacementContent"_s).toString())
                : chunk.value(u"new_string"_s).toString();
            if (!oldStr.isEmpty()) {
                simulated.replace(oldStr, newStr);
            }
        }
        req.describeDiff = unifiedDiff(req.path, existing, simulated);
        req.details = req.describeDiff;
    } else if (call.name == u"bash"_s) {
        req.risk = ToolRisk::Execute;
        req.details = args.value(u"command"_s).toString();
    } else if (call.name == u"write_plan_file"_s) {
        // Read risk on purpose: the plan file is a Kate-chosen temp file, not a
        // workspace mutation, so it never prompts and never enters the tracker.
        req.risk = ToolRisk::Read;
        req.details = args.value(u"content"_s).toString();
    } else if (call.name == u"question"_s) {
        // Read risk for the same reason: asking the user something neither
        // reads the workspace nor changes it.
        req.risk = ToolRisk::Read;
        req.questionText = args.value(u"question"_s).toString();
        for (const QJsonValue &value : args.value(u"options"_s).toArray()) {
            const QString option = value.toString().trimmed();
            if (!option.isEmpty()) {
                req.questionOptions.append(option);
            }
        }
        req.allowFreeform = jsonBool(args.value(u"allow_freeform"_s), true);
        req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    } else {
        // Unknown tools reach the permission card from here. A delegation or
        // an MCP call can run code, so they must not be presented with the
        // read-risk styling that auto-approval affordances key off.
        req.risk = (call.name == u"new_task"_s || isMcpToolName(call.name)) ? ToolRisk::Execute : ToolRisk::Read;
        req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    }
    return req;
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
    if (call.name == u"goal_report"_s) {
        return goalReport(args);
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

    result.ok = false;
    result.output = u"Unknown tool: %1"_s.arg(call.name);
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

    // Apply offset and limit parameters to paginate the file contents
    const int offset = jsonInt(args.value(u"offset"_s), 1);
    const int limit = jsonInt(args.value(u"limit"_s), 0);
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
    
    // Get the content to write from the tool arguments
    const QString content = args.value(u"content"_s).toString();
    
    // Write the content to the file through the document bridge
    if (!m_bridge || !m_bridge->writeDocument(resolved, content, &error)) {
        result.ok = false;
        result.output = error.isEmpty() ? u"Write failed."_s : error;
        return result;
    }
    
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

    if (oldString.isEmpty()) {
        result.ok = false;
        result.output = u"old_string must not be empty."_s;
        return result;
    }

    const int count = contents.count(oldString);
    if (count == 0) {
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
        result.output = u"old_string was not found in %1. Read the file and copy the exact text, including whitespace."_s.arg(resolved) + hint;
        return result;
    }
    if (count > 1 && !replaceAll) {
        result.ok = false;
        result.output = u"old_string matched %1 times; it must be unique, or set replace_all=true."_s.arg(count);
        return result;
    }

    contents.replace(oldString, newString);

    if (!m_bridge->writeDocument(resolved, contents, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }

    result.output = replaceAll && count > 1
        ? u"Updated %1 (%2 replacements)"_s.arg(resolved).arg(count)
        : u"Updated %1"_s.arg(resolved);
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

    if (!m_bridge->writeDocument(resolved, workingContents, &error)) {
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
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (m_sandbox.isDenied(info.absoluteFilePath())) {
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
    const int context = qBound(0, args.value(u"context"_s).toInt(0), 5);

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
        if (contents.left(8192).contains(u'\0')) {
            ++skippedFiles;
            return;
        }

        const QStringList lines = contents.split(u'\n');
        int lastMatchEnd = -1; // Track end of last context block to avoid duplicate separators
        for (int i = 0; i < lines.size(); ++i) {
            if (!re.match(lines.at(i)).hasMatch()) {
                continue;
            }
            if (context == 0) {
                hits.append(u"%1:%2:%3"_s.arg(rel).arg(i + 1).arg(lines.at(i)));
            } else {
                const int from = qMax(0, i - context);
                const int to = qMin(lines.size() - 1, i + context);
                // Only add separator if this match group doesn't overlap with the previous one
                if (lastMatchEnd >= 0 && from > lastMatchEnd) {
                    hits.append(u"--"_s);
                }
                for (int j = from; j <= to; ++j) {
                    const QChar mark = (j == i) ? u':' : u'-';
                    hits.append(u"%1%2%3%4%5"_s.arg(rel).arg(mark).arg(j + 1).arg(mark).arg(lines.at(j)));
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
            searchFile(it.next());
        }
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
    QDirIterator it(base, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (matches.size() >= 400) {
            capped = true;
            break;
        }
        const QString filePath = it.next();

        // Skip files that are denied by the sandbox for security
        if (m_sandbox.isDenied(filePath)) {
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
    QByteArray output;
    QElapsedTimer elapsed;
    elapsed.start();
    bool timedOut = false;
    while (true) {
        output += process.readAll();
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
#ifdef Q_OS_UNIX
    // Same session trick as the synchronous path: a timeout must be able to
    // reap the command and anything it started, not just the shell.
    process->setChildProcessModifier([]() {
        ::setsid();
    });
#endif
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
    connect(timeout, &QTimer::timeout, this, [this, callId, timeout, drain] {
        timeout->deleteLater();
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
        GraphNode *node = resolveGraphNode(m_projectGraph, nodeId);
        if (!node) {
            result.ok = false;
            result.output = u"Unknown node: %1"_s.arg(nodeId);
            return result;
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
        result.ok = false;
        result.output = u"Unknown query_type. Use summary, nodes, edges, dependencies, dependents, find_related, find_path, or dependency_chain."_s;
        return result;
    }

    result.ok = true;
    result.output = clip(lines.isEmpty() ? u"(no results)"_s : lines.join(u'\n'));
    return result;
}

ToolResult ToolRunner::goalReport(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"goal_report"_s;

    const QString status = args.value(u"status"_s).toString();
    const QString reason = args.value(u"reason"_s).toString();

    if (status.isEmpty()) {
        result.ok = false;
        result.output = u"Goal report requires a status (complete, failed, or blocked)."_s;
        return result;
    }

    if (status != u"complete"_s && status != u"failed"_s && status != u"blocked"_s) {
        result.ok = false;
        result.output = u"Invalid status: %1. Must be 'complete', 'failed', or 'blocked'."_s.arg(status);
        return result;
    }

    if (reason.isEmpty()) {
        result.ok = false;
        result.output = u"Goal report requires a reason explaining the outcome."_s;
        return result;
    }

    result.ok = true;
    result.output = u"Goal reported as %1: %2"_s.arg(status, reason);
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

    const QString text = args.value(u"suggestion"_s).toString().trimmed();
    if (text.isEmpty()) {
        result.ok = false;
        result.output = u"suggestion must not be empty: describe what you are suggesting."_s;
        return result;
    }

    // The anchor is optional: advice without a place to look is still valid.
    // It is carried through as given rather than resolved, because a
    // suggestion may point at a file the work has not created yet.
    const QString path = args.value(u"path"_s).toString().trimmed();
    int line = 0;
    const QJsonValue lineValue = args.value(u"line"_s);
    if (lineValue.isDouble()) {
        line = lineValue.toInt();
        if (line < 1) {
            result.ok = false;
            result.output = u"line must be a 1-based line number, or leave it out."_s;
            return result;
        }
    } else if (!lineValue.isUndefined() && !lineValue.isNull()) {
        result.ok = false;
        result.output = u"line must be a 1-based line number, or leave it out."_s;
        return result;
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

    result.ok = true;
    result.output = u"Plan written to %1 (%2 bytes). The user can open that file to follow the implementation; "
                      "later calls overwrite it, so keep it current as the work moves."_s
                      .arg(path)
                      .arg(content.toUtf8().size());
    return result;
}

} // namespace KateAi
