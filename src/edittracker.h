/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <KTextEditor/Range>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QVBoxLayout;
class QWidget;

namespace KateAi
{

struct EditEntry
{
    QString path;
    QString toolName; // "write_file" or "edit_file"
    QString diff;     // Unified diff showing the changes
    QString oldContent;
    QString newContent;
    // Set when the file did not exist before the agent created it. Rejecting
    // such an edit has to remove the file rather than truncate it to empty,
    // which would leave a zero-byte file where there was nothing before.
    bool createdFile = false;
};

/**
 * The review queue shown under the transcript while the agent has unconfirmed
 * file changes, in AcceptEdits mode.
 *
 * In AcceptEdits the agent writes files without stopping to ask, so the panel
 * collects every mutation and lets the user accept or reject it afterwards. That
 * inverts the usual review flow: the interesting comparison is not one diff but
 * "every change this turn, versus the file as it was before the turn started".
 * So entries are grouped by path rather than listed per tool call, and a group
 * is only settled once none of its edits are pending.
 *
 * Rejecting restores the content the file had before the first still-pending
 * edit in that group, not before the last one, so rejecting any subset of a
 * multi-edit file lands on a state that actually existed.
 */
class EditTracker : public QWidget
{
    Q_OBJECT

public:
    explicit EditTracker(QWidget *parent = nullptr);

    void addEdit(const QString &path, const QString &toolName, const QString &diff,
                 const QString &oldContent = QString(), const QString &newContent = QString(),
                 bool createdFile = false);
    // Updates the diff (and old/new content) of an existing group entry when a
    // later message carries them. A new path creates a new group.
    void updateEdit(const QString &path, const QString &toolName, const QString &diff,
                    const QString &oldContent = QString(), const QString &newContent = QString());
    void clear();
    bool hasPendingEdits() const;
    QList<EditEntry> pendingEdits() const;
    // Paths with at least one unresolved edit, in stable sorted order.
    QStringList pendingPaths() const;
    int pendingEditCount() const;

    void acceptAll();
    void rejectAll();

    void acceptEdit(const QString &path);
    void rejectEdit(const QString &path);

    // Line ranges (0-based, in the edited file) the agent's still-pending
    // edits cover, and the ranges of the last settled decision round. Used
    // by the in-editor hover hint; settled ranges are kept for the session
    // and cleared with the queue.
    QList<KTextEditor::Range> pendingEditRanges(const QString &path) const;
    QList<KTextEditor::Range> settledEditRanges(const QString &path) const;
    // Every path with pending or settled ranges, for the caller that pushes
    // them into the editor-side range tracker.
    QStringList rangedPaths() const;
    // Re-points every pending and remembered edit of oldPath at newPath. A
    // Save-As in the editor must carry the review queue along with the file,
    // or accepting and rejecting would settle a path that no longer exists.
    void renamePath(const QString &oldPath, const QString &newPath);

    // Single-step undo for the last accept/reject decision made in this tracker.
    // Cleared by addEdit(), since new edits invalidate what came before.
    bool canUndo() const;
    void undoLast();

    // Re-applies every theme-derived stylesheet and rebuilds the file rows, so
    // a theme change repaints a tracker that is currently showing.
    void applyStyle();

Q_SIGNALS:
    void editAccepted(const QString &path, const QString &toolName, const QString &newContent);
    void editRejected(const QString &path, const QString &toolName, const QString &oldContent);
    // Rejection of the last pending edit for a path that the agent created:
    // the caller has to delete the file, since writing "" back leaves debris.
    void fileCreatedThenRejected(const QString &path);
    void editsChanged(bool hasEdits);
    void statusMessage(const QString &message, bool isError);
    // The path of a .diff file holding this path's pending changes, laid out
    // for the caller to open: the preview is a document in the editor rather
    // than a window of the plugin's own.
    void previewDiffRequested(const QString &previewPath);

private:
    // One row per path, holding every still-unresolved edit to that file.
    struct Group {
        QString path;
        QList<EditEntry> pending;
        bool createdFile = false;
        QString lastKnownContent;
        int added = 0;
        int removed = 0;
    };

    struct Decision {
        enum Kind { Accepted, Rejected } kind = Accepted;
        QString path;
        QString toolName;
        // The edits that were settled, kept so undo can put them back verbatim.
        QList<EditEntry> pending;
        bool createdFile = false;
        // File content as it stood before these edits, so undoing an acceptance
        // can restore it. A null QString means "never read", which is different
        // from "read as empty".
        QString previousContent;
        // The line ranges this decision settled, mirrored in
        // m_settledRanges so undo can retract exactly them.
        QList<KTextEditor::Range> settledRanges;
    };

    void updateUI();
    void setFileListVisible(bool visible);
    void rebuildEditList();
    void appendRow(const Group &group);
    QWidget *createRow(const Group &group);
    // Writes the group's combined diff to a .diff file in the temp directory and
    // asks for it to be opened.
    void previewDiff(const QString &path);
    QString combinedDiff(const Group &group) const;
    QString previewDiffPath(const QString &path) const;
    void settle(const QString &path, bool accepted);
    void pruneGroups();
    Group makeGroup(const QString &path) const;
    void refreshTotals(Group &group) const;
    static int addedLines(const QString &diff);
    static int removedLines(const QString &diff);

    // QHash gives no ordering guarantee, and the list is user-facing: rows have
    // to appear in the order the edits arrived, with keep/reject all acting on
    // every path in that same order.
    QList<Group> m_groups;

    QList<Decision> m_undoStack;

    // Path -> added-line ranges of the decision rounds that settled in this
    // session. The hover hint shows "edited by Kate AI (turn finished)" over
    // these, so a change made from the panel stays discoverable days later.
    QHash<QString, QList<KTextEditor::Range>> m_settledRanges;

    QWidget *m_barWidget = nullptr;
    QLabel *m_countLabel = nullptr;
    QPushButton *m_acceptAllBtn = nullptr;
    QPushButton *m_rejectAllBtn = nullptr;
    QPushButton *m_toggleListBtn = nullptr;

    QWidget *m_listContainer = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QVBoxLayout *m_editListLayout = nullptr;
    bool m_fileListVisible = false;
};

} // namespace KateAi
