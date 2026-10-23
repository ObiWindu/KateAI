/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QJsonArray>
#include <QList>
#include <QWidget>

class QLabel;
class QTimer;
class QVBoxLayout;

namespace KateAi
{

// The todo list of the running turn: the plan steps with their advancement,
// drawn the way an agent panel draws a todo list. A header row carries the
// "done/total" counter, and every row leads with a status glyph: an empty
// circle for a pending step, a pulsing dot for the step being worked on, and
// a check for a finished one. Finished steps keep their text but read as done
// (muted, struck through) -- they are the record of what the run did, not
// something still competing for attention.
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

    // Re-applies every theme-derived stylesheet, so a theme change repaints a
    // list that is live in the transcript.
    void applyStyle();

private:
    struct Row {
        QWidget *container = nullptr;
        QLabel *glyph = nullptr;
        QLabel *text = nullptr;
        QString id;
        bool completed = false;
        bool inProgress = false;
    };

    void rebuildRows(const QJsonArray &todos);
    void updateProgressLabel();
    void updatePulse();
    void applyRowStyle(Row &row) const;

    QList<Row> m_rows;
    QLabel *m_header = nullptr;
    QLabel *m_progress = nullptr;
    QVBoxLayout *m_rowsLayout = nullptr;
    QTimer *m_pulse = nullptr;
    int m_pulseTick = 0;
};

} // namespace KateAi
