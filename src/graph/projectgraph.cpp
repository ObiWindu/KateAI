/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "projectgraph.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStack>
#include <QQueue>
#include <QVector>
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// Bumped when dependency/edge semantics change (version 2 resolves imports to
// file-node ids instead of storing raw import lines). An older cache must be
// rebuilt rather than trusted with an incompatible graph.
constexpr int kGraphSchemaVersion = 2;

// How much of a file is read for the analysis. A file is read whole so its
// line count, its definitions and its imports are the file's own and not the
// first page of it; the cap keeps one oversized file from taking the memory
// down with it.
constexpr qint64 kMaxAnalysedBytes = 1024 * 1024;

// What the graph keeps of each file once the analysis has run. The content is
// analysis input, not graph data: holding every file in memory would make the
// graph grow with the workspace.
constexpr int kContentPreviewChars = 16 * 1024;

// Node ids hash the path as it was scanned. On macOS a temporary directory is
// commonly /var/... while QFileInfo resolves the same file to /private/var/...;
// tools resolve paths before querying, so lookups need a canonical fallback.
QString canonicalLookupPath(const QString &path, const QString &workspace)
{
    QString absolute = path;
    if (absolute.isEmpty()) {
        return {};
    }
    if (QDir::isRelativePath(absolute)) {
        absolute = workspace.isEmpty() ? QDir::current().absoluteFilePath(absolute)
                                       : QDir(workspace).absoluteFilePath(absolute);
    }
    const QString canonical = QFileInfo(absolute).canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? absolute : canonical);
}

} // namespace

ProjectGraph::ProjectGraph()
    : m_lastUpdateTime(0)
    , m_isDirty(false)
{
}

ProjectGraph::~ProjectGraph()
{
    qDeleteAll(m_nodes);
    qDeleteAll(m_edges);
}

void ProjectGraph::generateGraph(const QString &workspacePath)
{
    setWorkspacePath(workspacePath);
    clear();
    // A fresh walk: a directory visited by an earlier scan no longer counts.
    m_visitedDirs.clear();

    // Add workspace root node
    GraphNode rootNode;
    rootNode.id = generateNodeId(workspacePath, u"directory"_s);
    rootNode.name = QDir(workspacePath).dirName();
    rootNode.type = u"directory"_s;
    rootNode.path = workspacePath;
    rootNode.isReadOnly = false;
    rootNode.lastModified = QFileInfo(workspacePath).lastModified().toSecsSinceEpoch();

    addNode(rootNode);

    // Recursively scan and add all files and directories
    scanDirectory(workspacePath, rootNode.id);

    // Analyze dependencies and relationships
    analyzeFileDependencies();
    analyzeCodeStructure();
    updateSecurityStatus();
    // The analysis is done with the full text of every file; what stays on the
    // nodes is a preview.
    trimContentPreviews();

    m_lastUpdateTime = QDateTime::currentSecsSinceEpoch();
    m_isDirty = false;
}

void ProjectGraph::trimContentPreviews()
{
    // The graph holds a bounded preview of each file rather than its text.
    // The analysis has already run over the full text by now, so the rest is
    // only kept to make a later incremental update cheap.
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        GraphNode *node = it.value();
        if (node && node->content.size() > kContentPreviewChars) {
            node->content = node->content.left(kContentPreviewChars);
        }
    }
}

void ProjectGraph::scanDirectory(const QString &dirPath, const QString &parentNodeId)
{
    // A graph of the first few hundred files is enough for orientation; the
    // tool can inspect the rest on demand. Raise the ceiling for normal
    // projects while keeping a first index bounded and responsive.
    constexpr int kMaxGraphNodes = 1000;
    if (m_nodes.size() >= kMaxGraphNodes) {
        return;
    }

    // A symlinked directory that points back into (or above) the workspace would
    // otherwise be re-scanned once per link level, adding a fresh duplicate node
    // id for every file on every pass and reading each file again and again.
    // Follow nothing, and never scan a directory that resolves outside the
    // workspace root.
    const QString resolvedDir = QFileInfo(dirPath).canonicalFilePath();
    const QString resolvedWorkspace = m_workspacePath.isEmpty()
        ? QString()
        : QFileInfo(m_workspacePath).canonicalFilePath();
    if (!resolvedWorkspace.isEmpty() && resolvedWorkspace != resolvedDir
        && !resolvedDir.startsWith(resolvedWorkspace + u'/')) {
        return;
    }
    if (m_visitedDirs.contains(resolvedDir)) {
        return;
    }
    m_visitedDirs.insert(resolvedDir);

    QDir dir(dirPath);
    // NoSymLinks keeps a link from being treated as a directory to descend into.
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks);

    for (const QFileInfo &entry : entries) {
        if (m_nodes.size() >= kMaxGraphNodes) {
            break;
        }

        const QString fileName = entry.fileName();

        if (entry.isDir()) {
            // Ignore hidden and heavy build/dependency directories
            if (fileName.startsWith(u'.') ||
                fileName == u"build"_s ||
                fileName == u"node_modules"_s ||
                fileName == u"dist"_s ||
                fileName == u"target"_s ||
                fileName == u"venv"_s ||
                fileName == u"__pycache__"_s) {
                continue;
            }

            GraphNode node;
            node.id = generateNodeId(entry.absoluteFilePath(), u"directory"_s);
            node.name = fileName;
            node.type = u"directory"_s;
            node.path = entry.absoluteFilePath();
            node.isReadOnly = false;
            node.lastModified = entry.lastModified().toSecsSinceEpoch();
            addNode(node);

            GraphEdge edge;
            edge.sourceId = parentNodeId;
            edge.targetId = node.id;
            edge.relationship = u"contains"_s;
            edge.isSafe = true;
            addEdge(edge);

            // Recursively scan subdirectories
            scanDirectory(entry.absoluteFilePath(), node.id);
        } else {
            // Skip binaries. The extension list used to be open-coded here and
            // missed text-named files with binary content; isBinaryFile() checks
            // the same extensions first and then sniffs the head of the file.
            if (isBinaryFile(entry.absoluteFilePath())) {
                continue;
            }

            GraphNode node;
            node.id = generateNodeId(entry.absoluteFilePath(), u"file"_s);
            node.name = fileName;
            // File nodes must report "file": the analysers, the directory listing
            // and the graph tool all filter on it, and this used to hold the
            // language, which hid every file from them. The language is kept in
            // metadata instead.
            node.type = u"file"_s;
            node.metadata.insert(u"language"_s, getNodeTypeFromPath(entry.absoluteFilePath()));
            node.path = entry.absoluteFilePath();
            node.isReadOnly = false;
            node.lastModified = entry.lastModified().toSecsSinceEpoch();

            // Read the file for the analysis
            QFile file(entry.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                // The whole file is read, not a prefix of it. A file too large
                // to analyse used to be missing from the graph altogether, and
                // a file analysed from a prefix alone is indexed with the wrong
                // line count and with most of its definitions missing. The read
                // is capped so one huge file cannot blow the memory up, and
                // what the graph keeps afterwards is trimmed back to a preview.
                //
                // Both metrics used to be left indeterminate here and
                // serialised as garbage; they are derived from what was
                // actually read, with the same helpers the incremental update
                // path uses.
                node.content = QString::fromUtf8(file.read(kMaxAnalysedBytes));
                node.linesOfCode = lineCount(node.content);
                node.complexity = definitionCount(*this, node.content);
                indexFileSymbols(&node);
            }
            addNode(node);

            // Create edge from parent to child
            GraphEdge edge;
            edge.sourceId = parentNodeId;
            edge.targetId = node.id;
            edge.relationship = u"contains"_s;
            edge.isSafe = true;
            addEdge(edge);
        }
    }
}

