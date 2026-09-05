#include "tools.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

static QJsonObject parseArgs(const ToolCall &call, QString *error)
{
    if (!call.arguments.isEmpty()) {
        return call.arguments;
    }
    if (call.argumentsJson.trimmed().isEmpty()) {
        return {};
    }
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
    return text.left(maxChars) + u"\n... truncated %1 characters"_s.arg(text.size() - maxChars);
}

QString unifiedDiff(const QString &path, const QString &before, const QString &after)
{
    const QStringList oldLines = before.split(u'\n');
    const QStringList newLines = after.split(u'\n');
    QString out;
    out += u"--- a/%1\n+++ b/%1\n"_s.arg(path);
    const int max = std::max(oldLines.size(), newLines.size());
    int shown = 0;
    for (int i = 0; i < max && shown < 200; ++i) {
        const QString a = i < oldLines.size() ? oldLines.at(i) : QString();
        const QString b = i < newLines.size() ? newLines.at(i) : QString();
        if (a == b) {
            continue;
        }
        if (i < oldLines.size()) {
            out += u"-%1\n"_s.arg(a);
        }
        if (i < newLines.size()) {
            out += u"+%1\n"_s.arg(b);
        }
        ++shown;
    }
    if (shown == 0) {
        out += u"(no line-level changes)\n"_s;
    }
    return out;
}

ToolRunner::ToolRunner(Sandbox sandbox, DocumentBridge *bridge, QObject *parent)
    : QObject(parent)
    , m_sandbox(std::move(sandbox))
    , m_bridge(bridge)
{
}

PermissionRequest ToolRunner::describe(const ToolCall &call) const
{
    QString parseError;
    const QJsonObject args = parseArgs(call, &parseError);
    PermissionRequest req;
    req.toolName = call.name;
    req.toolCallId = call.id;
    req.path = args.value(u"path"_s).toString();

    if (call.name == u"write_file"_s) {
        req.risk = ToolRisk::Write;
        req.summary = u"Write %1"_s.arg(req.path);
        QString existing;
        QString error;
        const QString resolved = m_sandbox.resolve(req.path, &error, true);
        if (!resolved.isEmpty() && m_bridge) {
            m_bridge->readDocument(resolved, &existing);
        }
        req.details = unifiedDiff(req.path, existing, args.value(u"content"_s).toString());
    } else if (call.name == u"edit_file"_s) {
        req.risk = ToolRisk::Write;
        req.summary = u"Edit %1"_s.arg(req.path);
        req.details = u"Replace:\n%1\n\nWith:\n%2"_s.arg(args.value(u"old_string"_s).toString(),
                                                         args.value(u"new_string"_s).toString());
    } else if (call.name == u"bash"_s) {
        req.risk = ToolRisk::Execute;
        const QString command = args.value(u"command"_s).toString();
        req.summary = u"Run command"_s;
        req.details = command;
    } else {
        req.risk = ToolRisk::Read;
        req.summary = call.name;
        req.details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    }
    return req;
}

ToolResult ToolRunner::run(const ToolCall &call)
{
    QString parseError;
    const QJsonObject args = parseArgs(call, &parseError);
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

    result.ok = false;
    result.output = u"Unknown tool: %1"_s.arg(call.name);
    return result;
}

