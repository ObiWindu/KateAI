/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Guards the two properties the chat panel's intent dock depends on:
 *
 *  1. A delegated agent task is drawn as a bordered box with its own
 *     background, so it reads as a distinct object rather than as more prose.
 *  2. It is compact -- the dock sits above the input and shares that strip
 *     with approval prompts, so the card has to stay small.
 */

#include "chattheme.h"
#include "subtaskwidget.h"

#include <QApplication>
#include <QColor>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <QTextBrowser>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestIntentDock : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testAgentTaskCardIsABorderedBoxWithBackground()
    {
        SubtaskWidget widget(u"task-1"_s);

        const QString style = widget.styleSheet();
        // A box: a border on the card, not just an accent rail.
        QVERIFY2(style.contains(u"QWidget#subtaskWidget"_s), qPrintable(style));
        QVERIFY2(style.contains(u"border:"_s), qPrintable(style));
        QVERIFY2(style.contains(u"border-radius:"_s), qPrintable(style));
        // And a background of its own, distinct from the panel backdrop.
        QVERIFY2(style.contains(u"background-color:"_s), qPrintable(style));
        QVERIFY2(!style.contains(ChatTheme::panelBg()), qPrintable(style));
    }

    void testAgentTaskCardStaysCompact()
    {
        SubtaskWidget widget(u"task-2"_s);

        const QString style = widget.styleSheet();
        // 10px padding was the old transcript-sized card; the docked variant
        // keeps well under that.
        QVERIFY2(!style.contains(u"10px"_s), qPrintable(style));

        // The header row is the part always on screen, so that is what has to
        // stay small: 11px title, 10px status.
        QVERIFY2(findLabelStyle(widget, QStringLiteral("font-size: 11px; font-weight: 600")),
                 qPrintable(style));
        QVERIFY2(findLabelStyle(widget, QStringLiteral("font-size: 10px;")), qPrintable(style));

        // The Cancel button is the reason the card lives in the dock; it has
        // to stay short enough not to dominate the strip.
        auto *cancel = widget.findChild<QPushButton *>();
        QVERIFY(cancel);
        QVERIFY2(cancel->maximumHeight() <= 22, qPrintable(QString::number(cancel->maximumHeight())));
    }

    void testAgentTaskCardActuallyPaintsItsBackground()
    {
        SubtaskWidget widget(u"paint-1"_s);

        // A plain QWidget subclass silently ignores a stylesheet
        // background-color unless WA_StyledBackground is set. Without this the
        // card renders fully transparent and the box never appears on screen,
        // even though the stylesheet is correct.
        QVERIFY2(widget.testAttribute(Qt::WA_StyledBackground),
                 "sub-agent card needs WA_StyledBackground to paint its box");
    }

    void testAgentTaskCardIsYellow()
    {
        // A delegated sub-agent is someone else's work, so its card is warm and
        // unmistakable next to the agent's own blue / green / violet tool cards.
        const QString style = ChatTheme::agentTaskCard();
        const int at = style.indexOf(u"background-color:"_s);
        QVERIFY2(at >= 0, qPrintable(style));
        const int valueStart = at + qstrlen("background-color:");
        const QString value = style.mid(valueStart, style.indexOf(u";"_s, valueStart) - valueStart).trimmed();
        const QColor colour(value);
        QVERIFY2(colour.isValid(), qPrintable(value));
        const int hue = colour.hue();
        QVERIFY2(hue >= 30 && hue <= 70,
                 qPrintable(QStringLiteral("card colour %1 has hue %2, which is not yellow")
                                .arg(colour.name(), QString::number(hue))));
        QVERIFY2(colour.saturation() > 40,
                 qPrintable(QStringLiteral("card colour %1 is too grey to read as yellow").arg(colour.name())));
    }

    void testThinkingPillPulseKeepsBothPhases()
    {
        // The pulse dims the pill between beats through the stylesheet rather
        // than through a QGraphicsOpacityEffect, which breaks colour-emoji
        // glyphs. Both phases have to exist, differ, and stay bare: the pill
        // is text colour alone, with no background or border to carry the
        // state, and at half its old footprint.
        const QString bright = ChatTheme::thinkingPill();
        const QString dim = ChatTheme::thinkingPillDim();
        QVERIFY2(bright != dim, "the pill needs a dim phase to pulse with");
        for (const QString &sheet : {bright, dim}) {
            QVERIFY2(!sheet.contains(u"background-color"_s), qPrintable(sheet));
            QVERIFY2(!sheet.contains(u"border: 1px"_s), qPrintable(sheet));
            QVERIFY2(sheet.contains(u"font-size: 6px"_s), qPrintable(sheet));
        }
        const QString work = ChatTheme::workingPill();
        const QString workDim = ChatTheme::workingPillDim();
        QVERIFY2(work != workDim, "the working pill needs a dim phase too");
        QVERIFY2(work.contains(u"font-size: 6px"_s), qPrintable(work));
    }

    void testRunningCardShowsTheDelegatedTask()
    {
        SubtaskWidget card(QStringLiteral("task-10"));
        card.startAgent(QStringLiteral("Reviewer"), QStringLiteral("code"),
                        QStringLiteral("Check the diff for the new parser"));

        // The delegated task is what the card is about, so it has to stay on
        // screen while the agent works. startAgent() used to clear it, which
        // left a running card with nothing in it but a timer.
        bool shown = false;
        for (const QTextBrowser *browser : card.findChildren<QTextBrowser *>()) {
            if (browser->isVisibleTo(&card) && browser->toPlainText().contains(u"Check the diff for the new parser")) {
                shown = true;
            }
        }
        QVERIFY2(shown, "a running sub-agent card must still name the work it was given");
    }

    void testRunningCardShowsWhatTheSubAgentIsDoing()
    {
        SubtaskWidget card(QStringLiteral("task-11"));
        card.startAgent(QStringLiteral("Reviewer"), QStringLiteral("code"), QStringLiteral("Check the diff"));

        // Before the sub-agent reports anything there is no step to show.
        QVERIFY2(!hasVisibleLabelContaining(card, u"read_file"_s),
                 "no live step should be on the card before the first one arrives");

        card.appendActivity(QStringLiteral("→ read_file src/parser.cpp"), false);
        QVERIFY2(hasVisibleLabelContaining(card, u"src/parser.cpp"_s),
                 "the step the sub-agent is on has to be visible on the card itself, "
                 "not only in the collapsed activity log");

        // The answer takes over when the run ends.
        card.finishAgent(ToolResult{});
        QVERIFY2(!hasVisibleLabelContaining(card, u"src/parser.cpp"_s),
                 "the finished card must not keep reporting a step from a run that is over");
    }

    void testRunningAgentTaskKeepsStateColourInTheBorder()
    {
        SubtaskWidget running(QStringLiteral("task-3"));
        running.startAgent(QStringLiteral("Reviewer"), QStringLiteral("code"), QStringLiteral("Check the diff"));

        const QString style = running.styleSheet();
        // Outcome is carried by the border colour, so a running task is
        // visually distinct without needing a separate legend.
        QVERIFY2(style.contains(ChatTheme::accent()), qPrintable(style));

        SubtaskWidget finished(QStringLiteral("task-4"));
        finished.startAgent(QStringLiteral("Reviewer"), QStringLiteral("code"), QStringLiteral("Check the diff"));
        finished.finishAgent(ToolResult{});
        QVERIFY2(finished.styleSheet().contains(ChatTheme::success()),
                 qPrintable(finished.styleSheet()));
    }

    void testDockChromeStaysFlat()
    {
        // The dock itself must not draw a box, and must not restyle its
        // children -- a descendant rule would flatten the borders on the very
        // cards the dock exists to frame. It does fill with the window chrome
        // colour: the dock is part of the window, pinned to the composer strip
        // (and under a built-in preset that colour is the panel colour, so the
        // dock still paints nothing of its own).
        const QString dock = ChatTheme::intentDock();
        QVERIFY(dock.contains(u"QWidget#intentDock"_s));
        QVERIFY2(dock.contains(ChatTheme::windowBg()), qPrintable(dock));
        QVERIFY2(!dock.contains(u"QWidget#intentDock QWidget"_s), qPrintable(dock));
        QVERIFY2(!dock.contains(u"border-radius"_s), qPrintable(dock));
    }

    void testApprovalRowIsABox()
    {
        const QString row = ChatTheme::intentApprovalRow();
        QVERIFY2(row.contains(u"QWidget#approvalRow"_s), qPrintable(row));
        QVERIFY2(row.contains(u"background-color:"_s), qPrintable(row));
        QVERIFY2(row.contains(u"border:"_s), qPrintable(row));
    }

private:
    static bool findLabelStyle(const QWidget &root, const QString &needle)
    {
        const auto labels = root.findChildren<QLabel *>();
        for (const QLabel *label : labels) {
            if (label->styleSheet().contains(needle)) {
                return true;
            }
        }
        return false;
    }

    static bool hasVisibleLabelContaining(QWidget &root, const QString &needle)
    {
        const auto labels = root.findChildren<QLabel *>();
        for (const QLabel *label : labels) {
            if (label->isVisibleTo(&root) && label->text().contains(needle)) {
                return true;
            }
        }
        return false;
    }
};

QTEST_MAIN(TestIntentDock)
#include "test_intentdock.moc"
