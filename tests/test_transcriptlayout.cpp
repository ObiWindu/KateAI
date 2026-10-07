#include "transcriptlayout.h"

#include <QTest>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

/**
 * Covers the two decisions the transcript's anchoring rests on: where a new
 * message is inserted relative to the slack, and whether the viewport follows
 * new content.
 *
 * Both were implicit in the chat widget and both broke when the anchoring was
 * flipped. Under the top-anchored layout the insert index was computed as the
 * stretch index, which put every appended message *below* the slack at the far
 * end of the viewport, while the rest of the widget still scrolled to the
 * maximum as though the newest content were at the top. And the follow test was
 * an inline comparison against a bare 40, duplicated in three lambdas that had
 * already started disagreeing about what to do when the tail came back into view.
 */
class TestTranscriptLayout : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Bottom-anchored: [stretch, msg1, msg2, indicators]. The next message goes
    // after the stretch. ChatWidget uses Top instead, because inserting here
    // repeatedly would put each new turn *above* the previous one.
    void testBottomAnchorInsertsAfterTheStretch()
    {
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Bottom, 0, 3, 4), 1);
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Bottom, 1, -1, 2), 2);
    }

    // Top-anchored: [msg1, msg2, stretch, indicators]. The next message goes
    // before the stretch, so turns stay chronological (oldest at the top) and
    // expanding a card grows downward into the slack.
    void testTopAnchorInsertsBeforeTheStretch()
    {
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Top, 2, 3, 4), 2);
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Top, 0, -1, 1), 0);
    }

    void testInsertNeverPassesTheIndicatorRow()
    {
        // No stretch, indicators last: the message still has to go directly above
        // the status pills. insertWidget() inserts *at* an index, so landing on
        // the indicators index is what puts the message before them.
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Bottom, -1, 2, 3), 2);
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Top, -1, 2, 3), 2);
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Bottom, -1, 0, 1), 0);
    }

    void testIndicatorRowWinsOverTheStretch()
    {
        // Stretch reported after the indicators, which the widget's scan order
        // cannot produce but a caller could: the indicator row still pins last.
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Bottom, 3, 1, 4), 1);
    }

    void testInsertStaysInsideTheLayout()
    {
        // Degenerate inputs still have to produce a usable index rather than one
        // past the end, which insertWidget() would treat as "append".
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Bottom, 0, -1, 1), 1);
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Bottom, 9, -1, 2), 2);
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Top, -5, -5, 3), 3);
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Top, -5, -5, 0), 0);
    }

    void testAppendsLandAtTheEndWhenNothingIsPinned()
    {
        // No stretch and no indicator row: plain append.
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Bottom, -1, -1, 7), 7);
        QCOMPARE(transcriptInsertIndexFor(TranscriptAnchor::Top, -1, -1, 7), 7);
    }

    void testFollowsTailWithinTheThreshold()
    {
        QVERIFY(transcriptShouldFollowTail(500, 520));
        QVERIFY(transcriptShouldFollowTail(0, 0));
        QVERIFY(transcriptShouldFollowTail(100, 100));
        // Exactly at the threshold still counts as following.
        QVERIFY(transcriptShouldFollowTail(480, 520));
    }

    void testStopsFollowingPastTheThreshold()
    {
        QVERIFY(!transcriptShouldFollowTail(479, 520));
        QVERIFY(!transcriptShouldFollowTail(0, 5000));
    }
};

} // namespace KateAi

int main(int argc, char **argv)
{
    // QCoreApplication is enough here: these are pure functions over integers.
    // The widget that calls them needs a QApplication, but that is exercised by
    // the edit tracker test rather than here.
    QCoreApplication app(argc, argv);
    QTEST_SET_MAIN_SOURCE_PATH
    KateAi::TestTranscriptLayout tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_transcriptlayout.moc"
