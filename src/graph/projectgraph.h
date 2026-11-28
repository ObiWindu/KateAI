/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
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
    int complexity = 0;      // Cyclomatic complexity for functions/classes
    int linesOfCode = 0;
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

// One top-level definition of a file, with its 1-based line range. endLine
// is 0 when the extent could not be determined. Stored in the file node's
// metadata under "symbols" so it serializes with the graph.
struct GraphSymbol {
    QString name;
    QString kind; // "class", "function"
    int line = 0;
    int column = 0;
    int endLine = 0;
};

class ProjectGraph {
public:
    ProjectGraph();
    ~ProjectGraph();
    ProjectGraph(const ProjectGraph &) = delete;
    ProjectGraph &operator=(const ProjectGraph &) = delete;

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
    void updateSecurityStatus();
    void generateSummaryReport() const;

    // Symbol indexing: extracts the definitions of one file with their line
    // ranges and stores them in the file node's metadata ("symbols").
    QList<GraphSymbol> extractSymbols(const QString &content, const QString &filePath) const;
    void indexFileSymbols(GraphNode *fileNode);
    // The definitions recorded for a file, empty when none were indexed.
    QList<GraphSymbol> symbolsInFile(const QString &filePath) const;
    // The definition enclosing a 1-based line: the one with the greatest
    // start line <= line whose range still covers it. Falls back to the
    // greatest start line <= line when no range covers it. Returns a symbol
    // with an empty name when the file has no indexed symbols.
    GraphSymbol enclosingSymbol(const QString &filePath, int line) const;
    // Number of recorded nodes whose file/directory timestamp no longer
    // matches the disk (or whose entry is gone): the graph cache is stale.
    int staleFileCount() const;

    // --- LLM/ACP presentation ------------------------------------------------
    // A path relative to the workspace, used in prompts and query results.
    // Hash ids are stable but unreadable, so agents should see paths first.
    QString displayPath(const QString &path) const;
    QString nodeLabel(const GraphNode *node) const;
    QString nodeSymbols(const GraphNode *node) const;
    GraphNode *findNodeByPath(const QString &path);
    const GraphNode *findNodeByPath(const QString &path) const;
    // Compact graph description for a system prompt or ACP session notes.
    // level follows contextCompressionLevel: 0 full, 1 summary, 2 minimal,
    // 3 counts only. maxNodes/maxEdges bound the output.
    QString contextSummary(int maxNodes, int maxEdges, int level) const;

    // Serialization
    enum class SqliteLoadError {
        None,
        Io,
        InvalidSchema,
        Corrupt,
    };
    bool saveToFile(const QString &filePath) const;
    bool loadFromFile(const QString &filePath);
    bool saveToSqlite(const QString &filePath) const;
    bool loadFromSqlite(const QString &filePath, SqliteLoadError *error = nullptr);
    bool saveToDatabase(const QString &filePath) const { return saveToSqlite(filePath); }
    bool loadFromDatabase(const QString &filePath) { return loadFromSqlite(filePath); }
    bool saveToDb(const QString &filePath) const { return saveToSqlite(filePath); }
    bool loadFromDb(const QString &filePath) { return loadFromSqlite(filePath); }
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &json);

    // Utility
    static QString generateNodeId(const QString &path, const QString &type = QStringLiteral("file"));
    static QString getNodeTypeFromPath(const QString &path);
    static bool isBinaryFile(const QString &path);
    static bool pathWithinRoot(const QString &path, const QString &root);
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
    // O(deg) adjacency indexes referencing the edges owned in m_edges.
    QHash<QString, QList<GraphEdge*>> m_outgoingEdges;
    QHash<QString, QList<GraphEdge*>> m_incomingEdges;
    // Forward adjacency (source -> dependencies) was maintained by hand beside
    // m_edges, so every dependency string was retained twice for the life of
    // the graph. It is now built from m_edges on demand and dropped whenever the
    // edges change: only the traversal queries need it, and they are rare
    // compared with a scan that touches every edge.
    mutable QHash<QString, QStringList> m_adjacencyList;
    mutable bool m_adjacencyValid = false;
    const QHash<QString, QStringList> &adjacencyList() const;
    QString m_workspacePath;
    // Directories already visited during the current scan, keyed by canonical
    // path, so a symlink loop cannot re-enter a directory it resolved from.
    QSet<QString> m_visitedDirs;
    // Import statements collected during the scan, keyed by file node id.
    // Resolving an import needs every node to exist, but the file text needed
    // to extract it is large. Keeping the compact specifiers lets the scan
    // release each file's text immediately instead of holding the whole
    // workspace in memory until the dependency pass runs. Cleared once that
    // pass has consumed it.
    QHash<QString, QStringList> m_pendingImports;
    qint64 m_lastUpdateTime;
    bool m_isDirty;

    void takeStateFrom(ProjectGraph &other);
    void unindexEdge(GraphEdge *edge);

    void cleanupDeletedNodes();
    void updateNodeMetrics(GraphNode *node);
    // Both counters live here so the scan and the update path agree on what a
    // line and a definition are; they used to be computed two different ways.
    static int lineCount(const QString &content);
    static int definitionCount(ProjectGraph &analyser, const QString &content);
    // Resolve the import statements in `content` to existing file-node ids.
    // Raw import syntax is useful for display but cannot answer "what depends
    // on this file"; edges and dependency sets must point at node ids.
    QSet<QString> resolveImportTargets(const QString &content, const QString &sourcePath) const;
    // The two halves of resolveImportTargets(), split so the scan can extract
    // the compact specifiers from a file's text and drop the text, then
    // resolve them after every node exists.
    static QStringList extractImportSpecs(const QString &content);
    QSet<QString> resolveImportSpecs(const QStringList &specs, const QString &sourcePath) const;
    void reindexFileDependencies(GraphNode *node);
    // Cuts the analysed text back to a bounded preview once the analysis has
    // run, so the graph does not hold every file of the workspace in memory.
    void trimContentPreviews();
    const GraphNode *findNodeByName(const QString &name) const;
    // Node lookup by path, including the canonical-path fallback used when a
    // caller and the scanner spell a symlinked prefix differently.
    const GraphNode *findFileNode(const QString &filePath) const;
    bool isSafeRelationship(const GraphNode *source, const GraphNode *target, const QString &relationship) const;

    // Helper functions for graph operations
    void scanDirectory(const QString &dirPath, const QString &parentNodeId);
    QString getDirectoryName(const QString &path) const;
    qint64 getFileLastModified(const QString &path) const;
    QString getFileContent(const QString &path) const;
    bool isFileReadable(const QString &path) const;
};

} // namespace KateAi