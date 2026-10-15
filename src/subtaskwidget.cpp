/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "subtaskwidget.h"

#include "agenttext.h"
#include "chattheme.h"

#include "agenttext.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QTextCursor>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{
// Keeps very chatty sub-agents from growing the transcript without bound.
constexpr int MaxActivityLines = 200;
constexpr int MaxActivityChars = 12000;
// How much of the delegated task is shown while the agent works. The answer
// replaces this text at the end anyway, and an unbounded description would
// push the dock (and the input with it) off the screen.
constexpr int RunningDescriptionChars = 400;
} // namespace

SubtaskWidget::SubtaskWidget(const QString &taskId, QWidget *parent)
    : QWidget(parent)
    , m_taskId(taskId)
{
    setObjectName(u"subtaskWidget"_s);
    // Without this, a plain QWidget subclass silently ignores a stylesheet
    // background-color and the card renders fully transparent -- the border and
    // background in ChatTheme::agentTaskCard() never reach the screen.
    setAttribute(Qt::WA_StyledBackground, true);

    // Compact: this card now sits in the intent dock above the input, and the
    // dock also carries approvals. Tighter padding and smaller type let several
    // live sub-agents plus an approval fit without stealing transcript space.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(4);

    // Header: a fixed delegation badge, state icon, "Agent · mode", status,
    // cancel. The fixed badge remains meaningful after the state icon becomes
    // a checkmark or failure mark.
    auto *header = new QHBoxLayout;
    header->setSpacing(6);

    auto *agentBadge = new QLabel(u"↳"_s, this);
    agentBadge->setToolTip(i18n("Delegated sub-agent"));
    agentBadge->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700; color: %1;").arg(ChatTheme::accent()));
    header->addWidget(agentBadge);

    m_icon = new QLabel(u"◐"_s, this);
    m_icon->setStyleSheet(u"font-size: 12px;"_s);
    header->addWidget(m_icon);

    m_title = new QLabel(this);
    m_title->setTextFormat(Qt::RichText);
    m_title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: 600; }")
                                  .arg(ChatTheme::textPrimary()));
    header->addWidget(m_title);

    m_status = new QLabel(this);
    // textPrimary rather than textMuted: the card surface is now a warm yellow,
    // and a grey-on-gold status line drops under a readable contrast.
    m_status->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(ChatTheme::textPrimary()));
    header->addWidget(m_status);
    header->addStretch();

    m_cancel = new QPushButton(i18n("Cancel"), this);
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setFixedHeight(20);
    m_cancel->setStyleSheet(
        QStringLiteral(
            "QPushButton { background: transparent; color: %1; border: 1px solid %2;"
            " border-radius: 5px; padding: 0 8px; font-size: 10px; }"
            "QPushButton:hover { background-color: %3; border-color: %1; }")
        .arg(ChatTheme::danger(), ChatTheme::border(), ChatTheme::hoverBg()));
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        Q_EMIT cancelRequested(m_taskId);
    });
    header->addWidget(m_cancel);

    root->addLayout(header);

    // The task description the orchestrator wrote, so the user can see what
    // was actually delegated without expanding anything.
    m_answer = new QTextBrowser(this);
    m_answer->setOpenExternalLinks(true);
    m_answer->setFrameShape(QFrame::NoFrame);
    m_answer->setStyleSheet(
        QStringLiteral("QTextBrowser { background: transparent; border: none; color: %1; font-size: 11px; padding: 0; }")
            .arg(ChatTheme::textPrimary()));
    m_answer->setMinimumHeight(0);
    root->addWidget(m_answer);

    // What the sub-agent is doing right now: the newest step it reported. The
    // status line in the header says how long the run has taken and how many
    // steps it has made, which is not the same thing as saying which step it is
    // on -- without this the card reads as a spinner with no reason.
    m_current = new QLabel(this);
    m_current->setWordWrap(false);
    m_current->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(ChatTheme::accent()));
    m_current->setVisible(false);
    root->addWidget(m_current);

    // Live activity log, hidden until the user asks for it.
    m_activityContainer = new QWidget(this);
    auto *activityLayout = new QVBoxLayout(m_activityContainer);
    activityLayout->setContentsMargins(0, 3, 0, 0);
    activityLayout->setSpacing(3);

    m_toggle = new QPushButton(i18n("Show activity"), m_activityContainer);
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->setStyleSheet(
        QStringLiteral("QPushButton { background: transparent; color: %1; border: none; text-align: left;"
                       " padding: 1px 0; font-size: 10px; }"
                       "QPushButton:hover { color: #ffffff; }")
            .arg(ChatTheme::accent()));
    activityLayout->addWidget(m_toggle);

    m_activity = new QTextBrowser(m_activityContainer);
    m_activity->setOpenExternalLinks(false);
    m_activity->setFrameShape(QFrame::NoFrame);
    m_activity->setStyleSheet(
        QStringLiteral("QTextBrowser { background: %1; border: 1px solid %2; border-radius: 5px;"
                       " color: %3; font-size: 10px; padding: 4px; }")
            .arg(ChatTheme::panelBg(), ChatTheme::border(), ChatTheme::textMuted()));
    m_activity->setVisible(false);
    activityLayout->addWidget(m_activity);

    connect(m_toggle, &QPushButton::clicked, this, [this] {
        const bool show = !m_activity->isVisible();
        m_activity->setVisible(show);
        m_toggle->setText(show ? i18n("Hide activity") : i18n("Show activity"));
        reflowNow();
    });
    m_expandedActivity = m_activityContainer;

    root->addWidget(m_activityContainer);

    // Drives the live "running · 12s · 5 steps" line.
    m_ticker = new QTimer(this);
    m_ticker->setInterval(1000);
    connect(m_ticker, &QTimer::timeout, this, &SubtaskWidget::updateElapsed);

    m_elapsed.start();
    updateHeader();
    applyStyle();
}