void ProjectGraph::updateGraph(const QString &filePath, const QString &content)
{
    QString nodeId = generateNodeId(filePath);
    GraphNode *node = getNode(nodeId);
    if (!node) {
        const QString wanted = canonicalLookupPath(filePath, m_workspacePath);
        for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
            GraphNode *candidate = it.value();
            if (candidate && candidate->type == u"file"_s && !candidate->path.isEmpty()
                && canonicalLookupPath(candidate->path, m_workspacePath) == wanted) {
                node = candidate;
                nodeId = it.key();
                break;
            }
        }
    }

    if (node) {
        // Update node content and metrics
        node->content = content;
        updateNodeMetrics(node);
        indexFileSymbols(node);
        node->lastModified = QFileInfo(node->path).lastModified().toSecsSinceEpoch();

        // Update dependencies based on new content
        reindexFileDependencies(node);
        // The content was needed whole for the metrics, the symbols and the
        // imports; what stays on the node is a preview.
        if (node->content.size() > kContentPreviewChars) {
            node->content = node->content.left(kContentPreviewChars);
        }
    } else {
        // Create new node for file. Use an absolute path so it matches the
        // ids the scanner creates for existing files.
        const QString absolutePath = QFileInfo(filePath).absoluteFilePath();
        GraphNode newNode;
        newNode.id = generateNodeId(absolutePath);
        newNode.name = QFileInfo(absolutePath).fileName();
        // Same convention as scanDirectory(): the node type is "file" and the
        // language lives in metadata. A language here would hide the node from
        // every analyser and query that filters on type == "file".
        newNode.type = u"file"_s;
        newNode.metadata.insert(u"language"_s, getNodeTypeFromPath(absolutePath));
        newNode.path = absolutePath;
        newNode.content = content;
        newNode.isReadOnly = false;
        newNode.lastModified = QDateTime::currentSecsSinceEpoch();
        updateNodeMetrics(&newNode);
        indexFileSymbols(&newNode);
        // The content was needed whole for the metrics and the symbols; what
        // stays on the node is a preview.
        if (newNode.content.size() > kContentPreviewChars) {
            newNode.content = newNode.content.left(kContentPreviewChars);
        }

        // Find parent directory
        QString parentDirId = generateNodeId(QFileInfo(absolutePath).absolutePath(), u"directory"_s);
        GraphNode *parentNode = getNode(parentDirId);
        if (parentNode) {
            addNode(newNode);
            GraphEdge edge;
            edge.sourceId = parentDirId;
            edge.targetId = newNode.id;
            edge.relationship = u"contains"_s;
            edge.isSafe = true;
            addEdge(edge);
            // A newly created file gets import edges immediately, instead of
            // waiting for a full regeneration of the project graph.
            if (GraphNode *added = getNode(newNode.id)) {
                reindexFileDependencies(added);
            }
        }
    }

    m_lastUpdateTime = QDateTime::currentSecsSinceEpoch();
    m_isDirty = true;
}

void ProjectGraph::removeNode(const QString &nodeId)
{
    if (!m_nodes.contains(nodeId)) {
        return;
    }

    // Remove all edges connected to this node
    QSet<QString> connectedNodes;
    for (auto it = m_edges.begin(); it != m_edges.end(); ) {
        const QString &edgeKey = it.key();
        // Parse edgeKey to get sourceId, targetId, and relationship
        // Format: sourceId->targetId->relationship. targetId may itself contain
        // "->" (raw import line, e.g. "#include <foo->bar>"), so split the
        // relationship off from the right; sourceIds never contain "->".
        int arrow1 = edgeKey.indexOf(u"->"_s);
        int arrow2 = edgeKey.lastIndexOf(u"->"_s);
        if (arrow1 != -1 && arrow2 != -1) {
            QString sourceId = edgeKey.mid(0, arrow1);
            QString targetId = edgeKey.mid(arrow1 + 2, arrow2 - arrow1 - 2);
            // relationship = edgeKey.mid(arrow2 + 2); // Not needed for removal
            if (sourceId == nodeId || targetId == nodeId) {
                connectedNodes.insert(sourceId);
                connectedNodes.insert(targetId);
                delete it.value();
                it = m_edges.erase(it);
                continue;
            }
        }
        ++it;
    }

    // Update adjacency list
    for (const QString &connectedId : connectedNodes) {
        if (m_adjacencyList.contains(connectedId)) {
            m_adjacencyList[connectedId].remove(nodeId);
        }
    }

    // Remove the node itself
    delete m_nodes.take(nodeId);

    // Update dependent nodes
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        GraphNode *node = it.value();
        node->dependencies.remove(nodeId);
        node->dependents.remove(nodeId);
    }
}

void ProjectGraph::clear()
{
    qDeleteAll(m_nodes);
    m_nodes.clear();
    qDeleteAll(m_edges);
    m_edges.clear();
    m_adjacencyList.clear();
    m_isDirty = true;
}

void ProjectGraph::addNode(const GraphNode &node)
{
    if (m_nodes.contains(node.id)) {
        return;
    }

    GraphNode *newNode = new GraphNode(node);
    m_nodes[node.id] = newNode;

    // Update adjacency list
    if (!m_adjacencyList.contains(node.id)) {
        m_adjacencyList[node.id] = QSet<QString>();
    }
}

GraphNode* ProjectGraph::getNode(const QString &nodeId)
{
    return m_nodes.value(nodeId, nullptr);
}

const GraphNode* ProjectGraph::getNode(const QString &nodeId) const
{
    return m_nodes.value(nodeId, nullptr);
}

QList<GraphNode*> ProjectGraph::getAllNodes() const
{
    return m_nodes.values();
}

QList<GraphNode*> ProjectGraph::getNodesByType(const QString &type) const
{
    QList<GraphNode*> result;
    for (GraphNode *node : m_nodes) {
        if (node->type == type) {
            result.append(node);
        }
    }
    return result;
}

QList<GraphNode*> ProjectGraph::getFilesInDirectory(const QString &dirPath) const
{
    QList<GraphNode*> result;
    QString dirNodeId = generateNodeId(dirPath, u"directory"_s);
    const GraphNode *dirNode = getNode(dirNodeId);

    if (dirNode) {
        for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
            const GraphEdge *edge = it.value();
            if (edge->sourceId == dirNodeId && edge->relationship == u"contains"_s) {
                const GraphNode *childNode = getNode(edge->targetId);
                if (childNode && childNode->type == u"file"_s) {
                    result.append(const_cast<GraphNode*>(childNode));
                }
            }
        }
    }
    return result;
}

void ProjectGraph::addEdge(const GraphEdge &edge)
{
    // Check if edge already exists
    QString edgeKey = edge.sourceId + u"->"_s + edge.targetId + u"->"_s + edge.relationship;
    if (m_edges.contains(edgeKey)) {
        return;
    }

    GraphEdge *newEdge = new GraphEdge(edge);
    m_edges[edgeKey] = newEdge;

    // Update adjacency list
    if (!m_adjacencyList.contains(edge.sourceId)) {
        m_adjacencyList[edge.sourceId] = QSet<QString>();
    }
    m_adjacencyList[edge.sourceId].insert(edge.targetId);

    // Update node dependencies
    GraphNode *sourceNode = getNode(edge.sourceId);
    GraphNode *targetNode = getNode(edge.targetId);
    if (sourceNode && targetNode) {
        sourceNode->dependencies.insert(edge.targetId);
        targetNode->dependents.insert(edge.sourceId);
    }
}

void ProjectGraph::removeEdge(const QString &sourceId, const QString &targetId)
{
    QString edgeKey;
    for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
        if (it.key().startsWith(sourceId + u"->"_s + targetId + u"->"_s)) {
            edgeKey = it.key();
            break;
        }
    }

    if (edgeKey.isEmpty()) {
        return;
    }
    delete m_edges.take(edgeKey);

    // Another relationship between the same pair may still exist. Only drop
    // the adjacency entry and the node back-links when this was the last one.
    for (auto it = m_edges.constBegin(); it != m_edges.constEnd(); ++it) {
        if (it.key().startsWith(sourceId + u"->"_s + targetId + u"->"_s)) {
            return;
        }
    }
    if (m_adjacencyList.contains(sourceId)) {
        m_adjacencyList[sourceId].remove(targetId);
    }
    GraphNode *sourceNode = getNode(sourceId);
    GraphNode *targetNode = getNode(targetId);
    if (sourceNode) {
        sourceNode->dependencies.remove(targetId);
    }
    if (targetNode) {
        targetNode->dependents.remove(sourceId);
    }
}

