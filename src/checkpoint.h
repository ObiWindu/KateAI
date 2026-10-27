/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QAtomicInt>
#include <QDateTime>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QString>

namespace KateAi
{

struct CheckpointInfo {
    QString id; // commit hash
    QString label;
    QDateTime createdAt;
    QString shortId;
    int changedFiles = 0;
};

// Snapshots the workspace into a private git repository kept outside the
// project, so a turn can be reviewed and rolled back without touching the
// user's own history.
class CheckpointManager : public QObject
{
    Q_OBJECT

public:
    explicit CheckpointManager(QObject *parent = nullptr);
    ~CheckpointManager() override;

    void setWorkspace(const QString &workspace);
    QString workspace() const
    {
        return m_workspace;
    }
    void setEnabled(bool enabled);
    bool isEnabled() const
    {
        return m_enabled;
    }
    void setRetention(int retention);

    // False when git is missing or the shadow repository cannot be created.
    // Every operation then degrades to a no-op instead of failing the agent.
    bool isAvailable() const;
    QString unavailableReason() const
    {
        return m_unavailableReason;
    }

    QString createCheckpoint(const QString &label, QString *error = nullptr);
    // The same snapshot, but the git commands run in the thread pool so a
    // large first `git add` cannot freeze the UI. The outcome arrives through
    // checkpointCreated() or failed(); there is no return value.
    void createCheckpointAsync(const QString &label);
    QList<CheckpointInfo> checkpoints() const;
    // Writes the checkpoint's file contents back into the workspace.
    bool restoreCheckpoint(const QString &id, QString *error = nullptr);
    // Unified diff between the checkpoint and the current working tree.
    QString diffAgainstCheckpoint(const QString &id, int maxLines = 400) const;
    // Number of files that differ from the checkpoint.
    int changedFileCount(const QString &id) const;

Q_SIGNALS:
    void checkpointCreated(const QString &id, const QString &label);
    void failed(const QString &error);

private:
    QString shadowRoot() const;
    QString gitDir() const;
    bool ensureRepository(QString *error = nullptr) const;
    void writeExcludes() const;
    // Runs git with the shadow repository and workspace bound in.
    bool runGit(const QStringList &arguments, QString *output = nullptr, QString *error = nullptr, int timeoutMs = 20000) const;
    // Stages the worktree so added and deleted files appear in later diffs.
    bool stageWorktree(QString *error = nullptr) const;
    // Sequence number for the next snapshot tag.
    int nextSequence() const;
    // Snapshot commit hashes, newest first.
    QStringList checkpointHashes() const;
    // Snapshot tag refs, newest first.
    QStringList checkpointRefs() const;
    void prune();
    // Runs the housekeeping commands in the background. "git gc" can take
    // seconds, and it must not freeze the editor.
    void runGitDetached(const QStringList &arguments, int timeoutMs = 60000);
    // Removes the shadow repositories of workspaces that no longer exist, and
    // the ones that were not touched for a long time. Without this the cache
    // directory grows with every project that was ever opened.
    void pruneStaleShadowRepositories() const;
    // Mutex for thread-safe checkpoint operations (recursive to allow prune() from createCheckpoint())
    mutable QRecursiveMutex m_checkpointMutex;
    // Snapshots currently running on the global thread pool. The destructor
    // waits for them: a worker holds m_checkpointMutex and must never
    // outlive it (destroying a locked QMutex then touching freed memory
    // crashes, e.g. stopping a turn while its pre-mutation snapshot runs).
    QAtomicInt m_inFlightSnapshots{0};
    // The git commands that run in the background. They are stopped with the
    // manager.
    QList<QProcess *> m_detachedProcesses;

    QString m_workspace;
    bool m_enabled = true;
    int m_retention = 20;
    mutable bool m_gitChecked = false;
    mutable bool m_gitAvailable = false;
    mutable QString m_unavailableReason;
};

} // namespace KateAi