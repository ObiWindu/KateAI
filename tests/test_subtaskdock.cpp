/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The intent dock's sub-agent slot: one tab strip over one visible card.
 *
 * Concurrent sub-agents used to stack vertically in the dock and push the
 * composer off the screen. These guard the properties that replaces that:
 *
 *  1. Only one sub-agent card is on screen at a time; the tabs above the card
 *     pick which, and a single card costs no tabs at all.
 *  2. A docked card is capped (a quarter of the chat panel) and scrolls inside
 *     that cap, so its content cannot grow the strip without bound.
 *  3. Whatever the cap, the card is as tall as its text: a QTextBrowser's size
 *     hint is a fixed 256x192 whatever it holds, and a card sized from that
 *     left the box almost all empty around a couple of lines.
 */

#include "chattheme.h"
#include "subtaskdock.h"
#include "subtaskwidget.h"

#include <QApplication>
#include <QPushButton>
#include <QTest>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtMath>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{
QString longTask(int paragraphs)
{
    QString text;
    for (int i = 0; i < paragraphs; ++i) {
        text += u"Paragraph %1 of the delegated work, which goes on for a while and would "
                u"once have grown the card without bound. "_s.arg(i);
    }
    return text;
}

// Lets the queued re-flow re-measure the documents and the row above lay out
// with the corrected heights. A single pass is not enough: the measurement
// arrives on a timer, and the geometry it changes follows on a LayoutRequest.
void settle()
{
    for (int i = 0; i < 5; ++i) {
        QApplication::processEvents();
    }
}

SubtaskWidget *finishedCard(const QString &taskId, const QString &task)
{
    auto *card = new SubtaskWidget(taskId);
    card->startAgent(u"Reviewer"_s, u"code"_s, task);
    ToolResult result;
    result.ok = true;
    result.output = u"Done — the diff is fine."_s;
    card->finishAgent(result);
    return card;
}
} // namespace