SubtaskWidget::~SubtaskWidget() = default;

void SubtaskWidget::startAgent(const QString &agentName, const QString &modeId, const QString &description)
{
    m_agentName = agentName.isEmpty() ? i18n("Sub-agent") : agentName;
    m_modeId = modeId;
    m_description = description.trimmed();
    m_running = true;
    m_elapsed.restart();
    m_ticker->start();
    // The delegated task is what the card is about, so it stays on screen while
    // the agent works and is replaced by the answer once it arrives. It used to
    // be cleared here, which left the running card saying nothing at all about
    // the work in flight.
    m_answer->setDocument(makeMarkdownDocument(closeMarkdown(m_description.left(RunningDescriptionChars)),
                                               ChatTheme::messageCss()));
    m_answer->setToolTip(m_description);
    m_answer->setVisible(!m_description.isEmpty());
    m_currentLine.clear();
    m_current->clear();
    m_current->setVisible(false);
    updateHeader();
    updateElapsed();
    applyStyle();
    reflowNow();
}

void SubtaskWidget::appendActivity(const QString &line, bool isError)
{
    ++m_stepCount;
    const QString color = isError ? u"#ef4444"_s : u"#9a9a9e"_s;
    QString html = QStringLiteral("<div style=\"color:%1\">%2</div>").arg(color, line.toHtmlEscaped());
    if (m_activity->document()->characterCount() > MaxActivityChars) {
        m_activity->setHtml(i18n("… earlier activity trimmed"));
    }
    // insertHtml at the end keeps the newest line visible without scrolling.
    QTextCursor cursor = m_activity->textCursor();
    cursor.movePosition(QTextCursor::End);
    if (!m_activity->document()->isEmpty() && m_activity->document()->characterCount() > 1) {
        cursor.insertHtml(html);
    } else {
        m_activity->setHtml(html);
    }
    m_activity->verticalScrollBar()->setValue(m_activity->verticalScrollBar()->maximum());
    // Mirror the step onto the card itself: the log below is opt-in, so without
    // this line a running card reported a step count and nothing else.
    m_currentLine = line;
    m_current->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;")
                                 .arg(isError ? ChatTheme::danger() : ChatTheme::accent()));
    updateCurrentActivity();
    updateHeader();
    updateElapsed();
    reflowNow();
}

void SubtaskWidget::finishAgent(const ToolResult &result)
{
    m_running = false;
    m_failed = !result.ok && !result.cancelled;
    m_cancelled = result.cancelled;
    if (m_ticker) {
        m_ticker->stop();
    }
    m_cancel->setVisible(false);
    // A sub-agent's answer is markdown like any assistant turn, so it gets the
    // same body table styling for any fenced code it contains.
    m_answer->setDocument(makeMarkdownDocument(closeMarkdown(result.output), ChatTheme::messageCss()));
    m_answer->setToolTip(QString());
    m_answer->setVisible(true);
    m_currentLine.clear();
    m_current->setVisible(false);
    updateHeader();
    applyStyle();
    reflowNow();
}

