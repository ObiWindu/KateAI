#pragma once

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStack>
#include <QQueue>
#include <algorithm>
#include <cmath>

namespace KateAi
{

struct GraphNode {
    QString id;
    QString name;
    QString type; // "file", "directory", "class", "function", "variable"
    QString path;
    QString content; // For files, the content preview
    QSet<QString> dependencies; // Other nodes this depends on
    QSet<QString> dependents; // Nodes that depend on this
    int complexity; // Cyclomatic complexity for functions/classes
    int linesOfCode;
    QJsonObject metadata;
    bool isReadOnly; // For security tracking
    qint64 lastModified;
};

struct GraphEdge {
    QString sourceId;
    QString targetId;
    QString relationship; // "imports", "calls", "extends", "contains", "references"
    QString details;
    bool isSafe; // For permission evaluation
};

class ProjectGraph {
public:
    ProjectGraph();
    ~ProjectGraph();

    // Graph generation
    void generateGraph(const QString &workspacePath);
    void updateGraph(const QString &filePath, const QString &content);
    void removeNode(const QString &nodeId);
    void clear();

    // Node operations
    void addNode(const GraphNode &node);
    GraphNode* getNode(const QString &nodeId);
    const GraphNode* getNode(const QString &nodeId) const;
    QList<GraphNode*> getAllNodes() const;
    QList<GraphNode*> getNodesByType(const QString &type) const;
    QList<GraphNode*> getFilesInDirectory(const QString &dirPath) const;

    // Edge operations
    void addEdge(const GraphEdge &edge);
    void removeEdge(const QString &sourceId, const QString &targetId);
    QList<GraphEdge*> getEdgesFrom(const QString &sourceId) const;
    QList<GraphEdge*> getEdgesTo(const QString &targetId) const;
    QList<GraphEdge*> getEdgesByRelationship(const QString &relationship) const;

    // Analysis and queries
    QList<GraphNode*> findRelatedNodes(const QString &nodeId, int maxDepth = 2) const;
    QList<QString> findImportPaths(const QString &sourceNodeId, const QString &targetNodeId) const;
    QSet<QString> getTransitiveDependencies(const QString &nodeId) const;
    QSet<QString> getTransitiveDependents(const QString &nodeId) const;
    QList<GraphNode*> getDependencyChain(const QString &startNodeId, const QString &endNodeId) const;

    // Project analysis
    void analyzeFileDependencies();
    void analyzeCodeStructure();
    void updateSecurityStatus();
    void generateSummaryReport() const;

    // Serialization
    bool saveToFile(const QString &filePath) const;
    bool loadFromFile(const QString &filePath);
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &json);

    // Utility
    static QString generateNodeId(const QString &path, const QString &type = QStringLiteral("file"));
    static QString getNodeTypeFromPath(const QString &path);
    static bool isBinaryFile(const QString &path);
    static QString extractContentPreview(const QString &content, int maxLength);

    // Getters
    QString getWorkspacePath() const { return m_workspacePath; }
    qint64 getLastUpdateTime() const { return m_lastUpdateTime; }
    int getNodeCount() const { return m_nodes.size(); }
    int getEdgeCount() const { return m_edges.size(); }
    const QMap<QString, GraphNode*>& getNodes() const { return m_nodes; }
    const QMap<QString, GraphEdge*>& getEdges() const { return m_edges; }

    // Setters
    void setWorkspacePath(const QString &path) { m_workspacePath = path; }

private:
    QMap<QString, GraphNode*> m_nodes;
    QMap<QString, GraphEdge*> m_edges;
    QMap<QString, QSet<QString>> m_adjacencyList;
    QString m_workspacePath;
    qint64 m_lastUpdateTime;
    bool m_isDirty;

    void buildAdjacencyList();
    void cleanupDeletedNodes();
    void updateNodeMetrics(GraphNode *node);
    QString extractImportsFromContent(const QString &content, const QString &filePath) const;
    QString extractClassDefinitions(const QString &content) const;
    QString extractFunctionDefinitions(const QString &content) const;
    bool isSafeRelationship(const GraphNode *source, const GraphNode *target, const QString &relationship) const;

    // Helper functions for graph operations
    void scanDirectory(const QString &dirPath, const QString &parentNodeId);
    QString getDirectoryName(const QString &path) const;
    qint64 getFileLastModified(const QString &path) const;
    QString getFileContent(const QString &path) const;
    bool isFileReadable(const QString &path) const;
};

} // namespace KateAi