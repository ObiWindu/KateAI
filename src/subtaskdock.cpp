/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "subtaskdock.h"

#include "chattheme.h"
#include "subtaskwidget.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{
// A tab shares one line with the sub-agent's state glyph, so a long agent name
// is elided rather than widening the strip past the card it labels.
constexpr int MaxTabLabelWidth = 160;
} // namespace

SubtaskDock::SubtaskDock(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"subtaskDock"_s);
    // The dock above already paints the strip's background and must not be
    // restyled through its children, so this host stays transparent and boxes
    // nothing: the cards bring their own borders.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    // Tabs sit right on top of the card they pick between.
    m_tabRow = new QWidget(this);
    m_tabRow->setObjectName(u"subtaskTabRow"_s);
    m_tabLayout = new QHBoxLayout(m_tabRow);
    m_tabLayout->setContentsMargins(0, 0, 0, 0);
    m_tabLayout->setSpacing(4);
    m_tabLayout->addStretch(1);
    root->addWidget(m_tabRow);

    m_cardLayout = new QVBoxLayout;
    m_cardLayout->setContentsMargins(0, 0, 0, 0);
    m_cardLayout->setSpacing(4);
    root->addLayout(m_cardLayout);

    m_tabRow->hide();
    // Nothing is docked yet, so the host takes no space in the strip. addCard()
    // shows it, removeCard() hides it again.
    hide();
}

void SubtaskDock::addCard(SubtaskWidget *card)
{
    if (!card) {
        return;
    }
    for (const Entry &entry : std::as_const(m_cards)) {
        if (entry.card.data() == card) {
            return;
        }
    }
    // The tab mirrors the card's headline, so it listens rather than polls.
    connect(card, &SubtaskWidget::appearanceChanged, this, &SubtaskDock::refreshTabs);
    if (m_heightBudget > 0) {
        card->setHeightBudget(m_heightBudget);
    }
    m_cards.append(Entry{card, nullptr});
    if (m_active.isNull()) {
        m_active = card;
    }
    m_cardLayout->addWidget(card);
    card->setVisible(card == m_active);
    show();
    refreshTabs();
    card->reflowNow();
}

void SubtaskDock::removeCard(SubtaskWidget *card)
{
    if (!card) {
        return;
    }
    for (int i = 0; i < m_cards.size(); ++i) {
        if (m_cards[i].card.data() != card) {
            continue;
        }
        disconnect(card, &SubtaskWidget::appearanceChanged, this, &SubtaskDock::refreshTabs);
        const bool wasActive = (m_active == card);
        // The card's tab leaves with it: the strip lists what is still here.
        if (m_cards[i].tab) {
            delete m_cards[i].tab.data();
        }
        m_cards.removeAt(i);
        // The card belongs to the transcript from here; take it out of the
        // strip first so the layout does not keep a live item on it.
        m_cardLayout->removeWidget(card);
        if (wasActive) {
            m_active = m_cards.isEmpty() ? nullptr : m_cards.first().card.data();
        }
        break;
    }
    if (m_cards.isEmpty()) {
        m_active.clear();
        hide();
    } else if (m_active) {
        for (const Entry &entry : std::as_const(m_cards)) {
            if (entry.card) {
                entry.card->setVisible(entry.card == m_active);
            }
        }
        m_active->reflowNow();
    }
    refreshTabs();
}

void SubtaskDock::clearCards()
{
    for (const Entry &entry : std::as_const(m_cards)) {
        if (entry.tab) {
            delete entry.tab.data();
        }
        if (entry.card) {
            disconnect(entry.card, &SubtaskWidget::appearanceChanged, this, &SubtaskDock::refreshTabs);
            // The owner sweeps the cards next; uncap them so a survivor does
            // not keep a budget that was only ever meant for the dock.
            entry.card->setHeightBudget(0);
            m_cardLayout->removeWidget(entry.card);
        }
    }
    m_cards.clear();
    m_active.clear();
    m_tabRow->hide();
    hide();
}