void SubtaskWidget::markAbandoned(const QString &reason)
{
    m_running = false;
    m_cancelled = true;
    if (m_ticker) {
        m_ticker->stop();
    }
    m_cancel->setVisible(false);
    m_answer->setDocument(makeMarkdownDocument(closeMarkdown(reason), ChatTheme::messageCss()));
    m_answer->setToolTip(QString());
    m_answer->setVisible(true);
    m_currentLine.clear();
    m_current->setVisible(false);
    updateHeader();
    applyStyle();
    reflowNow();
}

void SubtaskWidget::updateCurrentActivity()
{
    if (m_currentLine.isEmpty()) {
        m_current->setVisible(false);
        return;
    }
    // Elided to the card's own width: a raw step line can be long (a summary of
    // the arguments), and a label wider than the card would stretch the dock.
    // The full text stays reachable through the tooltip.
    const int available = qMax(80, width() - 24);
    m_current->setText(m_current->fontMetrics().elidedText(m_currentLine, Qt::ElideRight, available));
    m_current->setToolTip(m_currentLine);
    m_current->setVisible(true);
}

void SubtaskWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The live step is elided against the card width, so it has to be measured
    // again whenever that width changes.
    updateCurrentActivity();
}

void SubtaskWidget::updateHeader()
{
    const QString escapedName = m_agentName.toHtmlEscaped();
    const QString name = QStringLiteral("<span style=\"color:#b9b9c0\">%1</span>").arg(escapedName);
    m_title->setText(m_modeId.isEmpty()
                         ? i18n("Sub-agent · %1", name)
                         : QStringLiteral("<span style=\"color:#7aa2f7\">%1</span> <span style=\"color:#666;\">·</span> %2")
                               .arg(m_modeId.toHtmlEscaped(), i18n("Sub-agent · %1", name)));
    updateElapsed();
}

void SubtaskWidget::updateElapsed()
{
    QStringList parts;
    if (m_running) {
        parts << i18n("running %1s", m_elapsed.elapsed() / 1000);
    } else if (m_cancelled) {
        parts << i18n("cancelled after %1s", m_elapsed.elapsed() / 1000);
    } else if (m_failed) {
        parts << i18n("failed after %1s", m_elapsed.elapsed() / 1000);
    } else {
        parts << i18n("done in %1s", m_elapsed.elapsed() / 1000);
    }
    if (m_stepCount > 0) {
        // i18np() is the plural-aware KLocalizedString helper. Note also that
        // arguments are passed to i18n()/i18np() directly rather than chained
        // with QString::arg(): KI18n substitutes %1 itself, and a chained
        // .arg() leaves a raw N_ARGUMENT_MISSING error in the string.
        parts << i18np("%1 step", "%1 steps", m_stepCount);
    }
    m_status->setText(parts.join(QStringLiteral(" \xC2\xB7 ")));
}

void SubtaskWidget::applyStyle()
{
    // A delegated agent task is a distinct kind of thing from a tool the
    // orchestrator ran itself, so it gets a full box: a raised background plus
    // a border on all four sides. The border colour still carries the outcome,
    // so state stays readable at a glance.
    QString border = ChatTheme::border();
    if (m_running) {
        border = ChatTheme::accent();
    } else if (m_failed) {
        border = ChatTheme::danger();
    } else if (m_cancelled) {
        border = ChatTheme::textMuted();
    } else {
        border = ChatTheme::success();
    }

    const QString glyph = m_running ? u"◐"_s : (m_failed ? u"✗"_s : (m_cancelled ? u"⊘"_s : u"✓"_s));
    m_icon->setText(glyph);
    m_icon->setStyleSheet(QStringLiteral("font-size: 12px; color:%1;").arg(border));

    // Background from the theme, border tinted by state.
    QString sheet = ChatTheme::agentTaskCard();
    sheet += QStringLiteral("QWidget#subtaskWidget { border-color: %1; }"
                            "QWidget#subtaskWidget QLabel { background: transparent; }")
                 .arg(border);
    setStyleSheet(sheet);
}

void SubtaskWidget::setActivityVisible(bool visible)
{
    if (m_activity) {
        m_activity->setVisible(visible);
    }
}

void SubtaskWidget::reflowNow()
{
    if (m_answer->isVisible()) {
        m_answer->setMinimumHeight(0);
        m_answer->document()->setTextWidth(m_answer->viewport()->width());
        m_answer->setMinimumHeight(m_answer->document()->size().height());
    }
    if (m_activity && m_activity->isVisible()) {
        m_activity->document()->setTextWidth(m_activity->viewport()->width());
        m_activity->setMinimumHeight(m_activity->document()->size().height());
    }
    updateGeometry();
}

} // namespace KateAi
