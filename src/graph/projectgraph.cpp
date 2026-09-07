#include "projectgraph.h"

#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStack>
#include <QQueue>
#include <algorithm>
#include <cmath>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

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

    m_lastUpdateTime = QDateTime::currentSecsSinceEpoch();
    m_isDirty = false;
}

void ProjectGraph::scanDirectory(const QString &dirPath, const QString &parentNodeId)
{
    QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);

    for (const QFileInfo &entry : entries) {
        GraphNode node;
        node.id = generateNodeId(entry.absoluteFilePath(), entry.isDir() ? u"directory"_s : u"file"_s);
        node.name = entry.fileName();
        node.type = entry.isDir() ? u"directory"_s : getNodeTypeFromPath(entry.absoluteFilePath());
        node.path = entry.absoluteFilePath();
        node.isReadOnly = false;
        node.lastModified = entry.lastModified().toSecsSinceEpoch();

        if (entry.isDir()) {
            // Recursively scan subdirectories
            scanDirectory(entry.absoluteFilePath(), node.id);
        } else {
            // Read file content for analysis
            QFile file(entry.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                node.content = QString::fromUtf8(file.readAll());
            }

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

    if (node) {
        // Update node content and metrics
        node->content = content;
        updateNodeMetrics(node);

        // Update dependencies based on new content
        QSet<QString> oldDependencies = node->dependencies;
        QStringList importLines = extractImportsFromContent(content, filePath).split(u"\n"_s, Qt::SkipEmptyParts);
        QSet<QString> newDependencies;
        for (const QString &line : importLines) {
            if (!line.isEmpty()) {
                newDependencies.insert(line);
            }
        }

        // Remove old dependency edges
        for (const QString &oldDep : oldDependencies) {
            if (!newDependencies.contains(oldDep)) {
                removeEdge(nodeId, oldDep);
            }
        }

        // Add new dependency edges
        for (const QString &newDep : newDependencies) {
            if (!oldDependencies.contains(newDep)) {
                GraphEdge edge;
                edge.sourceId = nodeId;
                edge.targetId = newDep;
                edge.relationship = u"imports"_s;
                edge.isSafe = true;
                addEdge(edge);
            }
        }

        node->dependencies = newDependencies;

        // Update dependent nodes
        for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
            GraphNode *dependentNode = it.value();
            if (dependentNode->dependencies.contains(nodeId)) {
                dependentNode->dependents.insert(nodeId);
            }
        }
    } else {
        // Create new node for file
        GraphNode newNode;
        newNode.id = generateNodeId(filePath);
        newNode.name = QFileInfo(filePath).fileName();
        newNode.type = getNodeTypeFromPath(filePath);
        newNode.path = filePath;
        newNode.content = content;
        newNode.isReadOnly = false;
        newNode.lastModified = QDateTime::currentSecsSinceEpoch();
        updateNodeMetrics(&newNode);

        // Find parent directory
        QString parentDirId = generateNodeId(QFileInfo(filePath).absolutePath(), u"directory"_s);
        GraphNode *parentNode = getNode(parentDirId);
        if (parentNode) {
            addNode(newNode);
            GraphEdge edge;
            edge.sourceId = parentDirId;
            edge.targetId = newNode.id;
            edge.relationship = u"contains"_s;
            edge.isSafe = true;
            addEdge(edge);
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
        // Parse edgeKey to get sourceId and targetId
        int arrow1 = edgeKey.indexOf(u"->"_s);
        int arrow2 = edgeKey.indexOf(u"->"_s, arrow1 + 2);
        if (arrow1 != -1 && arrow2 != -1) {
            QString sourceId = edgeKey.mid(0, arrow1);
            QString targetId = edgeKey.mid(arrow1 + 2, arrow2 - arrow1 - 2);
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

    if (!edgeKey.isEmpty()) {
        GraphEdge *edge = m_edges.take(edgeKey);
        delete edge;

        // Update adjacency list
        if (m_adjacencyList.contains(sourceId)) {
            m_adjacencyList[sourceId].remove(targetId);
        }

        // Update node dependencies
        GraphNode *sourceNode = getNode(sourceId);
        GraphNode *targetNode = getNode(targetId);
        if (sourceNode && targetNode) {
            sourceNode->dependencies.remove(targetId);
            targetNode->dependents.remove(sourceId);
        }
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
    QList<QString> result;
    QSet<QString> visited;
    QStack<QPair<QString, QStringList>> stack;

    stack.push({sourceNodeId, {sourceNodeId}});
    visited.insert(sourceNodeId);

    while (!stack.isEmpty()) {
        QPair<QString, QStringList> current = stack.pop();
        QString currentNodeId = current.first;
        QStringList path = current.second;

        if (currentNodeId == targetNodeId) {
            result.append(path.join(u" -> "_s));
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
        QString currentId = queue.dequeue();
        if (m_adjacencyList.contains(currentId)) {
            for (const QString &dependentId : m_adjacencyList[currentId]) {
                if (!visited.contains(dependentId)) {
                    visited.insert(dependentId);
                    result.insert(dependentId);
                    queue.enqueue(dependentId);
                }
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

void ProjectGraph::analyzeFileDependencies()
{
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        GraphNode *node = it.value();
        if (node->type == u"file"_s) {
            QStringList importLines = extractImportsFromContent(node->content, node->path).split(u"\n"_s, Qt::SkipEmptyParts);
            QSet<QString> imports;
            for (const QString &line : importLines) {
                if (!line.isEmpty()) {
                    imports.insert(line);
                }
            }
            node->dependencies = imports;
        }
    }
}

void ProjectGraph::analyzeCodeStructure()
{
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        GraphNode *node = it.value();
        if (node->type == u"file"_s) {
            node->complexity = extractClassDefinitions(node->content).size() + extractFunctionDefinitions(node->content).size();
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

bool ProjectGraph::saveToFile(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QJsonObject json = toJson();
    QJsonDocument doc(json);
    file.write(doc.toJson());

    return true;
}

bool ProjectGraph::loadFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QByteArray data = file.readAll();
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

    // Add nodes
    QJsonArray nodesArray;
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it) {
        const GraphNode *node = it.value();
        QJsonObject nodeJson;

        nodeJson.insert(u"id"_s, node->id);
        nodeJson.insert(u"name"_s, node->name);
        nodeJson.insert(u"type"_s, node->type);
        nodeJson.insert(u"path"_s, node->path);
        nodeJson.insert(u"content"_s, node->content);

        // Add dependencies
        QJsonArray dependenciesArray;
        for (const QString &dep : node->dependencies) {
            dependenciesArray.append(dep);
        }
        nodeJson.insert(u"dependencies"_s, dependenciesArray);

        // Add dependents
        QJsonArray dependentsArray;
        for (const QString &dep : node->dependents) {
            dependentsArray.append(dep);
        }
        nodeJson.insert(u"dependents"_s, dependentsArray);

        nodeJson.insert(u"complexity"_s, node->complexity);
        nodeJson.insert(u"linesOfCode"_s, node->linesOfCode);
        nodeJson.insert(u"isReadOnly"_s, node->isReadOnly);
        nodeJson.insert(u"lastModified"_s, node->lastModified);

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
        // Parse edgeKey to get sourceId and targetId
        int arrow1 = edgeKey.indexOf(u"->"_s);
        int arrow2 = edgeKey.indexOf(u"->"_s, arrow1 + 2);
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

void ProjectGraph::updateNodeMetrics(GraphNode *node)
{
    if (!node) {
        return;
    }

    node->linesOfCode = node->content.split(u"\n"_s).size();

    // Calculate cyclomatic complexity based on control flow statements
    int complexity = 1; // Base complexity
    QRegularExpression ifRegex(u"if|else if|switch|case|for|while|do|catch|except|finally"_s);
    QRegularExpression loopRegex(u"for|while|do"_s);
    QRegularExpression conditionalRegex(u"if|else if|case|catch|except"_s);

    QStringList lines = node->content.split(u"\n"_s);
    for (const QString &line : lines) {
        if (ifRegex.match(line).hasMatch()) {
            complexity++;
        }
        if (loopRegex.match(line).hasMatch()) {
            complexity++;
        }
        if (conditionalRegex.match(line).hasMatch()) {
            complexity++;
        }
    }

    node->complexity = complexity;
}

QString ProjectGraph::extractImportsFromContent(const QString &content, const QString &filePath) const
{
    Q_UNUSED(filePath);
    QString imports;

    // Extract C++ includes
    QRegularExpression cppIncludeRegex(u"#[ \\t]*include[ \\t]*[<\"][^<>]+[>\"]"_s);
    QRegularExpressionMatchIterator it = cppIncludeRegex.globalMatch(content);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        imports += match.captured(0) + u"\n"_s;
    }

    // Extract Python imports
    QRegularExpression pythonImportRegex(u"^(import|from)\\s+\\w+(?:\\.\\w+)*"_s);
    it = pythonImportRegex.globalMatch(content);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        imports += match.captured(0) + u"\\n"_s;
    }

    // Extract JavaScript imports
    QRegularExpression jsImportRegex(u"^(import|require)\\s+[\\'\\\"][^\\'\\\"]+[\\'\\\"]"_s);
    it = jsImportRegex.globalMatch(content);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        imports += match.captured(0) + u"\\n"_s;
    }

    return imports.trimmed();
}

QString ProjectGraph::extractClassDefinitions(const QString &content) const
{
    // Extract class definitions from C++ and Java
    QRegularExpression classRegex(u"class\\s+\\w+\\s*:\\s*public\\s+\\w+"_s);
    QRegularExpressionMatchIterator it = classRegex.globalMatch(content);
    QStringList matches;
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        matches.append(match.captured(0));
    }
    return matches.join(u"\\n"_s);
}

QString ProjectGraph::extractFunctionDefinitions(const QString &content) const
{
    // Extract function definitions
    QRegularExpression functionRegex(u"\\w+\\s+\\w+\\s*\\([^)]*\\)\\s*\\{?"_s);
    QRegularExpressionMatchIterator it = functionRegex.globalMatch(content);
    QStringList matches;
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        matches.append(match.captured(0));
    }
    return matches.join(u"\\n"_s);
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
    QString id = path;
    id.replace('/', '_');
    id.replace(':', '_');
    id.replace('\\', '_');
    if (!type.isEmpty()) {
        id += u"_"_s + type;
    }
    return id;
}

QString ProjectGraph::getNodeTypeFromPath(const QString &path)
{
    // Determine node type based on file extension
    QFileInfo info(path);
    QString ext = info.suffix().toLower();

    if (ext == "cpp" || ext == "cxx" || ext == "cc" || ext == "c") {
        return u"cpp"_s;
    } else if (ext == "h" || ext == "hpp" || ext == "hxx") {
        return u"header"_s;
    } else if (ext == "py") {
        return u"python"_s;
    } else if (ext == "js" || ext == "jsx" || ext == "ts") {
        return u"javascript"_s;
    } else if (ext == "json") {
        return u"json"_s;
    } else if (ext == "md") {
        return u"markdown"_s;
    } else if (ext == "txt") {
        return u"text"_s;
    } else if (ext == "java") {
        return u"java"_s;
    } else if (ext == "php") {
        return u"php"_s;
    } else if (ext == "rb") {
        return u"ruby"_s;
    } else if (ext == "go") {
        return u"go"_s;
    } else if (ext == "rs") {
        return u"rust"_s;
    } else if (ext == "html" || ext == "htm") {
        return u"html"_s;
    } else if (ext == "css") {
        return u"css"_s;
    } else if (ext == "xml") {
        return u"xml"_s;
    } else if (ext == "sql") {
        return u"sql"_s;
    } else if (ext == "sh") {
        return u"shell"_s;
    } else if (ext == "yml" || ext == "yaml") {
        return u"yaml"_s;
    } else if (ext == "toml") {
        return u"toml"_s;
    } else if (ext == "ini") {
        return u"ini"_s;
    } else if (ext == "cfg" || ext == "conf") {
        return u"config"_s;
    } else if (ext == "bat" || ext == "cmd") {
        return u"batch"_s;
    } else if (ext == "pl") {
        return u"perl"_s;
    } else if (ext == "r") {
        return u"r"_s;
    } else if (ext == "m") {
        return u"matlab"_s;
    } else if (ext == "scala") {
        return u"scala"_s;
    } else if (ext == "kt") {
        return u"kotlin"_s;
    } else if (ext == "swift") {
        return u"swift"_s;
    } else if (ext == "ts") {
        return u"typescript"_s;
    } else if (ext == "tsx") {
        return u"tsx"_s;
    } else if (ext == "jsx") {
        return u"jsx"_s;
    } else if (ext == "vue") {
        return u"vue"_s;
    } else if (ext == "svelte") {
        return u"svelte"_s;
    } else if (ext == "astro") {
        return u"astro"_s;
    } else if (ext == "solid") {
        return u"solid"_s;
    } else if (ext == "qml") {
        return u"qml"_s;
    } else if (ext == "yaml") {
        return u"yaml"_s;
    } else if (ext == "yml") {
        return u"yaml"_s;
    } else if (ext == "toml") {
        return u"toml"_s;
    } else if (ext == "ini") {
        return u"ini"_s;
    } else if (ext == "cfg" || ext == "conf") {
        return u"config"_s;
    } else if (ext == "bat" || ext == "cmd") {
        return u"batch"_s;
    } else if (ext == "pl") {
        return u"perl"_s;
    } else if (ext == "r") {
        return u"r"_s;
    } else if (ext == "m") {
        return u"matlab"_s;
    } else if (ext == "scala") {
        return u"scala"_s;
    } else if (ext == "kt") {
        return u"kotlin"_s;
    } else if (ext == "swift") {
        return u"swift"_s;
    } else if (ext == "ts") {
        return u"typescript"_s;
    } else if (ext == "tsx") {
        return u"tsx"_s;
    } else if (ext == "jsx") {
        return u"jsx"_s;
    } else if (ext == "vue") {
        return u"vue"_s;
    } else if (ext == "svelte") {
        return u"svelte"_s;
    } else if (ext == "astro") {
        return u"astro"_s;
    } else if (ext == "solid") {
        return u"solid"_s;
    } else if (ext == "qml") {
        return u"qml"_s;
    } else if (ext == "yaml") {
        return u"yaml"_s;
    } else if (ext == "yml") {
        return u"yaml"_s;
    } else if (ext == "toml") {
        return u"toml"_s;
    } else if (ext == "ini") {
        return u"ini"_s;
    } else if (ext == "cfg" || ext == "conf") {
        return u"config"_s;
    } else if (ext == "bat" || ext == "cmd") {
        return u"batch"_s;
    } else if (ext == "pl") {
        return u"perl"_s;
    } else if (ext == "r") {
        return u"r"_s;
    } else if (ext == "m") {
        return u"matlab"_s;
    } else if (ext == "scala") {
        return u"scala"_s;
    } else if (ext == "kt") {
        return u"kotlin"_s;
    } else if (ext == "swift") {
        return u"swift"_s;
    } else if (ext == "ts") {
        return u"typescript"_s;
    } else if (ext == "tsx") {
        return u"tsx"_s;
    } else if (ext == "jsx") {
        return u"jsx"_s;
    } else if (ext == "vue") {
        return u"vue"_s;
    } else if (ext == "svelte") {
        return u"svelte"_s;
    } else if (ext == "astro") {
        return u"astro"_s;
    } else if (ext == "solid") {
        return u"solid"_s;
    } else if (ext == "qml") {
        return u"qml"_s;
    } else {
        return u"unknown"_s;
    }
}


