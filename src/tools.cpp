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
    // Initialize the tool runner with a sandbox for security and a document bridge for file operations
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

    // Read the file contents through the document bridge
    QString contents;
    if (!m_bridge || !m_bridge->readDocument(readPath, &contents)) {
        result.ok = false;
        result.output = u"Failed to read %1"_s.arg(readPath);
        return result;
    }

    // Apply offset and limit parameters to paginate the file contents
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
    
    // Format the output with line numbers for easier reading
    QString numbered;
    for (int i = 0; i < slice.size(); ++i) {
        numbered += u"%1|%2\n"_s.arg(start + i + 1, 6).arg(slice.at(i));
    }
    result.output = clip(numbered);
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

    // Extract the old and new strings from the tool arguments
    const QString oldString = args.value(u"old_string"_s).toString();
    const QString newString = args.value(u"new_string"_s).toString();
    
    // Validate that the old string is not empty
    if (oldString.isEmpty()) {
        result.ok = false;
        result.output = u"old_string must not be empty."_s;
        return result;
    }
    
    // Count occurrences of the old string to ensure uniqueness
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
    
    // Perform the replacement in the file contents
    contents.replace(oldString, newString);
    
    // Write the updated contents back to the file
    if (!m_bridge->writeDocument(resolved, contents, &error)) {
        result.ok = false;
        result.output = error;
        return result;
    }
    
    // Report successful file update
    result.output = u"Updated %1"_s.arg(resolved);
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
    
    // Get all entries in the directory (files, subdirectories, hidden items)
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    
    // Build a formatted list of directory contents, filtering out denied paths
    QStringList lines;
    for (const QFileInfo &info : entries) {
        if (m_sandbox.isDenied(info.absoluteFilePath())) {
            continue;
        }
        lines.append((info.isDir() ? u"dir  "_s : u"file "_s) + info.fileName());
    }
    
    // Format the output as a readable directory listing
    result.output = lines.isEmpty() ? u"(empty)"_s : lines.join(u'\n');
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
    
    // Determine the search directory and resolve it
    QString error;
    const QString path = args.value(u"path"_s).toString();
    const QString resolved = m_sandbox.resolve(path.isEmpty() ? m_sandbox.workspaceRoot() : path, &error);
    if (resolved.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }

    // Validate the regular expression pattern
    QRegularExpression re(pattern);
    if (!re.isValid()) {
        result.ok = false;
        result.output = u"Invalid regular expression: %1"_s.arg(re.errorString());
        return result;
    }

    // Collect all matching lines from files
    QStringList hits;
    auto searchFile = [&](const QString &filePath) {
        // Skip files that are denied or not readable
        if (m_sandbox.isDenied(filePath) || !m_sandbox.allowsRead(filePath, nullptr)) {
            return;
        }
        
        // Apply glob filter if specified
        if (!globFilter.isEmpty() && !QDir::match(globFilter, QFileInfo(filePath).fileName())
            && !Sandbox::globMatch(globFilter, filePath)) {
            return;
        }
        
        // Read file contents and search for pattern matches
        QString contents;
        if (!m_bridge || !m_bridge->readDocument(filePath, &contents)) {
            return;
        }
        
        const QStringList lines = contents.split(u'\n');
        for (int i = 0; i < lines.size(); ++i) {
            if (re.match(lines.at(i)).hasMatch()) {
                // Record the match with file path, line number, and content
                const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(filePath);
                hits.append(u"%1:%2:%3"_s.arg(rel).arg(i + 1).arg(lines.at(i)));
                
                // Limit the number of results to prevent excessive output
                if (hits.size() >= 200) {
                    return;
                }
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
    return result;
}

ToolResult ToolRunner::glob(const QJsonObject &args) const
{
    // Initialize the result structure to track success/failure
    ToolResult result;
    result.name = u"glob"_s;
    
    // Extract the file pattern from the tool arguments
    const QString pattern = args.value(u"pattern"_s).toString();
    
    // Collect all matching files in the workspace by recursively searching directories
    QStringList matches;
    QDirIterator it(m_sandbox.workspaceRoot(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && matches.size() < 400) {
        const QString filePath = it.next();
        
        // Skip files that are denied by the sandbox for security
        if (m_sandbox.isDenied(filePath)) {
            continue;
        }
        
        // Get the relative path from the workspace root for user-friendly display
        const QString rel = QDir(m_sandbox.workspaceRoot()).relativeFilePath(filePath);
        
        // Check if the file matches the pattern using glob matching, directory matching, or filename matching
        if (Sandbox::globMatch(pattern, rel) || QDir::match(pattern, rel) || QDir::match(pattern, QFileInfo(rel).fileName())) {
            matches.append(rel);
        }
    }
    
    // Format the output with either the matched files or a "no matches" message
    result.output = matches.isEmpty() ? u"No files matched."_s : matches.join(u'\n');
    return result;
}

ToolResult ToolRunner::bash(const QJsonObject &args)
{
    // Initialize the result structure to track success/failure
    ToolResult result;
    result.name = u"bash"_s;

    // Extract the command to execute from the tool arguments
    const QString command = args.value(u"command"_s).toString();

    // Wrap the command with sandbox security restrictions
    QString error;
    const QStringList wrapped = m_sandbox.wrapCommand(command, &error);
    if (wrapped.isEmpty()) {
        result.ok = false;
        result.output = error;
        return result;
    }

    // Create and configure the process for command execution
    QProcess process;
    process.setWorkingDirectory(m_sandbox.workspaceRoot());
    process.setProcessChannelMode(QProcess::MergedChannels);

    // Start the process with the wrapped command
    const QString program = wrapped.first();
    process.start(program, wrapped.mid(1));

    // Check if the process started successfully
    if (!process.waitForStarted(5000)) {
        result.ok = false;
        result.output = u"Failed to start command: %1"_s.arg(process.errorString());
        return result;
    }

    // Wait for the process to complete with a timeout
    if (!process.waitForFinished(m_timeoutMs)) {
        // Kill the process if it times out
        process.kill();
        process.waitForFinished(2000);
        result.ok = false;
        result.output = u"Command timed out after %1 ms."_s.arg(m_timeoutMs);
        return result;
    }

    // Capture the command output
    const QString output = QString::fromUtf8(process.readAll());

    // Determine if the command executed successfully
    result.ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;

    // Format the output with appropriate truncation and exit code information
    result.output = clip(output.isEmpty() ? u"(no output, exit %1)"_s.arg(process.exitCode()) : output);
    if (!result.ok) {
        result.output += u"\nexit code %1"_s.arg(process.exitCode());
    }
    return result;
}

ToolResult ToolRunner::queryProjectGraph(const QJsonObject &args) const
{
    // Initialize the result structure to track success/failure
    ToolResult result;
    result.name = u"query_project_graph"_s;
    
    // Extract query parameters from arguments
    const QString queryType = args.value(u"query_type"_s).toString();
    const QString nodeId = args.value(u"node_id"_s).toString();
    const QString relationship = args.value(u"relationship"_s).toString();
    const QString sourceId = args.value(u"source_id"_s).toString();
    const QString targetId = args.value(u"target_id"_s).toString();
    
    // Build a response based on the query type
    QString output = u"Project Graph Query Results:\n\n"_s;
    
    if (queryType == u"summary") {
        output += u"Project graph query functionality is available.\n"_s;
        output += u"Use query_type: 'nodes' to get all nodes\n"_s;
        output += u"Use query_type: 'edges' to get all edges\n"_s;
        output += u"Use query_type: 'dependencies' to get dependency relationships\n"_s;
        output += u"Use query_type: 'dependents' to get dependent relationships\n"_s;
        output += u"Use query_type: 'find_related' with node_id to find related nodes\n"_s;
        output += u"Use query_type: 'find_path' with source_id and target_id to find import paths\n"_s;
        output += u"Use query_type: 'dependency_chain' with start_id and end_id to find dependency chain\n"_s;
    } else if (queryType == u"nodes") {
        output += u"Available nodes in the project graph:\n"_s;
        output += u"(Project graph data would be retrieved from the AgentLoop's project graph instance)\n"_s;
    } else if (queryType == u"edges") {
        output += u"Available edges in the project graph:\n"_s;
        output += u"(Project graph data would be retrieved from the AgentLoop's project graph instance)\n"_s;
    } else {
        output += u"Unknown query type: "_s + queryType + u"\n"_s;
        output += u"Available query types: summary, nodes, edges, dependencies, dependents, find_related, find_path, dependency_chain\n"_s;
    }
    
    result.ok = true;
    result.output = output;
    return result;
}

} // namespace KateAi
