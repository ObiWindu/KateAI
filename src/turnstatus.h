/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QTimer;

namespace KateAi
{

// The strip that sits between the transcript and the composer.
//
// It used to be a static line of keyboard hints, which meant it carried no
// information at exactly the moment the user wants information: while a turn
// is running. This widget makes that row live. When idle it shows the
// keyboard hint; while the agent works it shows an elapsed timer, what the
// agent is doing right now, how many sub-agents are in flight, and how many
// tool calls have completed.
class TurnStatus : public QWidget
{
    Q_OBJECT

public:
    explicit TurnStatus(QWidget *parent = nullptr);

    // The hint shown when nothing is running.
    void setIdleText(const QString &text);
    // True while a turn is in flight; drives the timer and the layout.
    void setBusy(bool busy);
    // Short description of the current step, e.g. "Editing src/agentloop.cpp".
    void setActivity(const QString &activity);
    // Running sub-agents, and tool calls finished during this turn.
    void setSubtaskCount(int running);
    void setCompletedToolCount(int completed);
    // Total tokens reported by the model, 0 hides the counter.
    void setTokenCount(int tokens);
    // A transient message (model list errors and the like) that replaces the
    // hint until the next state change.
    void flash(const QString &message, bool isError);
    // Resets every counter and returns to the idle hint.
    void reset();

Q_SIGNALS:
    // The strip is also a status button: clicking it scrolls to the newest
    // content, which is what a user does when they lose track of the turn.
    void scrollToLatestRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    QString elapsedText() const;

    QLabel *m_spinner = nullptr;
    QLabel *m_activity = nullptr;
    QLabel *m_stats = nullptr;
    QLabel *m_idle = nullptr;
    QTimer *m_ticker = nullptr;
    QElapsedTimer m_elapsed;

    bool m_busy = false;
    bool m_error = false;
    QString m_activityText;
    QString m_message;
    int m_subtasks = 0;
    int m_completedTools = 0;
    int m_tokens = 0;
};

} // namespace KateAi