QList<GraphEdge*> ProjectGraph::getEdgesFrom(const QString &sourceId) const
{
    QList<GraphEdge*> result;
    for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
        if (it.key().startsWith(sourceId + u"->"_s)) {
            result.append(it.value());
        }
    }
    return result;
}

QList<GraphEdge*> ProjectGraph::getEdgesTo(const QString &targetId) const
{
    QList<GraphEdge*> result;
    for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
        if (it.key().contains(u"->"_s + targetId + u"->"_s)) {
            result.append(it.value());
        }
    }
    return result;
}

QList<GraphEdge*> ProjectGraph::getEdgesByRelationship(const QString &relationship) const
{
    QList<GraphEdge*> result;
    for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
        if (it.key().endsWith(u"->"_s + relationship)) {
            result.append(it.value());
        }
    }
    return result;
}

QList<GraphNode*> ProjectGraph::findRelatedNodes(const QString &nodeId, int maxDepth) const
{
    QList<GraphNode*> result;
    QSet<QString> visited;
    QQueue<QPair<QString, int>> queue;

    queue.enqueue({nodeId, 0});
    visited.insert(nodeId);

    while (!queue.isEmpty()) {
        QPair<QString, int> current = queue.dequeue();
        QString currentId = current.first;
        int depth = current.second;

        const GraphNode *node = getNode(currentId);
        if (node) {
            result.append(const_cast<GraphNode*>(node));
        }

        if (depth < maxDepth) {
            if (m_adjacencyList.contains(currentId)) {
                for (const QString &neighborId : m_adjacencyList[currentId]) {
                    if (!visited.contains(neighborId)) {
                        visited.insert(neighborId);
                        queue.enqueue({neighborId, depth + 1});
                    }
                }
            }
        }
    }

    return result;
}

QList<QString> ProjectGraph::findImportPaths(const QString &sourceNodeId, const QString &targetNodeId) const
{
    // The return value is the node-id sequence of one path, because the
    // callers resolve each element with getNode(). It used to return
    // "a -> b -> c" strings, which getNode() could never resolve.
    QSet<QString> visited;
    QStack<QPair<QString, QStringList>> stack;
    stack.push({sourceNodeId, {sourceNodeId}});
    visited.insert(sourceNodeId);

    while (!stack.isEmpty()) {
        const QPair<QString, QStringList> current = stack.pop();
        if (current.first == targetNodeId) {
            return current.second;
        }
        const GraphNode *currentNode = getNode(current.first);
        if (!currentNode) {
            continue;
        }
        for (const QString &dependencyId : currentNode->dependencies) {
            if (visited.contains(dependencyId)) {
                continue;
            }
            visited.insert(dependencyId);
            QStringList newPath = current.second;
            newPath.append(dependencyId);
            stack.push({dependencyId, newPath});
        }
    }
    return {};
}

QSet<QString> ProjectGraph::getTransitiveDependencies(const QString &nodeId) const
{
    QSet<QString> result;
    QSet<QString> visited;
    QQueue<QString> queue;

    queue.enqueue(nodeId);
    visited.insert(nodeId);

    while (!queue.isEmpty()) {
        QString currentId = queue.dequeue();
        const GraphNode *currentNode = getNode(currentId);
        if (currentNode) {
            for (const QString &dependencyId : currentNode->dependencies) {
                if (!visited.contains(dependencyId)) {
                    visited.insert(dependencyId);
                    result.insert(dependencyId);
                    queue.enqueue(dependencyId);
                }
            }
        }
    }

    return result;
}

QSet<QString> ProjectGraph::getTransitiveDependents(const QString &nodeId) const
{
    QSet<QString> result;
    QSet<QString> visited;
    QQueue<QString> queue;

    queue.enqueue(nodeId);
    visited.insert(nodeId);

    while (!queue.isEmpty()) {
        const QString currentId = queue.dequeue();
        // m_adjacencyList[source] holds source's dependencies (the forward
        // direction). Dependents are the reverse edges, kept on the node.
        const GraphNode *current = getNode(currentId);
        if (!current) {
            continue;
        }
        for (const QString &dependentId : current->dependents) {
            if (!visited.contains(dependentId)) {
                visited.insert(dependentId);
                result.insert(dependentId);
                queue.enqueue(dependentId);
            }
        }
    }

    return result;
}

QList<GraphNode*> ProjectGraph::getDependencyChain(const QString &startNodeId, const QString &endNodeId) const
{
    QSet<QString> visited;
    QStack<QPair<QString, QStringList>> stack;
    QList<GraphNode*> result;

    stack.push({startNodeId, {}});
    visited.insert(startNodeId);

    while (!stack.isEmpty()) {
        QPair<QString, QStringList> current = stack.pop();
        QString currentNodeId = current.first;
        QStringList path = current.second;

        if (currentNodeId == endNodeId) {
            for (const QString &nodeId : path) {
                const GraphNode *node = getNode(nodeId);
                if (node) {
                    result.append(const_cast<GraphNode*>(node));
                }
            }
            const GraphNode *endNode = getNode(endNodeId);
            if (endNode) {
                result.append(const_cast<GraphNode*>(endNode));
            }
            continue;
        }

        const GraphNode *currentNode = getNode(currentNodeId);
        if (currentNode) {
            for (const QString &dependencyId : currentNode->dependencies) {
                if (!visited.contains(dependencyId)) {
                    visited.insert(dependencyId);
                    QStringList newPath = path;
                    newPath.append(dependencyId);
                    stack.push({dependencyId, newPath});
                }
            }
        }
    }

    return result;
}

const GraphNode *ProjectGraph::findNodeByName(const QString &name) const
{
    if (name.isEmpty()) {
        return nullptr;
    }
    const GraphNode *match = nullptr;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        const GraphNode *node = it.value();
        if (!node || node->type != u"file"_s || node->path.isEmpty()) {
            continue;
        }
        if (node->name == name || QFileInfo(node->path).fileName() == name
            || displayPath(node->path) == name) {
            if (match) {
                return nullptr; // ambiguous basename
            }
            match = node;
        }
    }
    return match;
}