class TestSubtaskDock : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testOnlyOneCardIsShownAtATime()
    {
        SubtaskDock dock;
        auto *alpha = new SubtaskWidget(u"task-a"_s, &dock);
        auto *beta = new SubtaskWidget(u"task-b"_s, &dock);
        alpha->startAgent(u"Alpha"_s, u"code"_s, u"Read the parser"_s);
        beta->startAgent(u"Beta"_s, u"code"_s, u"Read the renderer"_s);
        dock.addCard(alpha);
        dock.addCard(beta);
        dock.resize(600, 300);
        dock.show();
        settle();

        const int visible = (alpha->isVisible() ? 1 : 0) + (beta->isVisible() ? 1 : 0);
        QCOMPARE(visible, 1);
        QVERIFY(dock.activeCard() == alpha);

        // The tabs are on top of the card, and they switch it.
        QPushButton *betaTab = tabFor(&dock, u"Beta"_s);
        QVERIFY2(betaTab, "a second sub-agent has to get a tab to switch to");
        const int tabTop = dock.mapFromGlobal(betaTab->mapToGlobal(QPoint(0, 0))).y();
        const int cardTop = dock.mapFromGlobal(dock.activeCard()->mapToGlobal(QPoint(0, 0))).y();
        QVERIFY2(tabTop < cardTop, "the tabs belong above the card they pick");
        betaTab->click();
        settle();
        QVERIFY2(alpha->isHidden(), "switching tabs must take the previous card off screen");
        QVERIFY2(beta->isVisible(), "switching tabs must bring the chosen card up");
        QVERIFY(dock.activeCard() == beta);
    }

    void testASingleCardNeedsNoTabs()
    {
        SubtaskDock dock;
        auto *card = new SubtaskWidget(u"task-c"_s, &dock);
        card->startAgent(u"Solo"_s, u"code"_s, u"Answer one question"_s);
        dock.addCard(card);
        dock.resize(600, 300);
        dock.show();
        settle();

        QVERIFY(card->isVisible());
        QVERIFY2(dock.findChildren<QPushButton *>(u"subtaskTab"_s).isEmpty(),
                 "one card has nothing to switch to, so it must not pay for a tab strip");
    }

    void testTheNextCardTakesOverWhenTheVisibleOneLeaves()
    {
        SubtaskDock dock;
        auto *first = new SubtaskWidget(u"task-d"_s, &dock);
        auto *second = new SubtaskWidget(u"task-e"_s, &dock);
        first->startAgent(u"First"_s, u"code"_s, u"Work one"_s);
        second->startAgent(u"Second"_s, u"code"_s, u"Work two"_s);
        dock.addCard(first);
        dock.addCard(second);
        dock.resize(600, 300);
        dock.show();
        settle();
        QVERIFY(dock.hasCards());

        // first retires into the transcript: it leaves the strip, and the card
        // still here takes over instead of the dock going blank.
        dock.removeCard(first);
        settle();
        QVERIFY2(second->isVisible(), "the remaining card has to take over the slot");
        QVERIFY(dock.findChildren<QPushButton *>(u"subtaskTab"_s).isEmpty());

        dock.removeCard(second);
        settle();
        QVERIFY2(!dock.isVisible(), "an empty slot must not leave a strip on screen");
        QVERIFY(!dock.hasCards());
    }

    void testClearCardsKeepsTheCardsForTheirOwner()
    {
        SubtaskDock dock;
        auto *card = new SubtaskWidget(u"task-f"_s, &dock);
        card->startAgent(u"Keeper"_s, u"code"_s, u"Do the work"_s);
        dock.addCard(card);
        dock.clearCards();
        // The cards belong to ChatWidget, which sweeps them: clearCards() only
        // forgets them, so this one must still be alive to delete.
        QVERIFY(card);
        QVERIFY(!dock.hasCards());
        QVERIFY(card->parentWidget());
        delete card;
    }

    void testTheHeightBudgetCapsTheCard()
    {
        // Parented rather than top-level: a top-level strip would resize
        // itself around a minimum, which is not what a strip inside the panel
        // does.
        QWidget host;
        host.resize(420, 400);
        SubtaskDock dock(&host);
        dock.resize(420, 400);
        auto *card = new SubtaskWidget(u"task-g"_s, &dock);
        card->startAgent(u"Chatty"_s, u"code"_s, longTask(40));
        for (int i = 0; i < 60; ++i) {
            card->appendActivity(u"Step %1 of the run, spelled out in full so the log is tall"_s.arg(i), false);
        }
        dock.addCard(card);
        host.show();
        settle();

        // Uncapped, the card grows to whatever its documents want -- which is
        // what used to grow the dock over the composer.
        QVERIFY2(card->height() > 160, qPrintable(QStringLiteral("the uncapped card is %1px tall").arg(card->height())));

        dock.setHeightBudget(160);
        settle();
        QCOMPARE(card->maximumHeight(), 160);
        QVERIFY2(card->height() <= 160,
                 qPrintable(QStringLiteral("a capped card still renders %1px tall").arg(card->height())));

        // The cap is only a cap: lifting it gives the card back its full height.
        dock.setHeightBudget(0);
        settle();
        QCOMPARE(card->maximumHeight(), QWIDGETSIZE_MAX);
        QVERIFY2(card->height() > 160, qPrintable(QStringLiteral("the uncapped card is %1px tall").arg(card->height())));
    }

    void testADockedCardIsAsTallAsItsText()
    {
        // The budget is what the strip may use, not what a card has to fill:
        // a two-line answer in a 200px budget used to render as a 200px box
        // with the text in the corner of it.
        SubtaskDock dock;
        dock.resize(420, 400);
        dock.setHeightBudget(200);

        auto *card = finishedCard(u"docked-short"_s, u"Check the diff."_s);
        QTextBrowser *answer = card->findChildren<QTextBrowser *>().constFirst();
        dock.addCard(card);
        dock.show();
        settle();

        const int textHeight = qCeil(answer->document()->size().height());
        QVERIFY2(card->height() <= 200,
                 qPrintable(QStringLiteral("the card renders %1px in a 200px budget").arg(card->height())));
        QVERIFY2(card->height() < 180,
                 qPrintable(QStringLiteral("a two-line card stretched itself to %1px").arg(card->height())));
        // And the box around the text is the text, not QTextBrowser's fixed
        // 256x192 hint: the document must fit in what the card gave it.
        QVERIFY2(answer->height() <= textHeight + 2,
                 qPrintable(QStringLiteral("the answer box is %1px for a %2px document")
                                .arg(answer->height())
                                .arg(textHeight)));
        QVERIFY2(answer->height() >= textHeight - 2,
                 qPrintable(QStringLiteral("the answer box is %1px for a %2px document")
                                .arg(answer->height())
                                .arg(textHeight)));
        delete card;
    }

    void testAFinishedCardInTranscriptIsAsTallAsItsText()
    {
        // The same card back in the transcript, uncapped, next to the spacer
        // the real transcript grows at. It must hug its two lines instead of
        // sizing itself around QTextBrowser's idea of a body.
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        auto *card = finishedCard(u"transcript-short"_s, u"Check the diff."_s);
        layout->addWidget(card);
        layout->addStretch(1);
        host.resize(420, 800);
        host.show();
        settle();

        QTextBrowser *answer = card->findChildren<QTextBrowser *>().constFirst();
        const int textHeight = qCeil(answer->document()->size().height());
        QVERIFY2(answer->height() <= textHeight + 2,
                 qPrintable(QStringLiteral("the answer box is %1px for a %2px document")
                                .arg(answer->height())
                                .arg(textHeight)));
        QVERIFY2(card->height() < 180,
                 qPrintable(QStringLiteral("a two-line transcript card grew to %1px").arg(card->height())));
        // The slack is the transcript's, not the card's.
        QVERIFY2(card->height() < host.height() / 2,
                 qPrintable(QStringLiteral("the card took %1px of a %2px row").arg(card->height()).arg(host.height())));
        delete card;
    }

    void testTheStripDoesNotKeepAnObsoleteMinimum()
    {
        // The strip's minimum is its cards' minimum, and a figure left over
        // from before they were measured properly is what the panel lays out
        // around: a strip offering an obsolete height is one the panel cannot
        // shrink to fit.
        QWidget host;
        host.resize(420, 400);
        SubtaskDock dock(&host);
        dock.resize(420, 400);
        dock.setHeightBudget(200);

        auto *card = new SubtaskWidget(u"stale"_s, &dock);
        card->startAgent(u"Chatty"_s, u"code"_s, longTask(40));
        for (int i = 0; i < 40; ++i) {
            card->appendActivity(u"Step %1: a long line of progress from the sub-agent"_s.arg(i), false);
        }
        dock.addCard(card);
        host.show();
        settle();
        QVERIFY2(dock.sizeHint().height() <= 200,
                 qPrintable(QStringLiteral("the strip offers %1px while a card may take 200").arg(dock.sizeHint().height())));

        // The budget shrinks under the strip: it has to follow, not go on
        // offering the height the cards asked for before.
        dock.setHeightBudget(120);
        settle();
        QVERIFY2(dock.sizeHint().height() <= 120,
                 qPrintable(QStringLiteral("the strip still offers %1px after the budget dropped to 120")
                                .arg(dock.sizeHint().height())));
        QVERIFY2(dock.minimumSizeHint().height() <= 120,
                 qPrintable(QStringLiteral("the strip still demands %1px after the budget dropped to 120")
                                .arg(dock.minimumSizeHint().height())));
        QVERIFY2(card->height() <= 120,
                 qPrintable(QStringLiteral("the card grew to %1px under a 120px budget").arg(card->height())));
    }

    void testTabsCarryTheCardState()
    {
        SubtaskDock dock;
        auto *alpha = new SubtaskWidget(u"task-h"_s, &dock);
        auto *beta = new SubtaskWidget(u"task-i"_s, &dock);
        alpha->startAgent(u"Alpha"_s, u"code"_s, u"Work"_s);
        beta->startAgent(u"Beta"_s, u"code"_s, u"More work"_s);
        dock.addCard(alpha);
        dock.addCard(beta);
        dock.resize(600, 300);
        dock.show();
        settle();

        QPushButton *alphaTab = tabFor(&dock, u"Alpha"_s);
        QVERIFY(alphaTab);
        QVERIFY2(alphaTab->text().contains(u"◐"_s), qPrintable(alphaTab->text()));

        alpha->finishAgent(ToolResult{});
        settle();
        QVERIFY2(alphaTab->text().contains(u"✓"_s),
                 qPrintable(QStringLiteral("the tab should show the card's state: %1").arg(alphaTab->text())));
        // The border carries the state colour, so the tab that is up reads as
        // such even before the text is read.
        QVERIFY2(alphaTab->styleSheet().contains(ChatTheme::success()), qPrintable(alphaTab->styleSheet()));
    }

private:
    static QPushButton *tabFor(SubtaskDock *dock, const QString &agentName)
    {
        for (QPushButton *tab : dock->findChildren<QPushButton *>(u"subtaskTab"_s)) {
            if (tab->text().contains(agentName)) {
                return tab;
            }
        }
        return nullptr;
    }
};

QTEST_MAIN(TestSubtaskDock)
#include "test_subtaskdock.moc"