ToolResult ToolRunner::readFile(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"read_file"_s;
    QString error;
    const QString path = args.value(u"path"_s).toString();
    if (!m_sandbox.allowsRead(path, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    const QString resolved = m_sandbox.resolve(path, &error);
    const QString readPath = resolved.isEmpty() ? QDir(m_sandbox.workspaceRoot()).absoluteFilePath(path) : resolved;
    if (resolved.isEmpty() && m_sandbox.profile() == SandboxProfile::Strict) {
        result.ok = false;
        result.output = error;
        return result;
    }

    QString contents;
    if (!m_bridge || !m_bridge->readDocument(readPath, &contents)) {
        result.ok = false;
        result.output = u"Failed to read %1"_s.arg(readPath);
        return result;
    }

    const int offset = args.value(u"offset"_s).toInt(1);
    const int limit = args.value(u"limit"_s).toInt(0);
    QStringList lines = contents.split(u'\n');
    int start = std::max(0, offset - 1);
    if (start >= lines.size()) {
        result.output = u"File has %1 lines; offset is past the end."_s.arg(lines.size());
        return result;
    }
    int count = limit > 0 ? limit : lines.size() - start;
    QStringList slice = lines.mid(start, count);
    QString numbered;
    for (int i = 0; i < slice.size(); ++i) {
        numbered += u"%1|%2\n"_s.arg(start + i + 1, 6).arg(slice.at(i));
    }
    result.output = clip(numbered);
    return result;
}

ToolResult ToolRunner::writeFile(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"write_file"_s;
    QString error;
    const QString path = args.value(u"path"_s).toString();
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
    const QString content = args.value(u"content"_s).toString();
    if (!m_bridge || !m_bridge->writeDocument(resolved, content, &error)) {
        result.ok = false;
        result.output = error.isEmpty() ? u"Write failed."_s : error;
        return result;
    }
    result.output = u"Wrote %1 (%2 bytes)"_s.arg(resolved).arg(content.toUtf8().size());
    return result;
}

ToolResult ToolRunner::editFile(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"edit_file"_s;
    QString error;
    const QString path = args.value(u"path"_s).toString();
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

    QString contents;
    if (!m_bridge || !m_bridge->readDocument(resolved, &contents)) {
        result.ok = false;
        result.output = u"Failed to read %1"_s.arg(resolved);
        return result;
    }

    const QString oldString = args.value(u"old_string"_s).toString();
    const QString newString = args.value(u"new_string"_s).toString();
    if (oldString.isEmpty()) {
        result.ok = false;
        result.output = u"old_string must not be empty."_s;
        return result;
    }
    const int count = contents.count(oldString);
    if (count == 0) {
        result.ok = false;
        result.output = u"old_string was not found in %1."_s.arg(resolved);
        return result;
    }
    if (count > 1) {
        result.ok = false;
        result.output = u"old_string matched %1 times; it must be unique."_s.arg(count);
        return result;
    }
    contents.replace(oldString, newString);
    if (!m_bridge->writeDocument(resolved, contents, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    result.output = u"Updated %1"_s.arg(resolved);
    return result;
}

ToolResult ToolRunner::listDir(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"list_dir"_s;
    QString error;
    const QString path = args.value(u"path"_s).toString();
    if (!m_sandbox.allowsRead(path.isEmpty() ? m_sandbox.workspaceRoot() : path, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    const QString resolved = m_sandbox.resolve(path.isEmpty() ? m_sandbox.workspaceRoot() : path, &error);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }
    QDir dir(resolved);
    if (!dir.exists()) {
        result.ok = false;
        result.output = u"Directory does not exist: %1"_s.arg(resolved);
        return result;
    }
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    QStringList lines;
    for (const QFileInfo &info : entries) {
        if (m_sandbox.isDenied(info.absoluteFilePath())) {
            continue;
        }
        lines.append((info.isDir() ? u"dir  "_s : u"file "_s) + info.fileName());
    }
    result.output = lines.isEmpty() ? u"(empty)"_s : lines.join(u'\n');
    return result;
}

ToolResult ToolRunner::grep(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"grep"_s;
    const QString pattern = args.value(u"pattern"_s).toString();
    const QString globFilter = args.value(u"glob"_s).toString();
    QString error;
    const QString path = args.value(u"path"_s).toString();
    const QString resolved = m_sandbox.resolve(path.isEmpty() ? m_sandbox.workspaceRoot() : path, &error);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }

    QRegularExpression re(pattern);
    if (!re.isValid()) {
        result.ok = false;
        result.output = u"Invalid regular expression: %1"_s.arg(re.errorString());
        return result;
    }

    QStringList hits;
    auto searchFile = [&](const QString &filePath) {
        if (m_sandbox.isDenied(filePath) || !m_sandbox.allowsRead(filePath, nullptr)) {
            return;
        }
        if (!globFilter.isEmpty() && !QDir::match(globFilter, QFileInfo(filePath).fileName())
            && !Sandbox::globMatch(globFilter, filePath)) {
            return;
        }
        QString contents;
        if (!m_bridge || !m_bridge->readDocument(filePath, &contents)) {
            return;
        }
        const QStringList lines = contents.split(u'\n');
        for (int i = 0; i < lines.size(); ++i) {
            if (re.match(lines.at(i)).hasMatch()) {
                const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(filePath);
                hits.append(u"%1:%2:%3"_s.arg(rel).arg(i + 1).arg(lines.at(i)));
                if (hits.size() >= 200) {
                    return;
                }
            }
        }
    };

    const QFileInfo info(resolved);
    if (info.isFile()) {
        searchFile(resolved);
    } else {
        QDirIterator it(resolved, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext() && hits.size() < 200) {
            searchFile(it.next());
        }
    }

    result.output = hits.isEmpty() ? u"No matches."_s : clip(hits.join(u'\n'));
    return result;
}

ToolResult ToolRunner::glob(const QJsonObject &args) const
{
    ToolResult result;
    result.name = u"glob"_s;
    const QString pattern = args.value(u"pattern"_s).toString();
    QStringList matches;
    QDirIterator it(m_sandbox.workspaceRoot(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && matches.size() < 400) {
        const QString filePath = it.next();
        if (m_sandbox.isDenied(filePath)) {
            continue;
        }
        const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(filePath);
        if (Sandbox::globMatch(pattern, rel) || QDir::match(pattern, rel) || QDir::match(pattern, QFileInfo(rel).fileName())) {
            matches.append(rel);
        }
    }
    result.output = matches.isEmpty() ? u"No files matched."_s : matches.join(u'\n');
    return result;
}

ToolResult ToolRunner::bash(const QJsonObject &args)
{
    ToolResult result;
    result.name = u"bash"_s;
    const QString command = args.value(u"command"_s).toString();
    QString error;
    const QStringList wrapped = m_sandbox.wrapCommand(command, &error);
    if (wrapped.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }

    QProcess process;
    process.setWorkingDirectory(m_sandbox.workspaceRoot());
    process.setProcessChannelMode(QProcess::MergedChannels);
    const QString program = wrapped.first();
    process.start(program, wrapped.mid(1));
    if (!process.waitForStarted(5000)) {
        result.ok = false;
        result.output = u"Failed to start command: %1"_s.arg(process.errorString());
        return result;
    }
    if (!process.waitForFinished(m_timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        result.ok = false;
        result.output = u"Command timed out after %1 ms."_s.arg(m_timeoutMs);
        return result;
    }
    const QString output = QString::fromUtf8(process.readAll());
    result.ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    result.output = clip(output.isEmpty() ? u"(no output, exit %1)"_s.arg(process.exitCode()) : output);
    if (!result.ok) {
        result.output += u"\nexit code %1"_s.arg(process.exitCode());
    }
    return result;
}

} // namespace KateAi
