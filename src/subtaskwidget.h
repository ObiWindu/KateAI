/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QElapsedTimer>
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
    // Finishes the card; `output` is the answer the orchestrator received.
    void finishAgent(const ToolResult &result);
    // Stops the card without a result, e.g. when the whole turn is aborted.
    void markAbandoned(const QString &reason);

    void reflowNow();

Q_SIGNALS:
    void cancelRequested(const QString &taskId);

private:
    void updateHeader();
    void updateElapsed();
    void setActivityVisible(bool visible);
    void applyStyle();

    QString m_taskId;
    QString m_agentName;
    QString m_modeId;
    QString m_description;
    bool m_running = false;
    bool m_failed = false;
    bool m_cancelled = false;
    int m_stepCount = 0;

    QElapsedTimer m_elapsed;

    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_toggle = nullptr;
    QTextBrowser *m_answer = nullptr;
    QTextBrowser *m_activity = nullptr;
    QWidget *m_activityContainer = nullptr;
    QTimer *m_ticker = nullptr;
    QPointer<QWidget> m_expandedActivity = nullptr;
};

} // namespace KateAi