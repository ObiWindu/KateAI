#include "edittracker.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTest>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

/**
 * Covers the review queue's state machine: which edits are pending, what a
 * rejection restores, and what undo puts back.
 *
 * The interesting cases are the ones that used to be wrong. The queue was keyed
 * by path in a QHash, so two edits to the same file in one turn overwrote each
 * other and only the last survived; rejecting then restored the content from
 * before the *last* edit, leaving the file holding the result of an edit the user
 * had just said no to. And a file the agent created was "rejected" by writing an
 * empty string back, leaving a zero-byte file where there had been nothing.
 */
class TestEditTracker : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSingleEditPending()
    {
        EditTracker tracker;
        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(2, 1), u"old"_s, u"new"_s);

        QCOMPARE(tracker.pendingEditCount(), 1);
        QCOMPARE(tracker.pendingPaths(), QStringList{u"/tmp/a.cpp"_s});
        QVERIFY(tracker.hasPendingEdits());
    }

    void testEmptyQueueReportsNothingPending()
    {
        EditTracker tracker;
        QVERIFY(!tracker.hasPendingEdits());
        QCOMPARE(tracker.pendingEditCount(), 0);
        QVERIFY(tracker.pendingPaths().isEmpty());
        QVERIFY(!tracker.canUndo());
    }

    void testEditsToSamePathAccumulate()
    {
        EditTracker tracker;
        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"v1"_s, u"v2"_s);
        tracker.addEdit(u"/tmp/a.cpp"_s, u"edit_file"_s, diffFor(1, 1), u"v2"_s, u"v3"_s);

        // Both edits are still queued. The old path-keyed map kept only one.
        QCOMPARE(tracker.pendingEditCount(), 2);
        // One row, because the row is per file.
        QCOMPARE(tracker.pendingPaths().size(), 1);

        const QList<EditEntry> pending = tracker.pendingEdits();
        QCOMPARE(pending.size(), 2);
        QCOMPARE(pending.at(0).newContent, u"v2"_s);
        QCOMPARE(pending.at(1).newContent, u"v3"_s);
    }

    void testRejectRestoresContentFromBeforeTheFirstPendingEdit()
    {
        EditTracker tracker;
        QSignalSpy rejected(&tracker, &EditTracker::editRejected);

        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"original"_s, u"second"_s);
        tracker.addEdit(u"/tmp/a.cpp"_s, u"edit_file"_s, diffFor(1, 1), u"second"_s, u"third"_s);

        tracker.rejectEdit(u"/tmp/a.cpp"_s);

        QCOMPARE(rejected.count(), 1);
        QCOMPARE(rejected.at(0).at(0).toString(), u"/tmp/a.cpp"_s);
        // The whole point: "original", not "second". Restoring "second" would
        // leave the file holding the result of an edit that was just rejected.
        QCOMPARE(rejected.at(0).at(2).toString(), u"original"_s);
        QVERIFY(!tracker.hasPendingEdits());
    }

    void testRejectCreatedFileAsksForDeletion()
    {
        EditTracker tracker;
        QSignalSpy rejected(&tracker, &EditTracker::editRejected);
        QSignalSpy deleted(&tracker, &EditTracker::fileCreatedThenRejected);

        // createdFile = true, so the pre-edit content is the empty string.
        tracker.addEdit(u"/tmp/new.cpp"_s, u"write_file"_s, diffFor(5, 0), QString(), u"contents"_s, true);

        tracker.rejectEdit(u"/tmp/new.cpp"_s);

        // Deleting, not writing "" back: a zero-byte file is debris the user
        // never asked for.
        QCOMPARE(deleted.count(), 1);
        QCOMPARE(deleted.at(0).at(0).toString(), u"/tmp/new.cpp"_s);
        QCOMPARE(rejected.count(), 0);
    }

    void testAcceptEmitsNewContentAndClearsQueue()
    {
        EditTracker tracker;
        QSignalSpy accepted(&tracker, &EditTracker::editAccepted);

        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"old"_s, u"new"_s);
        tracker.acceptEdit(u"/tmp/a.cpp"_s);

        QCOMPARE(accepted.count(), 1);
        QCOMPARE(accepted.at(0).at(2).toString(), u"new"_s);
        QVERIFY(!tracker.hasPendingEdits());
    }

    void testRejectsAreIndependentPerFile()
    {
        EditTracker tracker;
        QSignalSpy rejected(&tracker, &EditTracker::editRejected);

        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"a-old"_s, u"a-new"_s);
        tracker.addEdit(u"/tmp/b.cpp"_s, u"write_file"_s, diffFor(1, 0), u"b-old"_s, u"b-new"_s);

        tracker.rejectEdit(u"/tmp/a.cpp"_s);

        // Rejecting one file must leave the other queued: the file was edited
        // independently and the user only answered for one of them.
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(rejected.at(0).at(0).toString(), u"/tmp/a.cpp"_s);
        QCOMPARE(tracker.pendingPaths(), QStringList{u"/tmp/b.cpp"_s});
    }

    void testKeepAllAndRejectAllCoverEveryPath()
    {
        EditTracker tracker;
        QSignalSpy accepted(&tracker, &EditTracker::editAccepted);
        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"a"_s, u"A"_s);
        tracker.addEdit(u"/tmp/b.cpp"_s, u"write_file"_s, diffFor(1, 0), u"b"_s, u"B"_s);
        tracker.addEdit(u"/tmp/c.cpp"_s, u"edit_file"_s, diffFor(1, 0), u"c"_s, u"C"_s);

        tracker.acceptAll();
        QCOMPARE(accepted.count(), 3);
        QVERIFY(!tracker.hasPendingEdits());

        EditTracker other;
        QSignalSpy rejected(&other, &EditTracker::editRejected);
        other.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"a"_s, u"A"_s);
        other.addEdit(u"/tmp/b.cpp"_s, u"write_file"_s, diffFor(1, 0), u"b"_s, u"B"_s);

        other.rejectAll();
        QCOMPARE(rejected.count(), 2);
        QVERIFY(!other.hasPendingEdits());
    }

    void testUndoReinstatesLastDecision()
    {
        EditTracker tracker;
        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"original"_s, u"changed"_s);

        tracker.acceptEdit(u"/tmp/a.cpp"_s);
        QVERIFY(tracker.canUndo());

        tracker.undoLast();

        // The edit is back in the queue, not silently dropped.
        QCOMPARE(tracker.pendingEditCount(), 1);
        QCOMPARE(tracker.pendingPaths(), QStringList{u"/tmp/a.cpp"_s});
        QVERIFY(!tracker.canUndo());
    }

    void testUndoOfAcceptationRestoresTheFile()
    {
        EditTracker tracker;
        QSignalSpy rejected(&tracker, &EditTracker::editRejected);

        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"original"_s, u"changed"_s);
        tracker.acceptEdit(u"/tmp/a.cpp"_s);
        rejected.clear();

        tracker.undoLast();

        // Undoing a keep has to undo the file too, otherwise "undo" leaves the
        // change on disk while the queue says it is pending again.
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(rejected.at(0).at(2).toString(), u"original"_s);
    }

    void testNewEditsClearTheUndoHistory()
    {
        EditTracker tracker;
        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"original"_s, u"changed"_s);
        tracker.acceptEdit(u"/tmp/a.cpp"_s);
        QVERIFY(tracker.canUndo());

        // A new edit invalidates the state the earlier decision was made against,
        // so undo must not reach back past it.
        tracker.addEdit(u"/tmp/b.cpp"_s, u"write_file"_s, diffFor(1, 0), u"other"_s, u"OTHER"_s);
        QVERIFY(!tracker.canUndo());
    }

    void testClearDropsEverythingPending()
    {
        EditTracker tracker;
        QSignalSpy changed(&tracker, &EditTracker::editsChanged);

        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"a"_s, u"A"_s);
        tracker.addEdit(u"/tmp/b.cpp"_s, u"write_file"_s, diffFor(1, 0), u"b"_s, u"B"_s);
        changed.clear();

        tracker.clear();

        QVERIFY(!tracker.hasPendingEdits());
        QCOMPARE(tracker.pendingEditCount(), 0);
        QVERIFY(changed.count() > 0);
        QCOMPARE(changed.last().at(0).toBool(), false);
    }

    void testEditsKeepArrivalOrder()
    {
        EditTracker tracker;
        tracker.addEdit(u"/tmp/z.cpp"_s, u"write_file"_s, diffFor(1, 0), u"z"_s, u"Z"_s);
        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"a"_s, u"A"_s);
        tracker.addEdit(u"/tmp/m.cpp"_s, u"write_file"_s, diffFor(1, 0), u"m"_s, u"M"_s);

        // The container used to be a QHash, which guarantees no such thing. The
        // rows are user-facing, so they have to read in the order the agent
        // actually made them.
        QCOMPARE(tracker.pendingPaths(),
                 (QStringList{u"/tmp/z.cpp"_s, u"/tmp/a.cpp"_s, u"/tmp/m.cpp"_s}));
    }

    void testEmptyPathIsIgnored()
    {
        EditTracker tracker;
        tracker.addEdit(QString(), u"write_file"_s, diffFor(1, 0), QString(), QString());
        QVERIFY(!tracker.hasPendingEdits());
    }

private:
    // A minimal unified diff with `added` '+' lines and `removed` '-' lines, so the
    // +/- counters the rows show have something real to count.
    static QString diffFor(int added, int removed)
    {
        QString diff = u"--- a/f\n+++ b/f\n@@ -1 +1 @@\n"_s;
        for (int i = 0; i < removed; ++i) {
            diff += u"-old line\n"_s;
        }
        for (int i = 0; i < added; ++i) {
            diff += u"+new line\n"_s;
        }
        return diff;
    }
};

} // namespace KateAi

// QApplication, not QTEST_MAIN's QGuiApplication: EditTracker is a QWidget.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    KateAi::TestEditTracker tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "test_edittracker.moc"
