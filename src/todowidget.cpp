/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "todowidget.h"

#include "chattheme.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// The steps arrive from three producers -- the ACP layer, the native
// structured-planning parser, and a session restored from disk -- and they do
// not all spell the text field the same way. Try the known keys, then the ACP
// content-block array, so every variant renders instead of a blank row.
QString stepText(const QJsonObject &step)
{
    for (const QString &key : {u"description"_s, u"text"_s, u"title"_s, u"name"_s}) {
        const QString text = step.value(key).toString();
        if (!text.isEmpty()) {
            return text;
        }
    }
    const QJsonValue content = step.value(u"content"_s);
    if (content.isString()) {
        return content.toString();
    }
    if (content.isArray()) {
        QStringList parts;
        for (const QJsonValue &block : content.toArray()) {
            const QString text = block.toObject().value(u"text"_s).toString();
            if (!text.isEmpty()) {
                parts.append(text);
            }
        }
        return parts.join(u' ').trimmed();
    }
    return {};
}

// The booleans win when the producer sent booleans; otherwise the status string
// decides, in every spelling seen in the wild ("in_progress", "in-progress",
// "started", "done", ...).
bool stepCompleted(const QJsonObject &step)
{
    if (step.value(u"completed"_s).isBool()) {
        return step.value(u"completed"_s).toBool();
    }
    const QString status = step.value(u"status"_s).toString().toLower().replace(QChar(u'-'), QChar(u'_'));
    return status == u"completed"_s || status == u"done"_s;
}

bool stepInProgress(const QJsonObject &step)
{
    if (step.value(u"inProgress"_s).isBool()) {
        return step.value(u"inProgress"_s).toBool();
    }
    const QString status = step.value(u"status"_s).toString().toLower().replace(QChar(u'-'), QChar(u'_'));
    return status == u"in_progress"_s || status == u"started"_s;
}

} // namespace

TodoWidget::TodoWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(2);

    // Header: the caption on the left, the advancement counter and the
    // whole-list toggle on the right.
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
    m_toggle = new QPushButton(this);
    m_toggle->setObjectName(u"todoToggle"_s);
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->setFlat(true);
    m_toggle->hide();
    connect(m_toggle, &QPushButton::clicked, this, [this] {
        setExpanded(!m_expanded);
    });
    headerRow->addWidget(m_toggle);
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
            if (row.visible && row.inProgress && !row.completed && row.glyph) {
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
    updateToggle();
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

void TodoWidget::setExpanded(bool expanded)
{
    if (m_expanded == expanded) {
        return;
    }
    m_expanded = expanded;
    // The rows are rebuilt from the stored plan, so the toggle is a pure view
    // change: nothing about the plan's state moves.
    QJsonArray todos;
    for (const Row &row : std::as_const(m_rows)) {
        QJsonObject step;
        step.insert(u"id"_s, row.id);
        step.insert(u"completed"_s, row.completed);
        step.insert(u"inProgress"_s, row.inProgress);
        // The text was normalised on the way in; hand it back so the rebuild
        // does not have to re-guess the producer's spelling.
        step.insert(u"description"_s, row.description);
        todos.append(step);
    }
    rebuildRows(todos);
    updateToggle();
    updatePulse();
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
    if (m_toggle) {
        m_toggle->setStyleSheet(
            QStringLiteral("QPushButton { color: %1; font-size: 10px; font-weight: 600;"
                           " border: none; background: transparent; padding: 0 2px; }"
                           "QPushButton:hover { color: %2; }")
                .arg(ChatTheme::textMuted(), ChatTheme::textPrimary()));
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
    int synthetic = 0;
    for (const QJsonValue &value : todos) {
        const QJsonObject step = value.toObject();
        const QString description = stepText(step);
        // A step with no text is a row of bare glyphs: the list shows work,
        // not a counter.
        if (description.isEmpty()) {
            continue;
        }
        Row row;
        row.id = step.value(u"id"_s).toString();
        if (row.id.isEmpty()) {
            row.id = u"step-%1"_s.arg(++synthetic);
        }
        row.description = description;
        row.completed = stepCompleted(step);
        row.inProgress = stepInProgress(step) && !row.completed;
        m_rows.append(row);
    }

    const QList<int> window = visibleWindow();
    for (int index : window) {
        Row &row = m_rows[index];
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
        row.text = new QLabel(row.description, container);
        row.text->setObjectName(u"todoText"_s);
        row.text->setWordWrap(true);
        rowLayout->addWidget(row.text, 1);
        row.container = container;
        row.visible = true;

        applyRowStyle(row);
        m_rowsLayout->addWidget(container);
    }
}

void TodoWidget::updateProgressLabel()
{
    if (m_progress) {
        m_progress->setText(QStringLiteral("%1/%2").arg(completedCount()).arg(m_rows.size()));
    }
}

void TodoWidget::updateToggle()
{
    if (!m_toggle) {
        return;
    }
    // A plan that fits in the compact window has nothing to toggle.
    if (m_rows.size() <= 3) {
        m_toggle->hide();
        return;
    }
    m_toggle->setText(m_expanded ? i18n("Less") : i18n("All %1", m_rows.size()));
    m_toggle->show();
}

void TodoWidget::updatePulse()
{
    if (!m_pulse) {
        return;
    }
    bool anyActive = false;
    for (const Row &row : m_rows) {
        if (row.visible && row.inProgress && !row.completed) {
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

QList<int> TodoWidget::visibleWindow() const
{
    const int total = m_rows.size();
    if (m_expanded || total <= 3) {
        QList<int> all;
        for (int i = 0; i < total; ++i) {
            all.append(i);
        }
        return all;
    }

    // The step being worked on anchors the middle row. When nothing runs,
    // the first open step takes that slot; a fully finished plan has no
    // anchor at all.
    int active = -1;
    for (int i = 0; i < total; ++i) {
        if (m_rows.at(i).inProgress && !m_rows.at(i).completed) {
            active = i;
            break;
        }
    }
    if (active < 0) {
        for (int i = 0; i < total; ++i) {
            if (!m_rows.at(i).completed) {
                active = i;
                break;
            }
        }
    }
    if (active < 0) {
        // Everything is done: the tail of the list is the run's record.
        QList<int> tail;
        for (int i = total - 3; i < total; ++i) {
            if (i >= 0) {
                tail.append(i);
            }
        }
        return tail;
    }

    QList<int> window;
    window.append(active);
    // The latest finished step before it goes on top, barred; the next open
    // one goes below.
    for (int i = active - 1; i >= 0; --i) {
        if (m_rows.at(i).completed) {
            window.prepend(i);
            break;
        }
    }
    for (int i = active + 1; i < total; ++i) {
        if (!m_rows.at(i).completed && !m_rows.at(i).inProgress) {
            window.append(i);
            break;
        }
    }

    // Exactly three rows whenever the plan has them: fill the remaining slots
    // with the steps nearest to the current one, keeping the plan's order.
    while (window.size() < 3) {
        int nearest = -1;
        int bestDistance = total + 1;
        for (int i = 0; i < total; ++i) {
            if (window.contains(i)) {
                continue;
            }
            const int distance = qAbs(i - active);
            if (distance < bestDistance) {
                bestDistance = distance;
                nearest = i;
            }
        }
        if (nearest < 0) {
            break;
        }
        window.append(nearest);
    }
    std::sort(window.begin(), window.end());
    return window;
}

} // namespace KateAi