QSet<QString> ProjectGraph::resolveImportTargets(const QString &content, const QString &sourcePath) const
{
    QSet<QString> targets;
    if (content.isEmpty() || m_nodes.isEmpty()) {
        return targets;
    }
    const QFileInfo sourceInfo(sourcePath);
    const QString sourceDir = sourceInfo.absolutePath();
    const QString workspace = m_workspacePath;

    auto remember = [&targets](const GraphNode *node) {
        if (node && node->type == u"file"_s) {
            targets.insert(node->id);
        }
    };
    auto lookupPath = [this, &remember](const QString &candidate) {
        if (candidate.isEmpty()) {
            return;
        }
        const QString clean = QDir::cleanPath(candidate);
        if (const GraphNode *direct = getNode(generateNodeId(clean, u"file"_s))) {
            remember(direct);
        }
    };
    auto lookupBasename = [this, &remember](const QString &candidate) {
        if (candidate.isEmpty()) {
            return;
        }
        remember(findNodeByName(QFileInfo(candidate).fileName()));
    };
    static const QStringList extensions = {
        u".h"_s, u".hpp"_s, u".hh"_s, u".hxx"_s, u".c"_s, u".cc"_s, u".cpp"_s,
        u".cxx"_s, u".m"_s, u".mm"_s, u".py"_s, u".pyi"_s, u".js"_s, u".jsx"_s,
        u".ts"_s, u".tsx"_s, u".mjs"_s, u".cjs"_s, u".rs"_s, u".go"_s,
        u".java"_s, u".kt"_s, u".kts"_s, u".swift"_s, u".cs"_s, u".rb"_s,
        u".php"_s, u".vue"_s, u".svelte"_s,
    };
    auto tryCandidates = [&lookupPath](const QString &base, bool addExtensions) {
        lookupPath(base);
        if (!addExtensions) {
            return;
        }
        for (const QString &extension : extensions) {
            lookupPath(base + extension);
        }
        for (const QString &extension : extensions) {
            lookupPath(base + u"/index"_s + extension);
        }
        lookupPath(base + u"/__init__.py"_s);
    };

    // C/C++: #include "x" and #include <x>
    static const QRegularExpression cppInclude(uR"(#\s*include\s*[<"]([^>"]+)[>"])"_s);
    QRegularExpressionMatchIterator includes = cppInclude.globalMatch(content);
    while (includes.hasNext()) {
        const QString include = includes.next().captured(1).trimmed();
        if (include.isEmpty()) {
            continue;
        }
        const QStringList bases = {
            QDir(sourceDir).filePath(include),
            QDir(workspace).filePath(include),
            QDir(workspace + u"/include"_s).filePath(include),
            QDir(workspace + u"/src"_s).filePath(include),
        };
        const int before = targets.size();
        for (const QString &base : bases) {
            tryCandidates(base, true);
        }
        if (targets.size() == before && !QFileInfo(include).suffix().isEmpty()) {
            // A header may be named without the directory prefix.
            for (const QString &base : bases) {
                lookupBasename(base);
            }
        }
    }

    // Python: import a.b / from a.b import c. Relative forms are resolved
    // against the source directory; absolute module paths against the root.
    static const QRegularExpression pythonImport(uR"(^\s*(?:from\s+([.\w]+)\s+import|import\s+([.\w]+)))"_s,
                                                QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator pyImports = pythonImport.globalMatch(content);
    while (pyImports.hasNext()) {
        const QRegularExpressionMatch match = pyImports.next();
        QString module = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        if (module.isEmpty()) {
            continue;
        }
        int leadingDots = 0;
        while (leadingDots < module.size() && module.at(leadingDots) == u'.') {
            ++leadingDots;
        }
        module = module.mid(leadingDots);
        if (module.isEmpty()) {
            continue;
        }
        QString relative = module;
        relative.replace(u'.', u'/');
        const QString root = leadingDots > 0 ? sourceDir : workspace;
        const int before = targets.size();
        tryCandidates(QDir(root).filePath(relative), false);
        const QStringList parts = relative.split(u'/');
        if (!parts.isEmpty()) {
            tryCandidates(QDir(root).filePath(parts.join(u'/')), true);
        }
        if (targets.size() == before) {
            lookupBasename(QDir(root).filePath(parts.isEmpty() ? relative : parts.last()));
        }
    }

    // JavaScript/TypeScript imports and require(), but only relative paths:
    // a bare package name names node_modules, not a workspace file.
    static const QRegularExpression jsImport(
        uR"((?:from\s*|require\s*\(\s*)["']([^"']+)["'])"_s);
    QRegularExpressionMatchIterator jsImports = jsImport.globalMatch(content);
    while (jsImports.hasNext()) {
        const QString import = jsImports.next().captured(1).trimmed();
        if (!import.startsWith(u'.') && !import.startsWith(u'/')) {
            continue;
        }
        const QString base = QDir(sourceDir).filePath(import);
        const int before = targets.size();
        tryCandidates(base, true);
        if (targets.size() == before) {
            lookupBasename(base);
        }
    }

    return targets;
}

void ProjectGraph::reindexFileDependencies(GraphNode *node)
{
    if (!node || node->type != u"file"_s || node->path.isEmpty()) {
        return;
    }
    // Replace this file's import edges. removeEdge also fixes the reciprocal
    // dependents sets, so a changed include cannot leave a stale edge behind.
    const QSet<QString> oldTargets = node->dependencies;
    for (const QString &target : oldTargets) {
        removeEdge(node->id, target);
    }
    node->dependencies.clear();

    const QSet<QString> targets = resolveImportTargets(node->content, node->path);
    for (const QString &target : targets) {
        GraphEdge edge;
        edge.sourceId = node->id;
        edge.targetId = target;
        edge.relationship = u"imports"_s;
        edge.isSafe = true;
        addEdge(edge);
    }
}

void ProjectGraph::analyzeFileDependencies()
{
    // Start from a clean dependency state: generateGraph() may have created
    // containment edges only, and a re-analysis after an update must not keep
    // a previous import that no longer exists.
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        GraphNode *node = it.value();
        if (node && node->type == u"file"_s) {
            const QSet<QString> oldTargets = node->dependencies;
            for (const QString &target : oldTargets) {
                removeEdge(node->id, target);
            }
            node->dependencies.clear();
        }
    }
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        GraphNode *node = it.value();
        if (!node || node->type != u"file"_s) {
            continue;
        }
        const QSet<QString> targets = resolveImportTargets(node->content, node->path);
        for (const QString &target : targets) {
            GraphEdge edge;
            edge.sourceId = node->id;
            edge.targetId = target;
            edge.relationship = u"imports"_s;
            edge.isSafe = true;
            addEdge(edge);
        }
    }
}

void ProjectGraph::analyzeCodeStructure()
{
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        GraphNode *node = it.value();
        if (node->type == u"file"_s) {
            node->complexity = extractClassDefinitions(node->content).count() + extractFunctionDefinitions(node->content).count();
        }
    }
}

void ProjectGraph::updateSecurityStatus()
{
    for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
        GraphEdge *edge = it.value();
        GraphNode *sourceNode = getNode(edge->sourceId);
        GraphNode *targetNode = getNode(edge->targetId);

        if (sourceNode && targetNode) {
            edge->isSafe = isSafeRelationship(sourceNode, targetNode, edge->relationship);
        }
    }
}

void ProjectGraph::generateSummaryReport() const
{
    QMap<QString, int> typeCount;
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        const GraphNode *node = it.value();
        typeCount[node->type]++;
    }

    QMap<QString, int> relationshipCount;
    for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
        const GraphEdge *edge = it.value();
        relationshipCount[edge->relationship]++;
    }

    int totalComplexity = 0;
    int totalLinesOfCode = 0;
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        const GraphNode *node = it.value();
        totalComplexity += node->complexity;
        totalLinesOfCode += node->linesOfCode;
    }

    qDebug() << "Project Graph Summary:";
    qDebug() << "  Nodes:" << m_nodes.size();
    qDebug() << "  Edges:" << m_edges.size();
    qDebug() << "  Total Complexity:" << totalComplexity;
    qDebug() << "  Total Lines of Code:" << totalLinesOfCode;
    qDebug() << "  Node Types:";
    for (auto it = typeCount.begin(); it != typeCount.end(); ++it) {
        qDebug() << "    " << it.key() << ":" << it.value();
    }
    qDebug() << "  Relationship Types:";
    for (auto it = relationshipCount.begin(); it != relationshipCount.end(); ++it) {
        qDebug() << "    " << it.key() << ":" << it.value();
    }
}

