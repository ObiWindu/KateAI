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
#include <QLabel>
#include <QPushButton>
#include <QTest>

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
        // cards the dock exists to frame.
        const QString dock = ChatTheme::intentDock();
        QVERIFY(dock.contains(u"QWidget#intentDock"_s));
        QVERIFY(dock.contains(u"background: transparent"_s));
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
};

QTEST_MAIN(TestIntentDock)
#include "test_intentdock.moc"
