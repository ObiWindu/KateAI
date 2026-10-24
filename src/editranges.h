/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <KTextEditor/Document>
#include <KTextEditor/Range>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

namespace KTextEditor
{
class MovingCursor;
}

namespace KateAi
{

// Unified-diff hunk headers ("@@ -a,b +c,d @@") to the ranges of added lines
// in the new file, 0-based. A hunk with no added lines contributes nothing;
// a hunk header without a count means exactly one line.
QList<KTextEditor::Range> addedLineRangesFromDiff(const QString &diff);

/**
 * The line ranges the agent edited in each document, anchored with
 * MovingCursors so they survive the user's own edits: a hint that pointed at
 * line 42 must follow line 42 down when five lines are inserted above it.
 *
 * Ranges are pushed in from the review queue (pending) and from the last
 * settled turn (kept for the session), per file path.
 */
class EditRangeTracker : public QObject
{
    Q_OBJECT

public:
    explicit EditRangeTracker(QObject *parent = nullptr);
    ~EditRangeTracker() override;

    // Replaces the ranges of one path. The document is the open KTextEditor
    // document for the path; a null document clears the path (the file is not
    // open, so there is nothing to anchor or hover over).
    void setRanges(const QString &path, KTextEditor::Document *document,
                   const QList<KTextEditor::Range> &pending,
                   const QList<KTextEditor::Range> &settled);
    void clear();

    // True when the line sits inside a pending range of the path; writes the
    // number of pending ranges into pendingCount.
    bool lineInPendingRange(const QString &path, int line, int *pendingCount = nullptr) const;
    // True when the line sits inside a settled range of the path.
    bool lineInSettledRange(const QString &path, int line) const;

private:
    struct AnchoredRange {
        KTextEditor::MovingCursor *start = nullptr;
        KTextEditor::MovingCursor *end = nullptr;
    };
    struct PathRanges {
        QPointer<KTextEditor::Document> document;
        QList<AnchoredRange> pending;
        QList<AnchoredRange> settled;
    };

    static QList<AnchoredRange> anchor(KTextEditor::Document *document,
                                       const QList<KTextEditor::Range> &ranges);
    static bool lineInRanges(const QList<AnchoredRange> &ranges, int line, int *count = nullptr);

    QHash<QString, PathRanges> m_paths;
};

} // namespace KateAi