QList<GraphSymbol> ProjectGraph::extractSymbols(const QString &content, const QString &filePath) const
{
    QList<GraphSymbol> symbols;
    if (content.isEmpty()) {
        return symbols;
    }
    const QString language = getNodeTypeFromPath(filePath);
    const bool isPython = language == u"python"_s;
    const bool isJs = language == u"javascript"_s || language == u"typescript"_s
        || language == u"jsx"_s || language == u"tsx"_s;

    // The line index and the split lines are computed once per file. They used
    // to be recomputed for every symbol: lineOf() copied the whole prefix of
    // the file to count its newlines, and indentEndLine() split the file into
    // lines again - so indexing one file cost symbols x file size.
    QVector<int> lineStarts;
    lineStarts.reserve(64);
    lineStarts.append(0);
    for (int i = 0; i < content.size(); ++i) {
        if (content.at(i) == u'\n') {
            lineStarts.append(i + 1);
        }
    }
    const QStringList contentLines = content.split(u'\n');

    auto lineOf = [&lineStarts](int offset) -> int {
        if (offset <= 0) {
            return 1;
        }
        // The last line start that is not past the offset.
        const auto it = std::upper_bound(lineStarts.constBegin(), lineStarts.constEnd(), offset);
        return int(it - lineStarts.constBegin());
    };
    auto columnOf = [&content](int offset) -> int {
        const int nl = content.lastIndexOf(u'\n', qMax(0, offset - 1));
        return offset - nl; // 1-based column
    };
    // Brace-matched end line for C-like languages, starting at the match.
    auto braceEndLine = [&content, &lineOf](int from) -> int {
        int depth = 0;
        bool opened = false;
        const int cap = qMin(content.size(), from + 200 * 1024);
        for (int i = from; i < cap; ++i) {
            const QChar c = content.at(i);
            if (c == u'{') {
                ++depth;
                opened = true;
            } else if (c == u'}') {
                --depth;
                if (opened && depth <= 0) {
                    return lineOf(i);
                }
            }
        }
        return 0;
    };
    // Indentation-based end line for Python: the block ends at the first
    // non-blank line whose indent is not deeper than the definition's.
    auto indentEndLine = [&contentLines, &lineOf](int from, int baseIndent) -> int {
        const int startLine = lineOf(from);
        int endLine = startLine;
        for (int i = startLine; i < contentLines.size(); ++i) {
            const QString &line = contentLines.at(i);
            if (line.trimmed().isEmpty()) {
                continue;
            }
            int indent = 0;
            while (indent < line.size() && (line.at(indent) == u' ' || line.at(indent) == u'\t')) {
                ++indent;
            }
            if (indent <= baseIndent) {
                break;
            }
            endLine = i + 1;
        }
        return endLine;
    };
    auto add = [&](const QString &name, const QString &kind, int offset, int baseIndent, bool braceLang) {
        if (name.isEmpty() || symbols.size() >= 500) {
            return;
        }
        GraphSymbol sym;
        sym.name = name;
        sym.kind = kind;
        sym.line = lineOf(offset);
        sym.column = columnOf(offset);
        sym.endLine = braceLang ? braceEndLine(offset) : indentEndLine(offset, baseIndent);
        symbols.append(sym);
    };

    if (isPython) {
        static const QRegularExpression defRe(u"^([ \\t]*)(?:async[ \\t]+)?def[ \\t]+(\\w+)"_s,
                                              QRegularExpression::MultilineOption);
        static const QRegularExpression classRe(u"^([ \\t]*)class[ \\t]+(\\w+)"_s,
                                                QRegularExpression::MultilineOption);
        QRegularExpressionMatchIterator it = defRe.globalMatch(content);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            add(m.captured(2), u"function"_s, m.capturedStart(2), m.captured(1).size(), false);
        }
        it = classRe.globalMatch(content);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            add(m.captured(2), u"class"_s, m.capturedStart(2), m.captured(1).size(), false);
        }
        return symbols;
    }

    if (isJs) {
        static const QRegularExpression funcRe(u"\\bfunction[ \\t]+(\\w+)"_s);
        static const QRegularExpression arrowRe(
            u"\\b(?:const|let|var)[ \\t]+(\\w+)[ \\t]*=[ \\t]*(?:async[ \\t]*)?(?:\\([^)]*\\)|\\w+)[ \\t]*=>"_s);
        static const QRegularExpression classRe(u"\\bclass[ \\t]+(\\w+)"_s);
        QRegularExpressionMatchIterator it = funcRe.globalMatch(content);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            add(m.captured(1), u"function"_s, m.capturedStart(1), 0, true);
        }
        it = arrowRe.globalMatch(content);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            add(m.captured(1), u"function"_s, m.capturedStart(1), 0, true);
        }
        it = classRe.globalMatch(content);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            add(m.captured(1), u"class"_s, m.capturedStart(1), 0, true);
        }
        return symbols;
    }

    // C-like and every other brace language: classes, and functions whose
    // parameter list is followed by an opening brace (that requirement is
    // what keeps if/for/while/switch out).
    static const QSet<QString> keywords{
        u"if"_s, u"for"_s, u"while"_s, u"switch"_s, u"catch"_s, u"return"_s, u"sizeof"_s, u"do"_s, u"else"_s};
    static const QRegularExpression classRe(u"\\bclass[ \\t]+(\\w+)"_s);
    static const QRegularExpression funcRe(u"\\b(\\w+)[ \\t]*\\([^;{}]*\\)[ \\t]*(?:const[ \\t]*)?\\{"_s);
    QRegularExpressionMatchIterator it = classRe.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        add(m.captured(1), u"class"_s, m.capturedStart(1), 0, true);
    }
    it = funcRe.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString name = m.captured(1);
        if (keywords.contains(name)) {
            continue;
        }
        add(name, u"function"_s, m.capturedStart(1), 0, true);
    }
    return symbols;
}

void ProjectGraph::indexFileSymbols(GraphNode *fileNode)
{
    if (!fileNode || fileNode->type != u"file"_s) {
        return;
    }
    const QList<GraphSymbol> symbols = extractSymbols(fileNode->content, fileNode->path);
    QJsonArray arr;
    for (const GraphSymbol &sym : symbols) {
        QJsonObject obj;
        obj.insert(u"name"_s, sym.name);
        obj.insert(u"kind"_s, sym.kind);
        obj.insert(u"line"_s, sym.line);
        obj.insert(u"column"_s, sym.column);
        obj.insert(u"endLine"_s, sym.endLine);
        arr.append(obj);
    }
    fileNode->metadata.insert(u"symbols"_s, arr);
}

QList<GraphSymbol> ProjectGraph::symbolsInFile(const QString &filePath) const
{
    const GraphNode *node = getNode(generateNodeId(filePath));
    if (!node) {
        // The graph may have indexed a path spelling that differs only by a
        // symlinked prefix (macOS /var -> /private/var). Match canonically
        // before giving up.
        const QString wanted = canonicalLookupPath(filePath, m_workspacePath);
        for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
            const GraphNode *candidate = it.value();
            if (candidate && candidate->type == u"file"_s && !candidate->path.isEmpty()
                && canonicalLookupPath(candidate->path, m_workspacePath) == wanted) {
                node = candidate;
                break;
            }
        }
    }
    if (!node) {
        return {};
    }
    QList<GraphSymbol> out;
    const QJsonArray arr = node->metadata.value(u"symbols"_s).toArray();
    for (const QJsonValue &value : arr) {
        const QJsonObject obj = value.toObject();
        GraphSymbol sym;
        sym.name = obj.value(u"name"_s).toString();
        sym.kind = obj.value(u"kind"_s).toString();
        sym.line = obj.value(u"line"_s).toInt();
        sym.column = obj.value(u"column"_s).toInt();
        sym.endLine = obj.value(u"endLine"_s).toInt();
        if (!sym.name.isEmpty()) {
            out.append(sym);
        }
    }
    return out;
}

GraphSymbol ProjectGraph::enclosingSymbol(const QString &filePath, int line) const
{
    const QList<GraphSymbol> symbols = symbolsInFile(filePath);
    GraphSymbol covering;
    GraphSymbol nearest;
    for (const GraphSymbol &sym : symbols) {
        if (sym.line > line) {
            continue;
        }
        if (nearest.name.isEmpty() || sym.line >= nearest.line) {
            nearest = sym;
        }
        // A symbol whose range is known and still covers the line wins over
        // everything that merely starts before it.
        if (sym.endLine > 0 && sym.endLine >= line
            && (covering.name.isEmpty() || sym.line >= covering.line)) {
            covering = sym;
        }
    }
    return covering.name.isEmpty() ? nearest : covering;
}

