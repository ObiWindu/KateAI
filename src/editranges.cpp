/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "editranges.h"

#include <KTextEditor/Document>
#include <KTextEditor/MovingCursor>

#include <QRegularExpression>
#include <QUrl>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QList<KTextEditor::Range> addedLineRangesFromDiff(const QString &diff)
{
    QList<KTextEditor::Range> ranges;
    // "@@ -a,b +c,d @@" — the new-file side says where the added lines live.
    static const QRegularExpression hunkHeader(
        u"^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@"_s,
        QRegularExpression::MultilineOption);

    QRegularExpressionMatchIterator it = hunkHeader.globalMatch(diff);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int startLine = match.captured(3).toInt() - 1; // 0-based
        // An omitted count means exactly one line; a zero count means the
        // hunk only removed or kept context.
        const int addedCount = match.hasCaptured(4) ? match.captured(4).toInt() : 1;
        if (startLine < 0 || addedCount <= 0) {
            continue;
        }
        ranges.append(KTextEditor::Range(startLine, 0, startLine + addedCount, 0));
    }
    return ranges;
}

EditRangeTracker::EditRangeTracker(QObject *parent)
    : QObject(parent)
{
}

EditRangeTracker::~EditRangeTracker()
{
    clear();
}

QList<EditRangeTracker::AnchoredRange>
EditRangeTracker::anchor(KTextEditor::Document *document, const QList<KTextEditor::Range> &ranges)
{
    QList<AnchoredRange> anchored;
    if (!document) {
        return anchored;
    }
    anchored.reserve(ranges.size());
    for (const KTextEditor::Range &range : ranges) {
        AnchoredRange entry;
        // MoveOnInsert keeps the anchor under its own line when text is
        // inserted at the anchor position: an insertion at column 0 of the
        // hunk's first line moves the anchor down to what the user inserted,
        // which is the conservative choice for a hint that says "around here".
        entry.start = document->newMovingCursor(range.start(), KTextEditor::MovingCursor::MoveOnInsert);
        entry.end = document->newMovingCursor(range.end(), KTextEditor::MovingCursor::MoveOnInsert);
        anchored.append(entry);
    }
    return anchored;
}

bool EditRangeTracker::lineInRanges(const QList<AnchoredRange> &ranges, int line, int *count)
{
    bool found = false;
    int matches = 0;
    for (const AnchoredRange &range : ranges) {
        if (!range.start || !range.end) {
            continue;
        }
        // A MovingCursor at (line, 0) end-of-range means "the line before this
        // one is the last edited one"; the line itself is only inside when the
        // end anchor moved into it by user edits.
        const KTextEditor::Cursor start = range.start->toCursor();
        const KTextEditor::Cursor end = range.end->toCursor();
        if (line >= start.line() && (line < end.line() || (line == end.line() && end.column() > 0))) {
            found = true;
            ++matches;
        }
    }
    if (count && found) {
        *count = matches;
    }
    return found;
}

void EditRangeTracker::setRanges(const QString &path, KTextEditor::Document *document,
                                 const QList<KTextEditor::Range> &pending,
                                 const QList<KTextEditor::Range> &settled)
{
    m_paths.remove(path);
    if (!document || (pending.isEmpty() && settled.isEmpty())) {
        return;
    }
    PathRanges ranges;
    ranges.document = document;
    ranges.pending = anchor(document, pending);
    ranges.settled = anchor(document, settled);
    m_paths.insert(path, ranges);
}

void EditRangeTracker::clear()
{
    // The MovingCursors are owned by their documents; dropping the hash is
    // enough, the document deletes its moving cursors with itself.
    m_paths.clear();
}

bool EditRangeTracker::lineInPendingRange(const QString &path, int line, int *pendingCount) const
{
    const auto it = m_paths.constFind(path);
    if (it == m_paths.constEnd()) {
        return false;
    }
    int count = 0;
    if (!lineInRanges(it.value().pending, line, &count)) {
        return false;
    }
    if (pendingCount) {
        *pendingCount = count;
    }
    return true;
}

bool EditRangeTracker::lineInSettledRange(const QString &path, int line) const
{
    const auto it = m_paths.constFind(path);
    if (it == m_paths.constEnd()) {
        return false;
    }
    return lineInRanges(it.value().settled, line);
}

} // namespace KateAi
