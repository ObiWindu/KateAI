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
    // \s+ around the numbers: some producers emit tabs.
    static const QRegularExpression hunkHeader(
        u"^@@\\s+-(\\d+)(?:,(\\d+))?\\s+\\+(\\d+)(?:,(\\d+))?\\s+@@"_s,
        QRegularExpression::MultilineOption);

    QRegularExpressionMatchIterator it = hunkHeader.globalMatch(diff);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int startLine = match.captured(3).toInt() - 1; // 0-based
        // An omitted count means exactly one line; a zero count means the
        // hunk only removed or kept context. Empty (non-participating group)
        // also means omitted: hasCaptured() is true for "" on some Qt
        // versions, which previously skipped single-line additions.
        const int addedCount = match.captured(4).isEmpty() ? 1 : match.captured(4).toInt();
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
    // Clamp to the document: a range past the last line anchors nowhere.
    const int lastLine = document->lines();
    anchored.reserve(ranges.size());
    for (const KTextEditor::Range &range : ranges) {
        const int startLine = qBound(0, range.start().line(), qMax(0, lastLine - 1));
        const int endLine = qBound(startLine, range.end().line(), qMax(startLine, lastLine));
        AnchoredRange entry;
        // StayOnInsert for the start: an insertion at the anchor position
        // must not push the start down into the inserted text. MoveOnInsert
        // for the end so trailing inserts extend the range.
        entry.start = document->newMovingCursor(KTextEditor::Cursor(startLine, 0),
                                                KTextEditor::MovingCursor::StayOnInsert);
        entry.end = document->newMovingCursor(KTextEditor::Cursor(endLine, 0),
                                              KTextEditor::MovingCursor::MoveOnInsert);
        anchored.append(entry);
    }
    return anchored;
}

void EditRangeTracker::freeRanges(QList<AnchoredRange> &ranges)
{
    // MovingCursors are heap objects the caller must delete; the document
    // only tracks them while alive.
    for (AnchoredRange &range : ranges) {
        delete range.start;
        range.start = nullptr;
        delete range.end;
        range.end = nullptr;
    }
    ranges.clear();
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
    // Always report the count, 0 on a miss: callers must not read a stale value.
    if (count) {
        *count = matches;
    }
    return found;
}

void EditRangeTracker::setRanges(const QString &path, KTextEditor::Document *document,
                                 const QList<KTextEditor::Range> &pending,
                                 const QList<KTextEditor::Range> &settled)
{
    // Free the previous cursors for this path: they leak otherwise.
    auto old = m_paths.find(path);
    if (old != m_paths.end()) {
        freeRanges(old->pending);
        freeRanges(old->settled);
        m_paths.erase(old);
    }
    if (!document || (pending.isEmpty() && settled.isEmpty())) {
        return;
    }
    PathRanges ranges;
    ranges.document = document;
    ranges.pending = anchor(document, pending);
    ranges.settled = anchor(document, settled);
    m_paths.insert(path, ranges);
    // The document going away must drop (and null) the cursors: a QPointer
    // nulls itself, but the MovingCursors would dangle.
    connect(document, &QObject::destroyed, this, [this, path]() {
        auto it = m_paths.find(path);
        if (it != m_paths.end()) {
            // The cursors died with the document; null them without delete.
            for (AnchoredRange &range : it->pending) {
                range.start = nullptr;
                range.end = nullptr;
            }
            for (AnchoredRange &range : it->settled) {
                range.start = nullptr;
                range.end = nullptr;
            }
            m_paths.erase(it);
        }
    });
}

void EditRangeTracker::clear()
{
    for (auto it = m_paths.begin(); it != m_paths.end(); ++it) {
        freeRanges(it->pending);
        freeRanges(it->settled);
    }
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