int ProjectGraph::staleFileCount() const
{
    int stale = 0;
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        const GraphNode *node = it.value();
        if (!node || (node->type != u"file"_s && node->type != u"directory"_s)) {
            continue;
        }
        // An empty path used to be skipped entirely, so a crafted cache whose
        // nodes all had path "" was never considered stale and its injected
        // names/symbols stayed in the model context forever.
        if (node->path.isEmpty()) {
            ++stale;
            continue;
        }
        // A cache node naming a path outside this workspace is untrusted
        // content, not a file of this project.
        if (!m_workspacePath.isEmpty()) {
            const QString root = QDir::cleanPath(m_workspacePath);
            const QString path = QDir::cleanPath(node->path);
            if (path != root && !path.startsWith(root + u'/')) {
                ++stale;
                continue;
            }
        }
        const QFileInfo info(node->path);
        if (!info.exists() || info.lastModified().toSecsSinceEpoch() != node->lastModified) {
            ++stale;
        }
    }
    return stale;
}

QString ProjectGraph::displayPath(const QString &path) const
{
    if (path.isEmpty()) {
        return {};
    }
    if (m_workspacePath.isEmpty()) {
        return QDir::cleanPath(path);
    }
    const QString relative = QDir(m_workspacePath).relativeFilePath(path);
    if (relative == u"."_s) {
        return QDir(m_workspacePath).dirName();
    }
    return relative;
}

QString ProjectGraph::nodeLabel(const GraphNode *node) const
{
    if (!node) {
        return {};
    }
    if (!node->path.isEmpty()) {
        return displayPath(node->path);
    }
    if (!node->name.isEmpty()) {
        return node->name;
    }
    return node->id.left(12);
}

QString ProjectGraph::nodeSymbols(const GraphNode *node) const
{
    if (!node) {
        return {};
    }
    const QJsonArray symbols = node->metadata.value(u"symbols"_s).toArray();
    QStringList names;
    for (const QJsonValue &value : symbols) {
        const QString name = value.toObject().value(u"name"_s).toString().trimmed();
        if (!name.isEmpty() && !names.contains(name)) {
            names.append(name);
            if (names.size() >= 8) {
                break;
            }
        }
    }
    return names.join(u", "_s);
}

GraphNode *ProjectGraph::findNodeByPath(const QString &path)
{
    if (path.isEmpty()) {
        return nullptr;
    }
    QString absolute = path;
    if (QDir::isRelativePath(absolute) && !m_workspacePath.isEmpty()) {
        absolute = QDir(m_workspacePath).absoluteFilePath(absolute);
    }
    absolute = QDir::cleanPath(absolute);
    // Fast path: the caller usually names a file exactly as it appears in the
    // workspace, and node ids are a hash of that path. Scanning every node and
    // canonicalising each one is too expensive for compaction summaries.
    if (GraphNode *direct = getNode(generateNodeId(absolute, u"file"_s))) {
        return direct;
    }
    if (GraphNode *direct = getNode(generateNodeId(absolute, u"directory"_s))) {
        return direct;
    }
    const QString wanted = canonicalLookupPath(path, m_workspacePath);
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        GraphNode *node = it.value();
        if (!node || node->path.isEmpty()) {
            continue;
        }
        if (canonicalLookupPath(node->path, m_workspacePath) == wanted) {
            return node;
        }
        if (displayPath(node->path) == path || node->name == path) {
            return node;
        }
    }
    return nullptr;
}

const GraphNode *ProjectGraph::findNodeByPath(const QString &path) const
{
    return const_cast<ProjectGraph *>(this)->findNodeByPath(path);
}

QString ProjectGraph::contextSummary(int maxNodes, int maxEdges, int level) const
{
    if (m_nodes.isEmpty()) {
        return {};
    }
    const int nodeCap = qBound(1, maxNodes, 5000);
    const int edgeCap = qBound(0, maxEdges, 20000);
    const int detail = qBound(0, level, 3);

    int directories = 0;
    int files = 0;
    QSet<QString> languages;
    QList<const GraphNode *> fileNodes;
    QList<const GraphNode *> topDirectories;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        const GraphNode *node = it.value();
        if (!node) {
            continue;
        }
        if (node->type == u"directory"_s) {
            ++directories;
            const QString relative = displayPath(node->path);
            if (!relative.contains(u'/') && relative != displayPath(m_workspacePath)) {
                topDirectories.append(node);
            }
        } else if (node->type == u"file"_s) {
            ++files;
            fileNodes.append(node);
            const QString language = node->metadata.value(u"language"_s).toString();
            if (!language.isEmpty()) {
                languages.insert(language);
            }
        }
    }

    std::sort(fileNodes.begin(), fileNodes.end(), [this](const GraphNode *a, const GraphNode *b) {
        if (a->linesOfCode != b->linesOfCode) {
            return a->linesOfCode > b->linesOfCode;
        }
        return displayPath(a->path) < displayPath(b->path);
    });
    std::sort(topDirectories.begin(), topDirectories.end(), [this](const GraphNode *a, const GraphNode *b) {
        return displayPath(a->path) < displayPath(b->path);
    });

    QStringList lines;
    lines << u"Workspace: %1"_s.arg(displayPath(m_workspacePath));
    QStringList languageList = languages.values();
    languageList.sort();
    const QString languageText = languageList.join(u", "_s);
    lines << u"Indexed: %1 files, %2 directories, %3 edges%4"_s
                 .arg(files)
                 .arg(directories)
                 .arg(m_edges.size())
                 .arg(languageText.isEmpty() ? QString() : u"; languages: "_s + languageText);
    if (detail >= 3) {
        lines << u"Use query_project_graph for a file's symbols, imports, dependents, or a dependency path."_s;
        return lines.join(u'\n');
    }

    if (!topDirectories.isEmpty()) {
        QStringList names;
        for (const GraphNode *node : topDirectories) {
            if (names.size() >= 16) {
                names << u"…"_s;
                break;
            }
            names << displayPath(node->path) + u'/';
        }
        lines << u"Top-level directories: %1"_s.arg(names.join(u", "_s));
    }

    int shownFiles = 0;
    QStringList fileLines;
    for (const GraphNode *node : fileNodes) {
        if (shownFiles >= nodeCap) {
            fileLines << u"… (%1 more files)"_s.arg(fileNodes.size() - shownFiles);
            break;
        }
        QString line = u"- %1"_s.arg(displayPath(node->path));
        const QString language = node->metadata.value(u"language"_s).toString();
        if (!language.isEmpty()) {
            line += u" [%1]"_s.arg(language);
        }
        if (node->linesOfCode > 0) {
            line += u" %1 loc"_s.arg(node->linesOfCode);
        }
        if (detail <= 1) {
            const QString symbols = nodeSymbols(node);
            if (!symbols.isEmpty()) {
                line += u"; symbols: "_s + symbols;
            }
        }
        fileLines << line;
        ++shownFiles;
    }
    if (!fileLines.isEmpty()) {
        lines << u"Files:"_s;
        lines.append(fileLines);
    }

    if (detail <= 1 && edgeCap > 0) {
        QStringList edgeLines;
        int shownEdges = 0;
        for (auto it = m_edges.constBegin(); it != m_edges.constEnd(); ++it) {
            const GraphEdge *edge = it.value();
            if (!edge) {
                continue;
            }
            // "contains" is useful for the tree but not for understanding a
            // dependency; the file list already carries the tree.
            if (edge->relationship == u"contains"_s) {
                continue;
            }
            const GraphNode *source = getNode(edge->sourceId);
            const GraphNode *target = getNode(edge->targetId);
            if (!source || !target) {
                continue;
            }
            edgeLines << u"- %1 -> %2 (%3)"_s.arg(nodeLabel(source), nodeLabel(target), edge->relationship);
            if (++shownEdges >= edgeCap) {
                break;
            }
        }
        if (!edgeLines.isEmpty()) {
            lines << u"Dependencies:"_s;
            lines.append(edgeLines);
        }
    }
    return lines.join(u'\n');
}

