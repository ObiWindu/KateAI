/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "turnstatus.h"

#include "chattheme.h"

#include <KLocalizedString>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// Four dots at four phases reads as motion without a spinning widget, and it
// keeps the strip from reflowing as the glyph width changes.
QString spinnerFrame(int tick)
{
    static const char *kFrames[] = {"\xe2\x97\x81", "\xe2\x97\x83", "\xe2\x97\x8b", "\xe2\x97\x87", "\xe2\x97\x8d", "\xe2\x97\x89"};
    return QString::fromUtf8(kFrames[tick % 6]);
}

QString formatTokens(int tokens)
{
    if (tokens >= 1000) {
        return QStringLiteral("%1k").arg(tokens / 1000.0, 0, 'f', 1);
    }
    return QString::number(tokens);
}

} // namespace

TurnStatus::TurnStatus(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"turnStatus"_s);
    setCursor(Qt::PointingHandCursor);
    setToolTip(i18n("Scroll to the newest message"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(7);

    m_spinner = new QLabel(this);
    m_spinner->setFixedWidth(10);
    m_spinner->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 12px; }").arg(ChatTheme::accent()));
    layout->addWidget(m_spinner);

    m_activity = new QLabel(this);
    m_activity->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; }").arg(ChatTheme::textPrimary()));
    layout->addWidget(m_activity);

    m_stats = new QLabel(this);
    m_stats->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; }").arg(ChatTheme::textMuted()));
    layout->addWidget(m_stats);

    // Idle hint shares the left edge with the running line, so the strip does
    // not appear to jump when a turn starts.
    m_idle = new QLabel(this);
    m_idle->setStyleSheet(ChatTheme::hintLabel());
    layout->addWidget(m_idle);

    layout->addStretch();

    m_spinner->hide();
    m_activity->hide();
    m_stats->hide();

    m_ticker = new QTimer(this);
    m_ticker->setInterval(120);
    connect(m_ticker, &QTimer::timeout, this, [this] {
        m_spinner->setText(spinnerFrame(m_elapsed.elapsed() / 120));
        refresh();
    });
    m_spinner->setText(spinnerFrame(0));
}

bool TurnStatus::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease && watched == this) {
        Q_EMIT scrollToLatestRequested();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void TurnStatus::setIdleText(const QString &text)
{
    m_idle->setText(text);
    m_idle->setStyleSheet(ChatTheme::hintLabel());
    m_error = false;
}

void TurnStatus::setBusy(bool busy)
{
    if (busy == m_busy) {
        return;
    }
    m_busy = busy;
    if (busy) {
        m_elapsed.start();
        m_ticker->start();
    } else {
        m_ticker->stop();
        m_subtasks = 0;
        m_completedTools = 0;
        m_tokens = 0;
        m_activityText.clear();
        m_error = false;
    }
    refresh();
}

void TurnStatus::setActivity(const QString &activity)
{
    m_activityText = activity;
    m_message.clear();
    refresh();
}

void TurnStatus::setSubtaskCount(int running)
{
    m_subtasks = running;
    refresh();
}

void TurnStatus::setCompletedToolCount(int completed)
{
    m_completedTools = completed;
    refresh();
}

void TurnStatus::setTokenCount(int tokens)
{
    m_tokens = tokens;
    refresh();
}

void TurnStatus::flash(const QString &message, bool isError)
{
    m_message = message;
    m_error = isError;
    refresh();
}

void TurnStatus::reset()
{
    m_message.clear();
    m_error = false;
    m_activityText.clear();
    m_subtasks = 0;
    m_completedTools = 0;
    m_tokens = 0;
    setBusy(false);
}

QString TurnStatus::elapsedText() const
{
    const qint64 ms = m_elapsed.elapsed();
    if (ms < 60000) {
        return QStringLiteral("%1s").arg(ms / 1000);
    }
    const qint64 totalSeconds = ms / 1000;
    return QStringLiteral("%1m %2s").arg(totalSeconds / 60).arg(totalSeconds % 60);
}

void TurnStatus::refresh()
{
    // A transient message outranks the running state: a failed model list is
    // more urgent than "Reading…".
    if (!m_message.isEmpty()) {
        m_idle->setText(m_message);
        m_idle->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; }")
                                  .arg(m_error ? ChatTheme::danger() : ChatTheme::textPrimary()));
        m_spinner->hide();
        m_activity->hide();
        m_stats->hide();
        return;
    }

    if (!m_busy) {
        m_idle->setStyleSheet(ChatTheme::hintLabel());
        m_spinner->hide();
        m_activity->hide();
        m_stats->hide();
        return;
    }

    m_idle->clear();
    m_spinner->show();
    // Paint the first frame immediately: the ticker has a 120ms interval, and
    // an empty label would leave the strip without its leading glyph.
    if (m_spinner->text().isEmpty()) {
        m_spinner->setText(spinnerFrame(m_elapsed.elapsed() / 120));
    }
    m_activity->show();
    m_stats->show();

    QStringList parts;
    parts << elapsedText();
    if (!m_activityText.isEmpty()) {
        parts << m_activityText;
    }
    if (m_subtasks > 0) {
        parts << i18np("%1 sub-agent", "%1 sub-agents", m_subtasks);
    }
    if (m_completedTools > 0) {
        parts << i18np("%1 tool", "%1 tools", m_completedTools);
    }
    if (m_tokens > 0) {
        parts << i18n("%1 tokens", formatTokens(m_tokens));
    }
    m_activity->setText(parts.join(QStringLiteral("  ·  ")));
}

} // namespace KateAi