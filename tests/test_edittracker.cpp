#include "edittracker.h"

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <algorithm>
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

    void testSecondEditToACreatedFileStillDeletesOnReject()
    {
        EditTracker tracker;
        QSignalSpy rejected(&tracker, &EditTracker::editRejected);
        QSignalSpy deleted(&tracker, &EditTracker::fileCreatedThenRejected);

        // The first write creates the file, so there is no pre-edit content to
        // read and ChatWidget passes createdFile = true.
        tracker.addEdit(u"/tmp/new.cpp"_s, u"write_file"_s, diffFor(5, 0), QString(), u"contents"_s, true);
        // The agent edits that file again. By toolStarted it exists on disk, so
        // the read succeeds and ChatWidget passes createdFile = false.
        tracker.addEdit(u"/tmp/new.cpp"_s, u"edit_file"_s, diffFor(6, 1), u"contents"_s, u"contents2"_s, false);

        tracker.rejectEdit(u"/tmp/new.cpp"_s);

        // The file is still one the agent made up: rejecting deletes it. Were
        // the second edit allowed to clear the flag, this would restore a null
        // baseline and leave a zero-byte file behind.
        QCOMPARE(deleted.count(), 1);
        QCOMPARE(deleted.at(0).at(0).toString(), u"/tmp/new.cpp"_s);
        QCOMPARE(rejected.count(), 0);
    }

    void testRepeatedWritesToAPreExistingFileRestoreInsteadOfDelete()
    {
        EditTracker tracker;
        QSignalSpy rejected(&tracker, &EditTracker::editRejected);
        QSignalSpy deleted(&tracker, &EditTracker::fileCreatedThenRejected);

        // write_file over an existing file: it exists before and after, so it is
        // never "created" and rejecting restores the original content.
        tracker.addEdit(u"/tmp/old.cpp"_s, u"write_file"_s, diffFor(5, 0), u"original"_s, u"contents"_s, false);
        tracker.addEdit(u"/tmp/old.cpp"_s, u"write_file"_s, diffFor(6, 1), u"contents"_s, u"contents2"_s, false);

        tracker.rejectEdit(u"/tmp/old.cpp"_s);

        QCOMPARE(rejected.count(), 1);
        QCOMPARE(rejected.at(0).at(2).toString(), u"original"_s);
        QCOMPARE(deleted.count(), 0);
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

    // --- Reading a review ---------------------------------------------------
    //
    // Reviewing used to open a window of the plugin's own, with the diff
    // rendered into a QTextBrowser and its own Keep/Reject pair alongside the
    // ones on the row. It now writes a .diff file and hands it to the editor,
    // so the change is read the same way every other diff in the IDE is read.

    static QPushButton *buttonWithText(const QWidget &widget, const QString &text)
    {
        const auto buttons = widget.findChildren<QPushButton *>();
        for (QPushButton *button : buttons) {
            if (button->text() == text) {
                return button;
            }
        }
        return nullptr;
    }

    void testReviewHandsADiffFileToTheEditor()
    {
        EditTracker tracker;
        tracker.resize(600, 300);
        tracker.show();
        tracker.addEdit(u"/tmp/agentloop.cpp"_s, u"edit_file"_s, diffFor(2, 1), u"old"_s, u"new"_s);
        QVERIFY(QTest::qWaitForWindowExposed(&tracker));

        QPushButton *review = buttonWithText(tracker, u"Review"_s);
        QVERIFY2(review, "the review row has no Review button");

        QSignalSpy requested(&tracker, &EditTracker::previewDiffRequested);
        QVERIFY(requested.isValid());
        QTest::mouseClick(review, Qt::LeftButton);

        QCOMPARE(requested.size(), 1);
        const QString previewPath = requested.takeFirst().at(0).toString();
        // A .diff in the temp directory: the editor colours it as a diff, and
        // nothing is dropped where the agent's own tools would find it.
        QVERIFY2(previewPath.endsWith(u".diff"_s), qPrintable(previewPath));
        QVERIFY2(!previewPath.contains(u"/tmp/agentloop.cpp"_s), qPrintable(previewPath));
        QVERIFY2(previewPath.contains(u"agentloop"_s), qPrintable(previewPath));

        QFile preview(previewPath);
        QVERIFY2(preview.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(previewPath));
        const QString written = QString::fromUtf8(preview.readAll());
        QVERIFY2(written.contains(u"+new line"_s), qPrintable(written));
        QVERIFY2(written.contains(u"-old line"_s), qPrintable(written));
        // Every pending edit for the file is in the one comparison.
        tracker.addEdit(u"/tmp/agentloop.cpp"_s, u"write_file"_s, diffFor(3, 3), u"new"_s, u"newer"_s);
        QTest::mouseClick(buttonWithText(tracker, u"Review"_s), Qt::LeftButton);
        QFile again(previewPath);
        QVERIFY(again.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString rewritten = QString::fromUtf8(again.readAll());
        QVERIFY2(rewritten.count(u"+new line"_s) >= 2, qPrintable(rewritten));
        QVERIFY(rewritten.size() > written.size());
    }

    // The same file must not get a second window stacked behind the first, and
    // a review must not appear in a window at all any more.
    void testReviewOpensNoWindowOfItsOwn()
    {
        EditTracker tracker;
        tracker.resize(600, 300);
        tracker.show();
        tracker.addEdit(u"/tmp/agentloop.cpp"_s, u"edit_file"_s, diffFor(1, 0), u"old"_s, u"new"_s);
        QVERIFY(QTest::qWaitForWindowExposed(&tracker));

        QTest::mouseClick(buttonWithText(tracker, u"Review"_s), Qt::LeftButton);
        QTest::qWait(20);
        QVERIFY2(tracker.findChildren<QDialog *>().isEmpty(),
                 "the review preview is a document in the editor, not a window");
    }

    void testPreviewPathsAreDistinctPerFile()
    {
        EditTracker tracker;
        tracker.resize(600, 300);
        // Two files that differ only by directory: the preview names would be
        // identical if the path were not part of them.
        tracker.addEdit(u"/tmp/one/agentloop.cpp"_s, u"edit_file"_s, diffFor(1, 0), u"a"_s, u"b"_s);
        tracker.addEdit(u"/tmp/two/agentloop.cpp"_s, u"edit_file"_s, diffFor(1, 0), u"c"_s, u"d"_s);
        tracker.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tracker));

        QSignalSpy requested(&tracker, &EditTracker::previewDiffRequested);
        QVERIFY(requested.isValid());
        const auto buttons = tracker.findChildren<QPushButton *>();
        QCOMPARE(std::count_if(buttons.begin(), buttons.end(), [](QPushButton *b) {
            return b->text() == u"Review"_s;
        }), 2);

        // One row per file, each with its own Review button.
        for (QPushButton *review : buttons) {
            if (review->text() == u"Review"_s) {
                QTest::mouseClick(review, Qt::LeftButton);
            }
        }

        QCOMPARE(requested.size(), 2);
        const QString first = requested.at(0).at(0).toString();
        const QString second = requested.at(1).at(0).toString();
        QVERIFY2(first != second, qPrintable(QStringLiteral("%1 and %2 collide").arg(first, second)));
    }

    // The tracker is a compact review control, and its type was raised by
    // another 15% so it stays legible next to the transcript.
    void testTrackerTypeIsSixtyNinePercentOfTheChat()
    {
        EditTracker tracker;
        tracker.addEdit(u"/tmp/a.cpp"_s, u"write_file"_s, diffFor(1, 0), u"old"_s, u"new"_s);
        tracker.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tracker));

        QLabel *count = tracker.findChild<QLabel *>();
        QVERIFY2(count, "no label on the tracker bar");
        // A 12px label at 69% is 8.28px; at the previous 60% it was 7.2px.
        QVERIFY2(count->styleSheet().contains(QStringLiteral("8.28px")), qPrintable(count->styleSheet()));
        const QList<QPushButton *> buttons = tracker.findChildren<QPushButton *>();
        QVERIFY(!buttons.isEmpty());
        bool sawScaledButton = false;
        for (QPushButton *button : buttons) {
            if (button->styleSheet().contains(QStringLiteral("8.28px"))) {
                sawScaledButton = true;
            }
            // The buttons hug their text: the wide fixed paddings are gone.
            QVERIFY2(!button->styleSheet().contains(QStringLiteral("padding: 0 14px"))
                         && !button->styleSheet().contains(QStringLiteral("padding: 0 12px"))
                         && !button->styleSheet().contains(QStringLiteral("padding: 0 10px")),
                     qPrintable(button->styleSheet()));
        }
        QVERIFY2(sawScaledButton, "the tracker's buttons kept the old size");
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