void SubtaskDock::setHeightBudget(int px)
{
    if (m_heightBudget == px) {
        return;
    }
    m_heightBudget = px;
    for (const Entry &entry : std::as_const(m_cards)) {
        if (entry.card) {
            entry.card->setHeightBudget(px);
        }
    }
}

void SubtaskDock::applyStyle()
{
    refreshTabs();
}

void SubtaskDock::activateCard(SubtaskWidget *card)
{
    if (!card) {
        return;
    }
    bool known = false;
    for (const Entry &entry : std::as_const(m_cards)) {
        if (entry.card.data() == card) {
            known = true;
            break;
        }
    }
    if (!known) {
        return;
    }
    m_active = card;
    for (const Entry &entry : std::as_const(m_cards)) {
        if (entry.card) {
            entry.card->setVisible(entry.card == card);
        }
    }
    // The card was measured at its own width while hidden; show it at the
    // strip's width before its body is sized.
    card->reflowNow();
    refreshTabs();
}

QString SubtaskDock::tabSheet(bool active, const QString &stateColor) const
{
    // The checked tab is the card on screen, so it carries that card's state
    // colour in its border: which agent is up is legible before any text.
    return QStringLiteral("QPushButton#subtaskTab { background: transparent; color: %1;"
                          " border: 1px solid %2; border-radius: 5px; padding: 2px 8px;"
                          " font-size: 10px; }"
                          "QPushButton#subtaskTab:hover { background-color: %3; }"
                          "QPushButton#subtaskTab:checked { background-color: %4; color: %5;"
                          " border-color: %6; font-weight: 600; }")
        .arg(ChatTheme::textMuted(), ChatTheme::border(), ChatTheme::hoverBg(),
             ChatTheme::mixColors(ChatTheme::accent(), ChatTheme::cardBg(), 18), ChatTheme::textPrimary(),
             active ? stateColor : ChatTheme::border());
}

void SubtaskDock::refreshTabs()
{
    // Drop tabs whose card is gone: a card normally leaves through
    // removeCard(), but a conversation sweep can delete one outright.
    for (int i = m_cards.size() - 1; i >= 0; --i) {
        if (!m_cards[i].card.isNull()) {
            continue;
        }
        if (m_cards[i].tab) {
            delete m_cards[i].tab.data();
        }
        m_cards.removeAt(i);
    }
    if (m_active.isNull() && !m_cards.isEmpty()) {
        m_active = m_cards.first().card.data();
    }

    // One card has nothing to switch to, so the strip only exists for a
    // second one.
    const bool showTabs = m_cards.size() > 1;
    m_tabRow->setVisible(showTabs);

    for (int i = 0; i < m_cards.size(); ++i) {
        Entry &entry = m_cards[i];
        SubtaskWidget *card = entry.card.data();
        if (!showTabs) {
            delete entry.tab.data();
            continue;
        }
        if (!entry.tab) {
            auto *tab = new QPushButton(m_tabRow);
            tab->setObjectName(u"subtaskTab"_s);
            tab->setCursor(Qt::PointingHandCursor);
            tab->setCheckable(true);
            const QPointer<SubtaskWidget> guard = card;
            connect(tab, &QPushButton::clicked, this, [this, guard] {
                activateCard(guard.data());
            });
            entry.tab = tab;
            // Before the trailing stretch, in card order.
            m_tabLayout->insertWidget(i, tab);
        }
        QPushButton *tab = entry.tab.data();
        const bool active = (card == m_active.data());
        const QString name = tab->fontMetrics().elidedText(card->displayName(), Qt::ElideRight, MaxTabLabelWidth);
        tab->setText(QStringLiteral("<span style=\"color:%1;\">%2</span> %3")
                         .arg(card->stateColor(), card->stateGlyph(), name.toHtmlEscaped()));
        QString tip = card->statusText();
        if (!card->description().isEmpty()) {
            tip += u'\n';
            tip += card->description();
        }
        tab->setToolTip(tip);
        tab->setChecked(active);
        tab->setStyleSheet(tabSheet(active, card->stateColor()));
    }
}

} // namespace KateAi