bool ProjectGraph::saveToFile(const QString &filePath) const
{
    const QString directory = QFileInfo(filePath).absolutePath();
    if (!directory.isEmpty() && !QDir().mkpath(directory)) {
        return false;
    }
    // A graph cache is disposable, but a half-written one is not harmless: the
    // next start would read a truncated JSON object. QSaveFile commits the
    // whole document or leaves the previous one intact.
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QJsonObject json = toJson();
    QJsonDocument doc(json);
    if (file.write(doc.toJson()) < 0) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool ProjectGraph::loadFromFile(const QString &filePath)
{
    // A project-controlled path can be a FIFO, a device or a symlink to
    // /dev/zero; opening/readAll() would block or exhaust memory at workspace
    // open. Only a regular, non-symlink file of bounded size is read.
    const QFileInfo source(filePath);
    constexpr qint64 kMaxGraphBytes = 64 * 1024 * 1024;
    if (!source.exists() || !source.isFile() || source.isSymLink()) {
        return false;
    }
    if (source.size() > kMaxGraphBytes) {
        qWarning() << "KateAI: project graph file too large, not loaded:" << filePath << source.size();
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QByteArray data = file.read(kMaxGraphBytes + 1);
    if (data.size() > kMaxGraphBytes) {
        return false;
    }
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        return false;
    }

    bool result = fromJson(doc.object());
    if (result) {
        m_lastUpdateTime = QDateTime::currentSecsSinceEpoch();
        m_isDirty = false;
    }

    return result;
}

QJsonObject ProjectGraph::toJson() const
{
    QJsonObject json;
    json.insert(u"schemaVersion"_s, kGraphSchemaVersion);
    json.insert(u"workspacePath"_s, m_workspacePath);

    // Add nodes
    QJsonArray nodesArray;
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        const GraphNode *node = it.value();
        QJsonObject nodeJson;

        nodeJson.insert(u"id"_s, node->id);
        nodeJson.insert(u"name"_s, node->name);
        nodeJson.insert(u"type"_s, node->type);
        nodeJson.insert(u"path"_s, node->path);
        // The full file preview is in-memory analysis input, not graph data:
        // serialising it made the cache many megabytes and copied source text
        // into a second on-disk location. Symbols and metrics are enough for
        // every query the graph tool serves.
        nodeJson.insert(u"content"_s, QString());

        // Add dependencies. The sets are unordered and the hash seed is
        // different in every process, so the lists are sorted: the cache is
        // written to the project and compared and committed, and the same
        // graph has to serialise to the same bytes every time.
        QStringList dependencies = node->dependencies.values();
        dependencies.sort();
        QJsonArray dependenciesArray;
        for (const QString &dep : dependencies) {
            dependenciesArray.append(dep);
        }
        nodeJson.insert(u"dependencies"_s, dependenciesArray);

        // Add dependents
        QStringList dependents = node->dependents.values();
        dependents.sort();
        QJsonArray dependentsArray;
        for (const QString &dep : dependents) {
            dependentsArray.append(dep);
        }
        nodeJson.insert(u"dependents"_s, dependentsArray);

        nodeJson.insert(u"complexity"_s, node->complexity);
        nodeJson.insert(u"linesOfCode"_s, node->linesOfCode);
        nodeJson.insert(u"isReadOnly"_s, node->isReadOnly);
        nodeJson.insert(u"lastModified"_s, node->lastModified);
        // metadata carries the language every file node is keyed on; dropping
        // it made a reloaded graph lose the one field the graph tool filters
        // and formats by, so a restored session came back with unknown files.
        QJsonObject metadataJson;
        for (auto it = node->metadata.begin(); it != node->metadata.end(); ++it) {
            metadataJson.insert(it.key(), it.value());
        }
        nodeJson.insert(u"metadata"_s, metadataJson);

        nodesArray.append(nodeJson);
    }
    json.insert(u"nodes"_s, nodesArray);

    // Add edges
    QJsonArray edgesArray;
    for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
        const GraphEdge *edge = it.value();
        QJsonObject edgeJson;

        edgeJson.insert(u"sourceId"_s, edge->sourceId);
        edgeJson.insert(u"targetId"_s, edge->targetId);
        edgeJson.insert(u"relationship"_s, edge->relationship);
        edgeJson.insert(u"details"_s, edge->details);
        edgeJson.insert(u"isSafe"_s, edge->isSafe);

        edgesArray.append(edgeJson);
    }
    json.insert(u"edges"_s, edgesArray);

    return json;
}

bool ProjectGraph::fromJson(const QJsonObject &json)
{
    if (json.value(u"schemaVersion"_s).toInt() < kGraphSchemaVersion) {
        return false;
    }
    if (m_workspacePath.isEmpty()) {
        m_workspacePath = json.value(u"workspacePath"_s).toString();
    }
    // Clear existing graph
    clear();

    // Load nodes
    QJsonArray nodesArray = json.value(u"nodes"_s).toArray();
    for (const QJsonValue &nodeValue : nodesArray) {
        QJsonObject nodeJson = nodeValue.toObject();
        GraphNode node;

        node.id = nodeJson.value(u"id"_s).toString();
        node.name = nodeJson.value(u"name"_s).toString();
        node.type = nodeJson.value(u"type"_s).toString();
        node.path = nodeJson.value(u"path"_s).toString();
        node.content = nodeJson.value(u"content"_s).toString();
        node.isReadOnly = nodeJson.value(u"isReadOnly"_s).toBool();
        node.lastModified = nodeJson.value(u"lastModified"_s).toInteger();

        // Load dependencies
        QJsonArray dependenciesArray = nodeJson.value(u"dependencies"_s).toArray();
        for (const QJsonValue &depValue : dependenciesArray) {
            node.dependencies.insert(depValue.toString());
        }

        // Load dependents
        QJsonArray dependentsArray = nodeJson.value(u"dependents"_s).toArray();
        for (const QJsonValue &depValue : dependentsArray) {
            node.dependents.insert(depValue.toString());
        }

        node.complexity = nodeJson.value(u"complexity"_s).toInteger();
        node.linesOfCode = nodeJson.value(u"linesOfCode"_s).toInteger();
        node.metadata = nodeJson.value(u"metadata"_s).toObject();

        addNode(node);
    }

    // Load edges
    QJsonArray edgesArray = json.value(u"edges"_s).toArray();
    for (const QJsonValue &edgeValue : edgesArray) {
        QJsonObject edgeJson = edgeValue.toObject();
        GraphEdge edge;

        edge.sourceId = edgeJson.value(u"sourceId"_s).toString();
        edge.targetId = edgeJson.value(u"targetId"_s).toString();
        edge.relationship = edgeJson.value(u"relationship"_s).toString();
        edge.details = edgeJson.value(u"details"_s).toString();
        edge.isSafe = edgeJson.value(u"isSafe"_s).toBool();

        addEdge(edge);
    }

    return true;
}

void ProjectGraph::buildAdjacencyList()
{
    m_adjacencyList.clear();
    for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
        const GraphEdge *edge = it.value();
        if (!m_adjacencyList.contains(edge->sourceId)) {
            m_adjacencyList[edge->sourceId] = QSet<QString>();
        }
        m_adjacencyList[edge->sourceId].insert(edge->targetId);
    }
}

void ProjectGraph::cleanupDeletedNodes()
{
    QSet<QString> existingNodeIds;
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        existingNodeIds.insert(it.key());
    }

    for (auto it = m_edges.begin(); it != m_edges.end(); ++it) {
        const QString &edgeKey = it.key();
        // Parse edgeKey to get sourceId and targetId (see removeNode: split
        // from the right so a targetId containing "->" survives).
        int arrow1 = edgeKey.indexOf(u"->"_s);
        int arrow2 = edgeKey.lastIndexOf(u"->"_s);
        if (arrow1 != -1 && arrow2 != -1) {
            QString sourceId = edgeKey.mid(0, arrow1);
            QString targetId = edgeKey.mid(arrow1 + 2, arrow2 - arrow1 - 2);
            if (!existingNodeIds.contains(sourceId) || !existingNodeIds.contains(targetId)) {
                delete it.value();
                it = m_edges.erase(it);
            }
        }
    }

    for (auto it = m_adjacencyList.begin(); it != m_adjacencyList.end(); ++it) {
        QSet<QString> &neighbors = it.value();
        QSet<QString> newNeighbors;
        for (const QString &neighborId : neighbors) {
            if (existingNodeIds.contains(neighborId)) {
                newNeighbors.insert(neighborId);
            }
        }
        neighbors = newNeighbors;
    }
}

