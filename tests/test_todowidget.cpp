/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Guards the properties the todo list is built around:
 *
 *  1. It lists every step with its state glyph and a "done/total" counter.
 *  2. The counter and the per-row states follow the plan as it advances.
 *  3. A finished step reads as done (muted, struck through); the step being
 *     worked on is the accented, pulsing one.
 *  4. An empty plan hides the widget, and a retheme repaints it in place.
 */

#include "todowidget.h"

#include "chattheme.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

QJsonObject step(const QString &id, const QString &description, bool completed, bool inProgress)
{
    QJsonObject o;
    o.insert(u"id"_s, id);
    o.insert(u"description"_s, description);
    o.insert(u"completed"_s, completed);
    o.insert(u"inProgress"_s, inProgress);
    return o;
}

QWidget *rowForDescription(TodoWidget &widget, const QString &description)
{
    const QList<QWidget *> rows = widget.findChildren<QWidget *>(u"todoRow"_s);
    for (QWidget *row : rows) {
        if (auto *text = row->findChild<QLabel *>(u"todoText"_s)) {
            if (text->text() == description) {
                return row;
            }
        }
    }
    return nullptr;
}

} // namespace

class TestTodoWidget : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void normalizesEveryKnownStepVariant()
    {
        // Steps arrive from the ACP layer, the native structured-planning
        // parser and restored sessions, and the spellings differ. The widget
        // accepts every known variant instead of rendering blank rows.
        QJsonArray todos;
        {
            // The ACP spec form: content-block array plus a status string.
            QJsonObject acp;
            acp.insert(u"id"_s, u"acp"_s);
            QJsonArray content;
            content.append(QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, u"From the ACP array"_s}});
            acp.insert(u"content"_s, content);
            acp.insert(u"status"_s, u"completed"_s);
            todos.append(acp);
        }
        {
            // Native planner shorthand: description plus booleans.
            todos.append(QJsonObject{{u"description"_s, u"Native planner step"_s}, {u"inProgress"_s, true}});
        }
        {
            // Hyphenated status spelling, text under another key.
            todos.append(QJsonObject{{u"text"_s, u"Hyphenated in-progress step"_s}, {u"status"_s, u"in-progress"_s}});
        }
        {
            // A titled step that is already done.
            todos.append(QJsonObject{{u"title"_s, u"Titled done step"_s}, {u"status"_s, u"done"_s}});
        }
        {
            // No text at all: skipped rather than rendered as bare glyphs.
            todos.append(QJsonObject{{u"id"_s, u"empty"_s}, {u"status"_s, u"pending"_s}});
        }

        TodoWidget widget;
        widget.setExpanded(true);
        widget.setTodos(todos);
        QCOMPARE(widget.totalCount(), 4);
        QCOMPARE(widget.completedCount(), 2);
        QCOMPARE(rowForDescription(widget, u"From the ACP array"_s)->property("state").toString(), u"done"_s);
        QCOMPARE(rowForDescription(widget, u"Native planner step"_s)->property("state").toString(), u"active"_s);
        QCOMPARE(rowForDescription(widget, u"Hyphenated in-progress step"_s)->property("state").toString(), u"active"_s);
        QCOMPARE(rowForDescription(widget, u"Titled done step"_s)->property("state").toString(), u"done"_s);
    }

    void listsEveryStepWithItsGlyphAndTheCounter()
    {
        TodoWidget widget;
        widget.setTodos({step(u"step1"_s, u"Read the sources"_s, true, false),
                         step(u"step2"_s, u"Patch the parser"_s, false, true),
                         step(u"step3"_s, u"Run the tests"_s, false, false)});

        QCOMPARE(widget.totalCount(), 3);
        QCOMPARE(widget.completedCount(), 1);
        QCOMPARE(widget.findChildren<QWidget *>(u"todoRow"_s).size(), 3);

        auto *progress = widget.findChild<QLabel *>(u"todoProgress"_s);
        QVERIFY(progress);
        QCOMPARE(progress->text(), u"1/3"_s);

        auto *header = widget.findChild<QLabel *>(u"todoHeader"_s);
        QVERIFY(header);
        QCOMPARE(header->text(), u"Todos"_s);

        // Each row leads with the glyph of its state.
        QWidget *done = rowForDescription(widget, u"Read the sources"_s);
        QWidget *active = rowForDescription(widget, u"Patch the parser"_s);
        QWidget *pending = rowForDescription(widget, u"Run the tests"_s);
        QVERIFY(done && active && pending);
        QCOMPARE(done->property("state").toString(), u"done"_s);
        QCOMPARE(active->property("state").toString(), u"active"_s);
        QCOMPARE(pending->property("state").toString(), u"pending"_s);
        QCOMPARE(done->findChild<QLabel *>(u"todoGlyph"_s)->text(), u"✓"_s);
        QCOMPARE(active->findChild<QLabel *>(u"todoGlyph"_s)->text(), u"●"_s);
        QCOMPARE(pending->findChild<QLabel *>(u"todoGlyph"_s)->text(), u"○"_s);
    }

    void advancementFollowsThePlan()
    {
        TodoWidget widget;
        widget.setTodos({step(u"step1"_s, u"One"_s, false, false),
                         step(u"step2"_s, u"Two"_s, false, false),
                         step(u"step3"_s, u"Three"_s, false, false)});
        auto *progress = widget.findChild<QLabel *>(u"todoProgress"_s);
        QVERIFY(progress);
        QCOMPARE(progress->text(), u"0/3"_s);

        // Two steps finish; the counter and the rows move with them.
        widget.setTodos({step(u"step1"_s, u"One"_s, true, false),
                         step(u"step2"_s, u"Two"_s, true, false),
                         step(u"step3"_s, u"Three"_s, false, true)});
        QCOMPARE(progress->text(), u"2/3"_s);
        QCOMPARE(widget.completedCount(), 2);
        QCOMPARE(rowForDescription(widget, u"One"_s)->property("state").toString(), u"done"_s);
        QCOMPARE(rowForDescription(widget, u"Three"_s)->property("state").toString(), u"active"_s);

        // A step marked both completed and in progress is done, not active.
        widget.setTodos({step(u"step1"_s, u"One"_s, true, true),
                         step(u"step2"_s, u"Two"_s, true, false),
                         step(u"step3"_s, u"Three"_s, true, false)});
        QCOMPARE(progress->text(), u"3/3"_s);
        QCOMPARE(rowForDescription(widget, u"One"_s)->property("state").toString(), u"done"_s);
    }

    // The dock stays compact while the plan runs: only the step finished just
    // before the current one, the current one, and the next open step are on
    // screen, and the whole list is a toggle away.
    void compactDockShowsThePreviousCurrentAndNextStep()
    {
        TodoWidget widget;
        widget.setTodos({step(u"step1"_s, u"One"_s, true, false),
                         step(u"step2"_s, u"Two"_s, true, false),
                         step(u"step3"_s, u"Three"_s, false, true),
                         step(u"step4"_s, u"Four"_s, false, false),
                         step(u"step5"_s, u"Five"_s, false, false)});

        QCOMPARE(widget.totalCount(), 5);
        // Compact: the window around the current step, nothing else.
        QCOMPARE(widget.findChildren<QWidget *>(u"todoRow"_s).size(), 3);
        QVERIFY(rowForDescription(widget, u"Two"_s));
        QVERIFY(rowForDescription(widget, u"Three"_s));
        QVERIFY(rowForDescription(widget, u"Four"_s));
        QVERIFY(!rowForDescription(widget, u"One"_s));
        QVERIFY(!rowForDescription(widget, u"Five"_s));

        auto *progress = widget.findChild<QLabel *>(u"todoProgress"_s);
        QVERIFY(progress);
        QCOMPARE(progress->text(), u"2/5"_s);

        // The toggle opens the whole list.
        auto *toggle = widget.findChild<QPushButton *>(u"todoToggle"_s);
        QVERIFY(toggle);
        QVERIFY(toggle->isVisible());
        toggle->click();
        QVERIFY(widget.isExpanded());
        QCOMPARE(widget.findChildren<QWidget *>(u"todoRow"_s).size(), 5);
        QVERIFY(rowForDescription(widget, u"One"_s));
        QVERIFY(rowForDescription(widget, u"Five"_s));
        // The counter still describes the whole plan either way.
        QCOMPARE(progress->text(), u"2/5"_s);
        QCOMPARE(widget.completedCount(), 2);
        QCOMPARE(rowForDescription(widget, u"Two"_s)->property("state").toString(), u"done"_s);

        // ... and closes it again.
        toggle->click();
        QVERIFY(!widget.isExpanded());
        QCOMPARE(widget.findChildren<QWidget *>(u"todoRow"_s).size(), 3);

        // The window follows the plan: with the current step finished and the
        // next one started, the strip moves down the list.
        widget.setTodos({step(u"step1"_s, u"One"_s, true, false),
                         step(u"step2"_s, u"Two"_s, true, false),
                         step(u"step3"_s, u"Three"_s, true, false),
                         step(u"step4"_s, u"Four"_s, false, true),
                         step(u"step5"_s, u"Five"_s, false, false)});
        QVERIFY(rowForDescription(widget, u"Three"_s));
        QVERIFY(rowForDescription(widget, u"Four"_s));
        QVERIFY(rowForDescription(widget, u"Five"_s));
        QVERIFY(!rowForDescription(widget, u"One"_s));
    }

    // A plan that fits in the compact window has nothing to toggle, so the
    // toggle stays out of the way.
    void toggleHiddenWhileTheWholeListFits()
    {
        TodoWidget widget;
        widget.setTodos({step(u"step1"_s, u"One"_s, true, false),
                         step(u"step2"_s, u"Two"_s, false, true),
                         step(u"step3"_s, u"Three"_s, false, false)});
        auto *toggle = widget.findChild<QPushButton *>(u"todoToggle"_s);
        QVERIFY(toggle);
        QVERIFY(!toggle->isVisible());
        QCOMPARE(widget.findChildren<QWidget *>(u"todoRow"_s).size(), 3);
    }

    void finishedStepsReadAsDone()
    {
        TodoWidget widget;
        widget.setTodos({step(u"step1"_s, u"Done step"_s, true, false),
                         step(u"step2"_s, u"Active step"_s, false, true),
                         step(u"step3"_s, u"Pending step"_s, false, false)});

        // The finished step is struck through; the active glyph carries the
        // accent, the pending one the muted colour.
        const QString doneSheet = rowForDescription(widget, u"Done step"_s)
                                      ->findChild<QLabel *>(u"todoText"_s)
                                      ->styleSheet();
        QVERIFY2(doneSheet.contains(u"line-through"_s), qPrintable(doneSheet));
        QCOMPARE(doneSheet, ChatTheme::todoItemTextDone());

        const QString activeGlyphSheet = rowForDescription(widget, u"Active step"_s)
                                             ->findChild<QLabel *>(u"todoGlyph"_s)
                                             ->styleSheet();
        QVERIFY(activeGlyphSheet == ChatTheme::todoGlyphActive() || activeGlyphSheet == ChatTheme::todoGlyphActiveDim());

        const QString pendingGlyphSheet = rowForDescription(widget, u"Pending step"_s)
                                              ->findChild<QLabel *>(u"todoGlyph"_s)
                                              ->styleSheet();
        QCOMPARE(pendingGlyphSheet, ChatTheme::todoGlyphPending());
    }

    void emptyPlanHidesTheWidget()
    {
        QWidget container;
        container.resize(320, 200);
        TodoWidget widget(&container);
        container.show();
        QVERIFY(QTest::qWaitForWindowExposed(&container));

        // Constructed hidden; a plan makes it appear, an empty one hides it.
        QVERIFY(!widget.isVisible());
        widget.setTodos({step(u"step1"_s, u"One"_s, false, false)});
        QVERIFY(widget.isVisible());
        widget.setTodos(QJsonArray());
        QVERIFY(!widget.isVisible());
        QCOMPARE(widget.totalCount(), 0);
    }

    void pulseRunsOnlyWhileAStepIsInProgress()
    {
        TodoWidget widget;
        widget.setTodos({step(u"step1"_s, u"One"_s, false, true)});
        QTimer *pulse = widget.findChild<QTimer *>();
        QVERIFY(pulse);
        QVERIFY2(pulse->isActive(), "the active step's glyph should pulse");

        // Nothing in progress any more: the pulse stops instead of spinning
        // against a finished list.
        widget.setTodos({step(u"step1"_s, u"One"_s, true, false)});
        QVERIFY(!pulse->isActive());
    }

    void applyStyleRepaintsInPlace()
    {
        TodoWidget widget;
        widget.setTodos({step(u"step1"_s, u"Done step"_s, true, false),
                         step(u"step2"_s, u"Pending step"_s, false, false)});
        widget.applyStyle();

        // The repaint keeps the states: the finished row is still struck
        // through, the counter still carries the progress style.
        const QString doneSheet = rowForDescription(widget, u"Done step"_s)
                                      ->findChild<QLabel *>(u"todoText"_s)
                                      ->styleSheet();
        QCOMPARE(doneSheet, ChatTheme::todoItemTextDone());
        auto *progress = widget.findChild<QLabel *>(u"todoProgress"_s);
        QVERIFY(progress);
        QCOMPARE(progress->styleSheet(), ChatTheme::todoProgress());
        QCOMPARE(progress->text(), u"1/2"_s);
    }
};

QTEST_MAIN(TestTodoWidget)
#include "test_todowidget.moc"
