/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "todowidget.h"

#include "chattheme.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

TodoWidget::TodoWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(2);

    // Header: the caption on the left, the advancement counter on the right.
    auto *headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(4);
    m_header = new QLabel(i18n("Todos"), this);
    m_header->setObjectName(u"todoHeader"_s);
    headerRow->addWidget(m_header);
    headerRow->addStretch();
    m_progress = new QLabel(this);
    m_progress->setObjectName(u"todoProgress"_s);
    m_progress->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    headerRow->addWidget(m_progress);
    outer->addLayout(headerRow);

    m_rowsLayout = new QVBoxLayout();
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(1);
    outer->addLayout(m_rowsLayout);

    // The pulse is a stylesheet swap on the active glyph, the same trick the
    // chat panel's pills use: a graphics effect would force the label through
    // an offscreen surface and cost more than the effect is worth.
    m_pulse = new QTimer(this);
    m_pulse->setInterval(380);
    connect(m_pulse, &QTimer::timeout, this, [this] {
        m_pulseTick = (m_pulseTick + 1) & 1;
        for (Row &row : m_rows) {
            if (row.inProgress && !row.completed && row.glyph) {
                row.glyph->setStyleSheet(m_pulseTick == 0 ? ChatTheme::todoGlyphActive() : ChatTheme::todoGlyphActiveDim());
            }
        }
    });

    applyStyle();
    hide();
}

void TodoWidget::setTodos(const QJsonArray &todos)
{
    rebuildRows(todos);
    updateProgressLabel();
    updatePulse();
    setVisible(!todos.isEmpty());
}

int TodoWidget::completedCount() const
{
    int count = 0;
    for (const Row &row : m_rows) {
        if (row.completed) {
            ++count;
        }
    }
    return count;
}

void TodoWidget::applyStyle()
{
    setStyleSheet(ChatTheme::todoList());
    if (m_header) {
        m_header->setStyleSheet(ChatTheme::sectionLabel());
    }
    if (m_progress) {
        m_progress->setStyleSheet(ChatTheme::todoProgress());
    }
    for (Row &row : m_rows) {
        applyRowStyle(row);
    }
}

void TodoWidget::rebuildRows(const QJsonArray &todos)
{
    // The list is short (a plan is capped at 20 steps) and a plan update
    // replaces it wholesale, so rebuilding from scratch is simpler -- and
    // safer -- than diffing the old rows against the new ones. delete(), not
    // deleteLater(): a deferred deletion leaves the row parented and painting
    // over the top of the list until the event loop runs.
    for (Row &row : m_rows) {
        delete row.container;
    }
    m_rows.clear();
    for (const QJsonValue &value : todos) {
        const QJsonObject step = value.toObject();
        Row row;
        row.id = step.value(u"id"_s).toString();
        row.completed = step.value(u"completed"_s).toBool();
        row.inProgress = step.value(u"inProgress"_s).toBool() && !row.completed;

        auto *container = new QWidget(this);
        container->setObjectName(u"todoRow"_s);
        // The row's state as a property, so a test (or a future stylesheet
        // keyed off it) can tell the states apart without parsing glyphs.
        container->setProperty("state",
                               row.completed ? u"done"_s : (row.inProgress ? u"active"_s : u"pending"_s));
        auto *rowLayout = new QHBoxLayout(container);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);
        row.glyph = new QLabel(container);
        row.glyph->setObjectName(u"todoGlyph"_s);
        row.glyph->setFixedWidth(14);
        row.glyph->setAlignment(Qt::AlignCenter);
        rowLayout->addWidget(row.glyph);
        row.text = new QLabel(step.value(u"description"_s).toString(), container);
        row.text->setObjectName(u"todoText"_s);
        row.text->setWordWrap(true);
        rowLayout->addWidget(row.text, 1);
        row.container = container;

        applyRowStyle(row);
        m_rowsLayout->addWidget(container);
        m_rows.append(row);
    }
}

void TodoWidget::updateProgressLabel()
{
    if (m_progress) {
        m_progress->setText(QStringLiteral("%1/%2").arg(completedCount()).arg(m_rows.size()));
    }
}

void TodoWidget::updatePulse()
{
    if (!m_pulse) {
        return;
    }
    bool anyActive = false;
    for (const Row &row : m_rows) {
        if (row.inProgress && !row.completed) {
            anyActive = true;
            break;
        }
    }
    if (anyActive) {
        if (!m_pulse->isActive()) {
            m_pulse->start();
        }
        return;
    }
    if (m_pulse->isActive()) {
        m_pulse->stop();
        // Leave the active glyph in its bright phase when the pulse stops.
        m_pulseTick = 0;
        for (Row &row : m_rows) {
            applyRowStyle(row);
        }
    }
}

void TodoWidget::applyRowStyle(Row &row) const
{
    if (!row.glyph || !row.text) {
        return;
    }
    if (row.completed) {
        row.glyph->setText(u"✓"_s);
        row.glyph->setStyleSheet(ChatTheme::todoGlyphDone());
        row.text->setStyleSheet(ChatTheme::todoItemTextDone());
    } else if (row.inProgress) {
        row.glyph->setText(u"●"_s);
        row.glyph->setStyleSheet(m_pulseTick == 0 ? ChatTheme::todoGlyphActive() : ChatTheme::todoGlyphActiveDim());
        row.text->setStyleSheet(ChatTheme::todoItemTextActive());
    } else {
        row.glyph->setText(u"○"_s);
        row.glyph->setStyleSheet(ChatTheme::todoGlyphPending());
        row.text->setStyleSheet(ChatTheme::todoItemText());
    }
}

} // namespace KateAi