int ProjectGraph::lineCount(const QString &content)
{
    if (content.isEmpty()) {
        return 0;
    }
    // count('\n') undercounts a last line without trailing newline (single-line
    // file without '\n' reported 0 LOC while UI shows 1).
    return content.count(u'\n') + (content.endsWith(u'\n') ? 0 : 1);
}

int ProjectGraph::definitionCount(ProjectGraph &analyser, const QString &content)
{
    return analyser.extractClassDefinitions(content).count()
        + analyser.extractFunctionDefinitions(content).count();
}

void ProjectGraph::updateNodeMetrics(GraphNode *node)
{
    if (!node) {
        return;
    }

    node->linesOfCode = lineCount(node->content);
    node->complexity = definitionCount(*this, node->content);
}

QStringList ProjectGraph::extractClassDefinitions(const QString &content) const
{
    // Extract class definitions from C++ and Java
    QRegularExpression classRegex(u"class\\s+\\w+\\s*:\\s*public\\s+\\w+"_s);
    QRegularExpressionMatchIterator it = classRegex.globalMatch(content);
    QStringList matches;
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        matches.append(match.captured(0));
    }
    return matches;
}

QStringList ProjectGraph::extractFunctionDefinitions(const QString &content) const
{
    // Extract function definitions
    QRegularExpression functionRegex(u"\\w+\\s+\\w+\\s*\\([^)]*\\)\\s*\\{?"_s);
    QRegularExpressionMatchIterator it = functionRegex.globalMatch(content);
    QStringList matches;
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        matches.append(match.captured(0));
    }
    return matches;
}

bool ProjectGraph::isSafeRelationship(const GraphNode *source, const GraphNode *target, const QString &relationship) const
{
    if (!source || !target) {
        return false;
    }

    // Check if the relationship is safe based on node types and permissions
    if (relationship == u"imports"_s || relationship == u"calls"_s || relationship == u"references"_s) {
        // Allow most relationships between code files
        return true;
    } else if (relationship == u"contains"_s) {
        // Directory containment is always safe
        return true;
    } else if (relationship == u"extends"_s) {
        // Inheritance relationships are safe for object-oriented languages
        return true;
    }

    return false;
}

}

namespace KateAi
{

QString ProjectGraph::generateNodeId(const QString &path, const QString &type)
{
    // Generate a unique ID for a node based on its path and type
    // Use a hash of the full path to avoid collisions (e.g., src/foo.cpp vs src_foo.cpp)
    // Use full SHA256 hash (64 hex chars) to eliminate collision risk
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(path.toUtf8());
    QString id = QString::fromLatin1(hash.result().toHex());
    if (!type.isEmpty()) {
        id += u"_"_s + type;
    }
    return id;
}

bool ProjectGraph::isBinaryFile(const QString &path)
{
    // Extension first: a known binary extension answers without reading, and
    // reading a 4 GiB database or a video to sniff its first bytes is waste.
    static const QSet<QString> binaryExtensions = {
        u"o"_s, u"so"_s, u"a"_s, u"dll"_s, u"exe"_s, u"bin"_s, u"obj"_s, u"lib"_s,
        u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"ico"_s, u"bmp"_s, u"webp"_s,
        u"pdf"_s, u"zip"_s, u"tar"_s, u"gz"_s, u"xz"_s, u"bz2"_s, u"7z"_s,
        u"class"_s, u"jar"_s, u"pyc"_s, u"wasm"_s, u"db"_s, u"sqlite"_s,
        u"mp3"_s, u"mp4"_s, u"avi"_s, u"mov"_s, u"wav"_s, u"woff"_s, u"woff2"_s, u"ttf"_s};
    if (binaryExtensions.contains(QFileInfo(path).suffix().toLower())) {
        return true;
    }
    // Then content: a NUL byte anywhere in the head is the practical test,
    // because every text format we analyse stays printable there.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray head = file.read(1024);
    return head.contains('\0');
}

QString ProjectGraph::extractContentPreview(const QString &content, int maxLength)
{
    const int budget = qMax(0, maxLength);
    // Headers and blank lines say nothing a preview needs, and the project
    // graph is quoted back into the system prompt where space is scarce.
    const QString flattened = content.simplified();
    if (budget <= 0) {
        return QString();
    }
    if (flattened.size() <= budget) {
        return flattened;
    }
    return flattened.left(budget) + u"\u2026"_s;
}

QString ProjectGraph::getNodeTypeFromPath(const QString &path)
{
    // Determine node type based on file extension
    QFileInfo info(path);
    QString ext = info.suffix().toLower();

    if (ext == QStringLiteral("cpp") || ext == QStringLiteral("cxx") || ext == QStringLiteral("cc") || ext == QStringLiteral("c")) {
        return u"cpp"_s;
    } else if (ext == QStringLiteral("h") || ext == QStringLiteral("hpp") || ext == QStringLiteral("hxx")) {
        return u"header"_s;
    } else if (ext == QStringLiteral("py")) {
        return u"python"_s;
    } else if (ext == QStringLiteral("js")) {
        return u"javascript"_s;
    } else if (ext == QStringLiteral("json")) {
        return u"json"_s;
    } else if (ext == QStringLiteral("md")) {
        return u"markdown"_s;
    } else if (ext == QStringLiteral("txt")) {
        return u"text"_s;
    } else if (ext == QStringLiteral("java")) {
        return u"java"_s;
    } else if (ext == QStringLiteral("php")) {
        return u"php"_s;
    } else if (ext == QStringLiteral("rb")) {
        return u"ruby"_s;
    } else if (ext == QStringLiteral("go")) {
        return u"go"_s;
    } else if (ext == QStringLiteral("rs")) {
        return u"rust"_s;
    } else if (ext == QStringLiteral("html") || ext == QStringLiteral("htm")) {
        return u"html"_s;
    } else if (ext == QStringLiteral("css")) {
        return u"css"_s;
    } else if (ext == QStringLiteral("xml")) {
        return u"xml"_s;
    } else if (ext == QStringLiteral("sql")) {
        return u"sql"_s;
    } else if (ext == QStringLiteral("sh")) {
        return u"shell"_s;
    } else if (ext == QStringLiteral("yml") || ext == QStringLiteral("yaml")) {
        return u"yaml"_s;
    } else if (ext == QStringLiteral("toml")) {
        return u"toml"_s;
    } else if (ext == QStringLiteral("ini")) {
        return u"ini"_s;
    } else if (ext == QStringLiteral("cfg") || ext == QStringLiteral("conf")) {
        return u"config"_s;
    } else if (ext == QStringLiteral("bat") || ext == QStringLiteral("cmd")) {
        return u"batch"_s;
    } else if (ext == QStringLiteral("pl")) {
        return u"perl"_s;
    } else if (ext == QStringLiteral("r")) {
        return u"r"_s;
    } else if (ext == QStringLiteral("m")) {
        return u"matlab"_s;
    } else if (ext == QStringLiteral("scala")) {
        return u"scala"_s;
    } else if (ext == QStringLiteral("kt")) {
        return u"kotlin"_s;
    } else if (ext == QStringLiteral("swift")) {
        return u"swift"_s;
    } else if (ext == QStringLiteral("ts")) {
        return u"typescript"_s;
    } else if (ext == QStringLiteral("tsx")) {
        return u"tsx"_s;
    } else if (ext == QStringLiteral("jsx")) {
        return u"jsx"_s;
    } else if (ext == QStringLiteral("vue")) {
        return u"vue"_s;
    } else if (ext == QStringLiteral("svelte")) {
        return u"svelte"_s;
    } else if (ext == QStringLiteral("astro")) {
        return u"astro"_s;
    } else if (ext == QStringLiteral("solid")) {
        return u"solid"_s;
    } else if (ext == QStringLiteral("qml")) {
        return u"qml"_s;
    } else {
        return u"unknown"_s;
    }
}

}


