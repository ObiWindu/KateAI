/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The intent dock's sub-agent slot: one tab strip over one visible card.
 *
 * Concurrent sub-agents used to stack vertically in the dock and push the
 * composer off the screen. These guard the two properties that replaces that:
 *
 *  1. Only one sub-agent card is on screen at a time; the tabs above the card
 *     pick which, and a single card costs no tabs at all.
 *  2. A docked card is capped (a quarter of the chat panel) and scrolls inside
 *     that cap, so its content cannot grow the strip without bound.
 */

#include "chattheme.h"
#include "subtaskdock.h"
#include "subtaskwidget.h"

#include <QApplication>
#include <QPushButton>
#include <QTest>
#include <QTextBrowser>

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
        QApplication::processEvents();

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
        QApplication::processEvents();
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
        QApplication::processEvents();

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
        QApplication::processEvents();
        QVERIFY(dock.hasCards());

        // first retires into the transcript: it leaves the strip, and the card
        // still here takes over instead of the dock going blank.
        dock.removeCard(first);
        QApplication::processEvents();
        QVERIFY2(second->isVisible(), "the remaining card has to take over the slot");
        QVERIFY(dock.findChildren<QPushButton *>(u"subtaskTab"_s).isEmpty());

        dock.removeCard(second);
        QApplication::processEvents();
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
        SubtaskDock dock;
        auto *card = new SubtaskWidget(u"task-g"_s, &dock);
        card->startAgent(u"Chatty"_s, u"code"_s, longTask(40));
        for (int i = 0; i < 60; ++i) {
            card->appendActivity(u"Step %1 of the run, spelled out in full so the log is tall"_s.arg(i), false);
        }
        dock.addCard(card);
        dock.resize(600, 300);
        dock.show();
        QApplication::processEvents();

        // Uncapped, the card grows to whatever its documents want -- which is
        // what used to grow the dock over the composer.
        QVERIFY2(card->height() > 160, qPrintable(QStringLiteral("the uncapped card is %1px tall").arg(card->height())));

        dock.setHeightBudget(160);
        QApplication::processEvents();
        QCOMPARE(card->maximumHeight(), 160);
        QVERIFY2(card->height() <= 160,
                 qPrintable(QStringLiteral("a capped card still renders %1px tall").arg(card->height())));

        // The cap is only a cap: lifting it gives the card back its full height.
        dock.setHeightBudget(0);
        QApplication::processEvents();
        QCOMPARE(card->maximumHeight(), QWIDGETSIZE_MAX);
        QVERIFY2(card->height() > 160, qPrintable(QStringLiteral("the uncapped card is %1px tall").arg(card->height())));
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
        QApplication::processEvents();

        QPushButton *alphaTab = tabFor(&dock, u"Alpha"_s);
        QVERIFY(alphaTab);
        QVERIFY2(alphaTab->text().contains(u"◐"_s), qPrintable(alphaTab->text()));

        alpha->finishAgent(ToolResult{});
        QApplication::processEvents();
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
