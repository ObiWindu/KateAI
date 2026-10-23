/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QJsonArray>
#include <QList>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace KateAi
{

// The todo list of the running turn, docked at the bottom of the chat. While
// the plan advances the dock stays compact: it shows the step being worked
// on framed by the step finished just before it and the next one open, so the
// strip reads as "just did X, now doing Y, next is Z" at a glance. A toggle
// in the header row opens the whole list; a header counter carries the
// "done/total" progress either way. Every row leads with a status glyph: an
// empty circle for a pending step, a pulsing dot for the step being worked
// on, and a check for a finished one. Finished steps keep their text but
// read as done (muted, struck through) -- they are the record of what the
// run did, not something still competing for attention.
class TodoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TodoWidget(QWidget *parent = nullptr);

    // Replaces the whole list. The array carries the plan's step shape:
    // {id, description, completed, inProgress}. An empty array hides the
    // widget; the caller decides where the (possibly hidden) widget lives.
    void setTodos(const QJsonArray &todos);

    int totalCount() const
    {
        return m_rows.size();
    }
    int completedCount() const;

    // Whether the whole list is shown, or only the three-step window around
    // the current step. The header's toggle flips it.
    bool isExpanded() const
    {
        return m_expanded;
    }
    void setExpanded(bool expanded);

    // Re-applies every theme-derived stylesheet, so a theme change repaints a
    // list that is live in the chat.
    void applyStyle();

private:
    struct Row {
        QWidget *container = nullptr;
        QLabel *glyph = nullptr;
        QLabel *text = nullptr;
        QString id;
        QString description;
        bool completed = false;
        bool inProgress = false;
        bool visible = false;
    };

    void rebuildRows(const QJsonArray &todos);
    void updateProgressLabel();
    void updateToggle();
    void updatePulse();
    void applyRowStyle(Row &row) const;
    // The rows the current mode shows: the whole plan when expanded, and the
    // previous-completed / current / next-pending window when compact.
    QList<int> visibleWindow() const;

    QList<Row> m_rows;
    bool m_expanded = false;
    QLabel *m_header = nullptr;
    QLabel *m_progress = nullptr;
    QPushButton *m_toggle = nullptr;
    QVBoxLayout *m_rowsLayout = nullptr;
    QTimer *m_pulse = nullptr;
    int m_pulseTick = 0;
};

} // namespace KateAi
