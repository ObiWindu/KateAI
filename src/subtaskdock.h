/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

class QHBoxLayout;
class QPushButton;
class QVBoxLayout;

namespace KateAi
{

class SubtaskWidget;

// The intent dock's sub-agent slot: one tab strip over one visible card.
//
// Concurrent sub-agents used to stack vertically in the dock and push the
// composer off the screen. Only one card is ever on top here; the tabs above
// it choose which, so the strip stays the height of a single card however
// many agents run at once, and the card under the tabs is capped to a quarter
// of the chat panel (see SubtaskWidget::setHeightBudget()).
class SubtaskDock : public QWidget
{
    Q_OBJECT

public:
    explicit SubtaskDock(QWidget *parent = nullptr);

    // Moves a card into the dock (reparenting it) and puts it on top if nothing
    // else is showing. Tabs appear from the second card on: one card has no
    // one to switch to.
    void addCard(SubtaskWidget *card);
    // Detaches a card that is leaving the dock to retire into the transcript.
    // If it was the one on top, the next card takes over.
    void removeCard(SubtaskWidget *card);
    // Drops the bookkeeping for every card without deleting any of them: the
    // caller owns the cards and sweeps them when the conversation is cleared.
    void clearCards();

    bool hasCards() const
    {
        return !m_cards.isEmpty();
    }
    SubtaskWidget *activeCard() const
    {
        return m_active;
    }

    // The height every card is capped to, in px; <= 0 leaves them unbounded.
    void setHeightBudget(int px);
    int heightBudget() const
    {
        return m_heightBudget;
    }
    // Re-derives the tab stylesheets from the palette, after a theme change.
    void applyStyle();

private:
    void refreshTabs();
    void activateCard(SubtaskWidget *card);
    QString tabSheet(bool active, const QString &stateColor) const;

    struct Entry {
        QPointer<SubtaskWidget> card;
        QPointer<QPushButton> tab;
    };
    QList<Entry> m_cards;
    QPointer<SubtaskWidget> m_active;
    QWidget *m_tabRow = nullptr;
    QHBoxLayout *m_tabLayout = nullptr;
    QVBoxLayout *m_cardLayout = nullptr;
    int m_heightBudget = 0;
};

} // namespace KateAi
