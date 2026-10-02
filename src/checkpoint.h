/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
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

    QString m_workspace;
    bool m_enabled = true;
    int m_retention = 20;
    mutable bool m_gitChecked = false;
    mutable bool m_gitAvailable = false;
    mutable QString m_unavailableReason;
};

} // namespace KateAi