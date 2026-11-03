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
    // Whether startAgent ever ran for this card. A card that exists but never
    // started is waiting on a task id that does not match it.
    bool wasStarted() const
    {
        return m_everStarted;
    }

    // Begins the run. agentName/modeId describe who is being started.
    void startAgent(const QString &agentName, const QString &modeId, const QString &description);
    // One line of progress from the sub-agent's own tool activity.
    void appendActivity(const QString &line, bool isError);
    // Live partial output from a running child tool (bash stream, fetch
    // progress). Shown immediately so the card reads as live.
    void setPartialOutput(const QString &partial);
    // Incremental user-facing text from the sub-agent while it is running.
    void appendMessageDelta(const QString &delta);
    // Finishes the card; `output` is the answer the orchestrator received.
    void finishAgent(const ToolResult &result);
    // Stops the card without a result, e.g. when the whole turn is aborted.
    void markAbandoned(const QString &reason);

    void reflowNow();
    // Caps the card's height in px; <= 0 lifts the cap. The intent dock sets
    // this to a quarter of the chat panel, so a chatty sub-agent scrolls its
    // body inside the card instead of growing the strip over the composer.
    // The cap is lifted again when the card retires into the transcript,
    // where it may be as tall as its content.
    void setHeightBudget(int px);
    int heightBudget() const
    {
        return m_heightBudget;
    }

    // --- Intent-dock tab strip --------------------------------------------------
    // Who this card is and how it is doing, so the tab strip that picks between
    // concurrent sub-agent cards can label them without reaching into the card's
    // own widgets.
    QString displayName() const;
    QString stateGlyph() const;
    QString stateColor() const;
    // "Running · 12s · 5 steps" as plain text, for a tab tooltip.
    QString statusText() const;
    QString description() const
    {
        return m_description;
    }

    // Re-applies every theme-derived stylesheet, so a theme change repaints a
    // card that is live in the transcript or the intent dock.
    void applyStyle();

Q_SIGNALS:
    void cancelRequested(const QString &taskId);
    // The card's headline changed (state, name, elapsed time, step count): the
    // tab pointing at it has to be redrawn.
    void appearanceChanged();
    // The height this card asks for changed, because one of its documents was
    // re-measured: whoever is laying the card out has to do it again.
    void bodyLayoutChanged();

private:
    void updateHeader();
    void updateElapsed();
    void updateCurrentActivity();
    void setActivityVisible(bool open);
    // The disclosure keeps its own arrow and, once the sub-agent has reported
    // anything, the number of steps behind it.
    void updateActivityToggle();
    // Re-renders the activity log as one markdown document from the
    // accumulated lines, so a sub-agent that summarises its steps with
    // emphasis or code renders instead of showing literal markup.
    void rebuildActivityLog();
    // The part of the card that is not the two scrolling documents -- header,
    // caption, status line -- which a height budget has to leave room for.
    int chromeHeight() const;
    int activityChromeHeight() const;
    QString timeText() const;
    QString stateWord() const;
    // Coalesces a reflow onto the next event-loop turn, when the layout has
    // settled and the documents are wrapped at the width they really hold.
    void scheduleReflow();
    // The height a document needs at its current width.
    int documentHeight(QTextBrowser *browser);
    // Gives a document exactly that height: minimum *and* maximum. A
    // QTextBrowser's sizeHint is a fixed 256x192 whatever it holds, so without
    // a maximum the card sizes itself around that and leaves the text in the
    // corner of an otherwise empty box. Returns whether anything moved.
    bool setBodyHeight(QTextBrowser *browser, int height);

protected:
    void resizeEvent(QResizeEvent *event) override;
    // The two documents re-wrap when the layout hands them their real width,
    // which happens after this widget has already been resized, so the reflow
    // that measures them is queued from here instead of run inline.
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    // Deltas that arrived before startAgent (toolStarted creates the card
    // before subtaskStarted marks it running). Kept so early streaming text
    // is not silently dropped.
    QString m_pendingStreamed;
    // Latest live tool output for the Doing: line.
    QString m_partialOutput;
    bool m_currentIsError = false;
    bool m_running = false;
    bool m_failed = false;
    bool m_cancelled = false;
    int m_stepCount = 0;
    // The dock's cap on this card's height, in px; 0 means uncapped.
    int m_heightBudget = 0;
    // A reflow is already waiting for the next event-loop turn.
    bool m_reflowQueued = false;
    // Set once startAgent runs. Unlike m_running it stays true after the run,
    // so a late live signal can tell a card that never started (wrong task id)
    // from one that already finished.
    bool m_everStarted = false;
    // Set once the user opens or closes the activity log themselves. From then
    // on the card stops opening and closing it on their behalf.
    bool m_activityUserSet = false;

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
