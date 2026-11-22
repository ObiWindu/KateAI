/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace KateAi
{

enum class MemoryScope {
    Managed,
    User,
    Project,
    Nested,
    Local,
};

struct MemoryFile {
    QString path;
    QString displayPath;
    MemoryScope scope = MemoryScope::Project;
    QString content;
    qint64 bytes = 0;
    QString hash;
    int priority = 0;
    // Frontmatter `paths:` globs. Empty means "always, when alwaysApply is not
    // false". A path-scoped file is lazy: it loads when a tool touches a
    // matching path.
    QStringList paths;
    bool alwaysApply = true;
    QString description;
    bool truncated = false;
};

/**
 * Scoped, deduplicated project instructions.
 *
 * Discovery follows the ecosystem conventions (AGENTS.md, CLAUDE.md,
 * KATEAI.md, .kateai/rules, .claude/rules, .agents/rules) and adds the managed
 * and user scopes. Files are read once per loader, path-scoped files load only
 * when a path matches, and identical content (a CLAUDE.md symlink or copy of
 * AGENTS.md) is included once.
 */
class MemoryLoader
{
public:
    struct Config {
        int maxFileBytes = 32 * 1024;
        int maxTotalBytes = 128 * 1024;
        int maxFiles = 32;
    };

    explicit MemoryLoader(const QString &workspace = QString());
    MemoryLoader(const QString &workspace, const Config &config);

    void setWorkspace(const QString &workspace);
    QString workspace() const
    {
        return m_workspace;
    }

    // Loads project/user/managed files plus nested files for the touched paths.
    // Calling it repeatedly with more paths is cheap: already-loaded files are
    // not re-read.
    QList<MemoryFile> load(const QStringList &touchedPaths = QStringList());

    // Everything loaded so far, in precedence order (managed, user, project,
    // nested by depth, local last).
    QList<MemoryFile> files() const
    {
        return m_files;
    }

    // Resets the lazy-loaded set without rediscovering the workspace roots.
    void reset();

    static QList<MemoryFile> discover(const QString &workspace,
                                      const QStringList &touchedPaths = QStringList());
    static QList<MemoryFile> discover(const QString &workspace,
                                      const QStringList &touchedPaths,
                                      const Config &config);

    // Pure path filter; used by tests and by the prompt builder when a tool
    // touched a path after the prompt was assembled.
    static bool matchesPath(const QStringList &globs, const QString &path);
    static QString render(const QList<MemoryFile> &files);

    // Keeps the labels stable for tests and reports.
    static QString scopeId(MemoryScope scope);
    static QString scopeLabel(MemoryScope scope);

    // Applies the budget and returns a copy with truncation markers. Public so
    // the memory manager can preview what the model will see.
    static QList<MemoryFile> applyBudget(QList<MemoryFile> files, const Config &config);

    static void setHomeForTests(const QString &home);

private:
    void discoverRoots();
    void loadNestedFor(const QString &relativePath);
    bool addFile(const QString &path, MemoryScope scope, const QString &displayPath);
    bool alreadyLoaded(const QString &canonicalPath, const QString &hash) const;

    QString m_workspace;
    Config m_config;
    QList<MemoryFile> m_files;
    QStringList m_rootPaths;
    QStringList m_loadedPaths;
    QSet<QString> m_loadedHashes;
    QSet<QString> m_nestedDirs;
    bool m_rootsDiscovered = false;
};

} // namespace KateAi
