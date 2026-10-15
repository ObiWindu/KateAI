/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Guards the properties the suggestion card is built around:
 *
 *  1. It shows what the agent suggested and where it points.
 *  2. Accept and Dismiss both report the card's id, and Accept carries the
 *     anchor so the view can open it.
 *  3. A suggestion that never arrived cannot be accepted, and one replayed
 *     from an earlier turn is a record rather than an open question.
 */

#include "chattheme.h"
#include "suggestionwidget.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTextBrowser>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

static QPushButton *buttonWithText(const QWidget &widget, const QString &text)
{
    const QList<QPushButton *> buttons = widget.findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

static QLabel *labelWithText(const QWidget &widget, const QString &needle)
{
    const QList<QLabel *> labels = widget.findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (label->text().contains(needle, Qt::CaseInsensitive)) {
            return label;
        }
    }
    return nullptr;
}

// The state glyph, as opposed to the fixed lightbulb badge beside it.
static QLabel *glyphLabel(const QWidget &widget)
{
    const QList<QLabel *> labels = widget.findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (label->styleSheet().contains(QStringLiteral("font-size: 12px"))) {
            return label;
        }
    }
    return nullptr;
}

class TestSuggestionWidget : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCardShowsSuggestionAndAnchor()
    {
        SuggestionWidget widget(u"sugg-1"_s);
        widget.setSuggestion(u"Use std::string_view here instead of const std::string &"_s, u"src/main.cpp"_s, 42);
        // isVisible() only means anything for a widget that has been shown.
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        const QList<QTextBrowser *> bodies = widget.findChildren<QTextBrowser *>();
        QCOMPARE(bodies.size(), 1);
        QVERIFY2(bodies.first()->toPlainText().contains(u"string_view"_s), qPrintable(bodies.first()->toPlainText()));

        QVERIFY2(labelWithText(widget, u"src/main.cpp:42"_s), "the anchor label should read path:line");
        QVERIFY2(labelWithText(widget, u"posted"_s), "a fresh card is posted, not decided");

        // An open suggestion is one the user can act on.
        QVERIFY(buttonWithText(widget, u"Accept"_s)->isVisible());
        QVERIFY(buttonWithText(widget, u"Dismiss"_s)->isVisible());
    }

    void testAcceptReportsIdAndAnchor()
    {
        SuggestionWidget widget(u"sugg-2"_s);
        widget.setSuggestion(u"Rewrite this loop with erase/remove_if"_s, u"src/tools.cpp"_s, 1180);

        QSignalSpy accepted(&widget, &SuggestionWidget::accepted);
        QVERIFY(accepted.isValid());

        QTest::mouseClick(buttonWithText(widget, u"Accept"_s), Qt::LeftButton);

        QCOMPARE(accepted.size(), 1);
        const QList<QVariant> args = accepted.takeFirst();
        QCOMPARE(args.at(0).toString(), u"sugg-2"_s);
        QCOMPARE(args.at(1).toString(), u"src/tools.cpp"_s);
        QCOMPARE(args.at(2).toInt(), 1180);
    }

    void testDismissReportsId()
    {
        SuggestionWidget widget(u"sugg-3"_s);
        widget.setSuggestion(u"Consider inverting this condition"_s, QString(), 0);

        // No anchor: the header must not invent one.
        QVERIFY(!labelWithText(widget, u":"_s));

        QSignalSpy dismissed(&widget, &SuggestionWidget::dismissed);
        QVERIFY(dismissed.isValid());

        QTest::mouseClick(buttonWithText(widget, u"Dismiss"_s), Qt::LeftButton);

        QCOMPARE(dismissed.size(), 1);
        QCOMPARE(dismissed.takeFirst().at(0).toString(), u"sugg-3"_s);
    }

    void testFailedSuggestionCannotBeAccepted()
    {
        SuggestionWidget widget(u"sugg-4"_s);
        widget.setError(u"suggestion must not be empty"_s);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        const QList<QTextBrowser *> bodies = widget.findChildren<QTextBrowser *>();
        QVERIFY(!bodies.isEmpty());
        QVERIFY(bodies.first()->toPlainText().contains(u"must not be empty"_s));
        QVERIFY2(labelWithText(widget, u"failed"_s), "a failed suggestion is marked as such");

        // Advice that never arrived is not a decision the user can make.
        QVERIFY(!buttonWithText(widget, u"Accept"_s)->isVisible());
        QVERIFY(!buttonWithText(widget, u"Dismiss"_s)->isVisible());
    }

    void testReplayedCardIsARecord()
    {
        SuggestionWidget widget(u"sugg-5"_s);
        widget.setSuggestion(u"Extract this into a helper"_s, u"src/acpclient.cpp"_s, 7);
        widget.markDecided();
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        // Restored from history: the decision belongs to the turn that is over.
        QVERIFY(!buttonWithText(widget, u"Accept"_s)->isVisible());
        QVERIFY(!buttonWithText(widget, u"Dismiss"_s)->isVisible());
        QVERIFY2(labelWithText(widget, u"earlier turn"_s), "a replayed card says where it came from");
    }

    // --- Live while its tool runs ---------------------------------------------
    //
    // The advice is in hand the moment the agent writes it, so the card is
    // posted at toolStarted rather than only once the call has reported back.
    // It then carries a live state until the result settles it.

    void testCardIsLiveWhileItsToolRuns()
    {
        SuggestionWidget widget(u"sugg-6"_s);
        widget.setSuggestion(u"Hoist this allocation out of the loop"_s, u"src/agentloop.cpp"_s, 2100);
        widget.setRunning();
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        // The body is already there: the user is not shown an empty card for
        // the length of the call.
        const QList<QTextBrowser *> bodies = widget.findChildren<QTextBrowser *>();
        QVERIFY2(!bodies.isEmpty(), qPrintable(QStringLiteral("no body on a live card")));
        QVERIFY2(bodies.first()->toPlainText().contains(u"Hoist this allocation"_s),
                 qPrintable(bodies.first()->toPlainText()));
        QVERIFY2(labelWithText(widget, u"posting"_s), "a live card must say it is still in flight");
        QVERIFY2(widget.styleSheet().contains(ChatTheme::accent()),
                 qPrintable(widget.styleSheet()));
        // The glyph is the live one, not the settled tick.
        QLabel *glyph = glyphLabel(widget);
        QVERIFY2(glyph, "no state glyph on a live card");
        QVERIFY2(glyph->text() == QString(QChar(0x25D0)), qPrintable(glyph->text()));
    }

    void testLiveCardSettlesIntoPosted()
    {
        SuggestionWidget widget(u"sugg-7"_s);
        widget.setSuggestion(u"Use a QHash here"_s, u"src/mcp.cpp"_s, 12);
        widget.setRunning();
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        // The tool reported back: the card is a posted card now, not a live one.
        widget.setSuggestion(u"Use a QHash here"_s, u"src/mcp.cpp"_s, 12);
        QVERIFY2(labelWithText(widget, u"posted"_s), "a settled card must read as posted");
        QVERIFY2(!labelWithText(widget, u"posting"_s), "the live state outlived its tool");
        QVERIFY(buttonWithText(widget, u"Accept"_s)->isVisible());
    }

    void testLiveCardBecomesAnErrorWhenTheToolFails()
    {
        SuggestionWidget widget(u"sugg-8"_s);
        widget.setSuggestion(u"Trim this trailing whitespace"_s, u"src/tools.cpp"_s, 4);
        widget.setRunning();

        // A suggestion that never arrived must not be left on screen as advice.
        widget.setError(u"suggestion must not be empty"_s);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QVERIFY2(labelWithText(widget, u"failed"_s), "a failed call must be reported as such");
        QVERIFY2(!labelWithText(widget, u"posting"_s), "the live state outlived its tool");
        QVERIFY(!buttonWithText(widget, u"Accept"_s)->isVisible());
        QVERIFY(!buttonWithText(widget, u"Dismiss"_s)->isVisible());
    }

    void testErrorBeatsRunning()
    {
        // The two states can race when a card is settled twice; a failure must
        // never be relabelled as still in flight.
        SuggestionWidget widget(u"sugg-9"_s);
        widget.setSuggestion(u"Split this function"_s, QString(), 0);
        widget.setError(u"the tool crashed"_s);
        widget.setRunning();
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        QVERIFY2(labelWithText(widget, u"failed"_s), "an error was overwritten by the running state");
    }
};

QTEST_MAIN(TestSuggestionWidget)
#include "test_suggestionwidget.moc"
