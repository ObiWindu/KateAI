/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>
#include <QStringList>

namespace KateAi
{

/**
 * Temporary git worktrees for isolated sub-agents.
 *
 * The worktree lives outside the workspace (in the cache directory) so the
 * project graph, searches, and rule discovery never index it. The child edits
 * there; the parent only receives a diff and an explicit Apply/Discard
 * decision. Nothing is merged automatically.
 */
struct WorktreeResult {
    bool ok = false;
    QString path;
    // The commit the detached worktree started from, used as the diff base.
    QString baseCommit;
    QString error;
};

class SubagentWorktree
{
public:
    // Git must exist and the workspace must be inside a repository. A bare
    // repository and a non-git folder are rejected with an actionable error.
    static bool isAvailable(const QString &workspace, QString *reason = nullptr);
    static QString repositoryRoot(const QString &workspace);

    // Creates a detached worktree at HEAD. `taskId` only names the directory;
    // unsafe characters are replaced. `scopeKey` separates runs that may reuse
    // a task id in different conversations or windows; an empty key keeps the
    // legacy per-repository path so a killed run can still be reclaimed.
    static WorktreeResult create(const QString &workspace, const QString &taskId,
                                 const QString &scopeKey = QString());
    static bool remove(const QString &workspace, const QString &worktreePath,
                       QString *error = nullptr);

    // Stages the worktree (so new files appear) and returns a unified diff
    // against the base commit. Never touches the parent tree.
    static QString diff(const QString &worktreePath, const QString &baseCommit,
                        QString *error = nullptr);
    static QStringList changedFiles(const QString &worktreePath, const QString &baseCommit,
                                    QString *error = nullptr);
    // Applies a unified diff to the parent workspace after a --check pass.
    static bool apply(const QString &workspace, const QString &patch,
                      QString *error = nullptr);

    static QString cacheRoot();

    // Tests point this at a temporary directory.
    static void setCacheRootForTests(const QString &path);
};

} // namespace KateAi
