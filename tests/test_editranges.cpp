/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "editranges.h"

#include <KTextEditor/Document>
#include <KTextEditor/Editor>

#include <QApplication>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

/**
 * The hover-hint half of the agent-edit feature: unified diff hunks to line
 * ranges, and the MovingCursor anchoring that keeps a hint pointing at the
 * lines it was recorded for while the user edits above them.
 *
 * Uses a real KTextEditor document (offscreen), because MovingCursors are
 * what makes the ranges survive edits and only the document can move them.
 */
class TestEditRanges : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();

    void diffParsesAddedLineRanges();
    void diffHandlesCountForms();
    void diffIgnoresRemovalOnlyHunks();
    void diffIgnoresGarbage();
    void rangesAnswerPendingAndSettled();
    void rangesFollowUserEdits();
    void rangesFollowDeletions();
    void nullDocumentClearsPath();
    void clearingDropsEverything();

private:
    KTextEditor::Document *m_document = nullptr;
};

void TestEditRanges::initTestCase()
{
    auto *editor = KTextEditor::Editor::instance();
    QVERIFY2(editor, "no editor instance available");
    m_document = editor->createDocument(nullptr);
    QVERIFY2(m_document, "no document could be created");
}

void TestEditRanges::cleanup()
{
    m_document->setText(u"l0\nl1\nl2\nl3\nl4\n"_s);
}

void TestEditRanges::diffParsesAddedLineRanges()
{
    const QString diff = u"--- a/file\n+++ b/file\n"
                          "@@ -1,3 +1,4 @@\n"
                          " context\n"
                          "+added one\n"
                          "+added two\n"
                          " context\n"
                          "@@ -10,2 +11,1 @@\n"
                          "-removed\n"
                          " context\n"_s;
    // The mapping is header-based: the new-file side of each hunk says which
    // lines the hunk covers, which for the review hint is the region to mark.
    const QList<KTextEditor::Range> ranges = addedLineRangesFromDiff(diff);
    QCOMPARE(ranges.size(), 2);
    QCOMPARE(ranges.first(), KTextEditor::Range(0, 0, 4, 0));
    QCOMPARE(ranges.at(1), KTextEditor::Range(10, 0, 11, 0));
}

void TestEditRanges::diffHandlesCountForms()
{
    // "@@ -1 +1 @@" (count omitted = one line) and "+9,2" (two added lines).
    const QString diff = u"@@ -1 +1 @@\n+one\n@@ -8,5 +9,2 @@\n+a\n+b\n"_s;
    const QList<KTextEditor::Range> ranges = addedLineRangesFromDiff(diff);
    QCOMPARE(ranges.size(), 2);
    QCOMPARE(ranges.at(0), KTextEditor::Range(0, 0, 1, 0));
    QCOMPARE(ranges.at(1), KTextEditor::Range(8, 0, 10, 0));
}

void TestEditRanges::diffIgnoresRemovalOnlyHunks()
{
    const QString diff = u"@@ -5,3 +6,0 @@\n-gone\n-gone too\n"_s;
    QVERIFY(addedLineRangesFromDiff(diff).isEmpty());
}

void TestEditRanges::diffIgnoresGarbage()
{
    QVERIFY(addedLineRangesFromDiff(u"not a diff at all\n"_s).isEmpty());
    QVERIFY(addedLineRangesFromDiff(QString()).isEmpty());
}

void TestEditRanges::rangesAnswerPendingAndSettled()
{
    EditRangeTracker tracker;
    const QString path = u"/tmp/ranges.cpp"_s;
    // Lines 1-2 pending, line 4 settled.
    tracker.setRanges(path, m_document, {KTextEditor::Range(1, 0, 3, 0)},
                      {KTextEditor::Range(4, 0, 5, 0)});

    int pendingCount = 0;
    QVERIFY(tracker.lineInPendingRange(path, 1, &pendingCount));
    QCOMPARE(pendingCount, 1);
    QVERIFY(tracker.lineInPendingRange(path, 2));
    QVERIFY(!tracker.lineInPendingRange(path, 3)); // end anchor sits at column 0
    QVERIFY(!tracker.lineInPendingRange(path, 0));

    QVERIFY(tracker.lineInSettledRange(path, 4));
    QVERIFY(!tracker.lineInSettledRange(path, 1));
    QVERIFY(!tracker.lineInSettledRange(u"/tmp/other.cpp"_s, 1));
}

void TestEditRanges::rangesFollowUserEdits()
{
    m_document->setText(u"l0\nl1\nl2\nl3\nl4\n"_s);
    EditRangeTracker tracker;
    const QString path = u"/tmp/moving.cpp"_s;
    tracker.setRanges(path, m_document, {KTextEditor::Range(1, 0, 3, 0)}, {});

    QVERIFY(tracker.lineInPendingRange(path, 1));

    // The user types two lines above the range: the hint must follow the
    // edited lines down instead of pointing at whatever now lives there.
    m_document->insertText(KTextEditor::Cursor(0, 0), u"new top\nanother\n"_s);
    QVERIFY(tracker.lineInPendingRange(path, 3));
    QVERIFY(tracker.lineInPendingRange(path, 4));
    QVERIFY(!tracker.lineInPendingRange(path, 1));
}

void TestEditRanges::rangesFollowDeletions()
{
    m_document->setText(u"l0\nl1\nl2\nl3\nl4\n"_s);
    EditRangeTracker tracker;
    const QString path = u"/tmp/deleting.cpp"_s;
    tracker.setRanges(path, m_document, {KTextEditor::Range(3, 0, 5, 0)}, {});

    // Both lines above the range are deleted; the range's anchors move up.
    m_document->removeText(KTextEditor::Range(0, 0, 2, 0));
    QVERIFY(tracker.lineInPendingRange(path, 1));
    QVERIFY(tracker.lineInPendingRange(path, 2));
}

void TestEditRanges::nullDocumentClearsPath()
{
    EditRangeTracker tracker;
    tracker.setRanges(u"/tmp/gone.cpp"_s, nullptr, {KTextEditor::Range(0, 0, 1, 0)}, {});
    QVERIFY(!tracker.lineInPendingRange(u"/tmp/gone.cpp"_s, 0));
}

void TestEditRanges::clearingDropsEverything()
{
    EditRangeTracker tracker;
    const QString path = u"/tmp/clear.cpp"_s;
    tracker.setRanges(path, m_document, {KTextEditor::Range(0, 0, 1, 0)}, {KTextEditor::Range(2, 0, 3, 0)});
    QVERIFY(tracker.lineInPendingRange(path, 0));
    tracker.clear();
    QVERIFY(!tracker.lineInPendingRange(path, 0));
    QVERIFY(!tracker.lineInSettledRange(path, 2));
}

QTEST_MAIN(TestEditRanges)
#include "test_editranges.moc"
