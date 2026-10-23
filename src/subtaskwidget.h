/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QElapsedTimer>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QWidget>

class QLabel;
class QPushButton;
class QTextBrowser;
class QTimer;

namespace KateAi
{

// A transcript card for one sub-agent. Shows who is running, what it is doing
// right now, and its answer when it finishes. Unlike ToolCallWidget this has to
// cope with an operation that is still in flight, so it owns a live status line
// and a cancel affordance.
class SubtaskWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SubtaskWidget(const QString &taskId, QWidget *parent = nullptr);
    ~SubtaskWidget() override;

    QString taskId() const
    {
        return m_taskId;
    }
    bool isRunning() const
    {
        return m_running;
    }

    // Begins the run. agentName/modeId describe who is being started.
    void startAgent(const QString &agentName, const QString &modeId, const QString &description);
    // One line of progress from the sub-agent's own tool activity.
    void appendActivity(const QString &line, bool isError);
    // Incremental user-facing text from the sub-agent while it is running.
    void appendMessageDelta(const QString &delta);
    // Finishes the card; `output` is the answer the orchestrator received.
    void finishAgent(const ToolResult &result);
    // Stops the card without a result, e.g. when the whole turn is aborted.
    void markAbandoned(const QString &reason);

    void reflowNow();
    // Re-applies every theme-derived stylesheet, so a theme change repaints a
    // card that is live in the transcript or the intent dock.
    void applyStyle();

Q_SIGNALS:
    void cancelRequested(const QString &taskId);

private:
    void updateHeader();
    void updateElapsed();
    void updateCurrentActivity();
    void setActivityVisible(bool visible);
    // Re-renders the activity log as one markdown document from the
    // accumulated lines, so a sub-agent that summarises its steps with
    // emphasis or code renders instead of showing literal markup.
    void rebuildActivityLog();

protected:
    void resizeEvent(QResizeEvent *event) override;

    QString m_taskId;
    QString m_agentName;
    QString m_modeId;
    QString m_description;
    // The most recent step the sub-agent reported, kept in full so it can be
    // re-elided whenever the card is resized.
    QString m_currentLine;
    // The activity log's lines with their error flag, the source the log is
    // re-rendered from. Capped, so a chatty sub-agent cannot grow the card
    // without bound.
    QList<QPair<QString, bool>> m_activityLog;
    QString m_streamedMessage;
    bool m_currentIsError = false;
    bool m_running = false;
    bool m_failed = false;
    bool m_cancelled = false;
    int m_stepCount = 0;

    QElapsedTimer m_elapsed;

    QLabel *m_badge = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_modeChip = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_bodyCaption = nullptr;
    // The step the sub-agent is on right now, mirrored from its own tool
    // activity so the card says what it is doing, not merely that it is.
    QLabel *m_current = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_toggle = nullptr;
    QTextBrowser *m_answer = nullptr;
    QTextBrowser *m_activity = nullptr;
    QWidget *m_activityContainer = nullptr;
    QTimer *m_ticker = nullptr;
    QPointer<QWidget> m_expandedActivity = nullptr;
};

} // namespace KateAi
