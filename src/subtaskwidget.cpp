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

    // The badge carries both the delegation glyph and a text label, so the card
    // is identifiable as a sub-agent even when the glyph alone is ambiguous.
    m_badge = new QLabel(i18n("↳ Agent"), this);
    m_badge->setToolTip(i18n("Delegated sub-agent"));
    header->addWidget(m_badge);

    m_icon = new QLabel(u"◐"_s, this);
    m_icon->setStyleSheet(u"font-size: 12px;"_s);
    header->addWidget(m_icon);

    m_title = new QLabel(this);
    m_title->setTextFormat(Qt::RichText);
    m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->addWidget(m_title);

    // The mode the sub-agent runs in, shown as a small chip between the title
    // and the status line. A delegated task is only meaningful with the mode
    // that was chosen for it, and burying it in the title makes it easy to miss.
    m_modeChip = new QLabel(this);
    m_modeChip->setTextFormat(Qt::RichText);
    m_modeChip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_modeChip->setVisible(false);
    header->addWidget(m_modeChip);

    m_status = new QLabel(this);
    m_status->setMinimumWidth(0);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // textPrimary rather than textMuted: the card surface is now a warm yellow,
    // and a grey-on-gold status line drops under a readable contrast.
    m_status->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(ChatTheme::textPrimary()));
    header->addWidget(m_status);
    header->addStretch();

    m_cancel = new QPushButton(i18n("Cancel"), this);
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setFixedHeight(20);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        Q_EMIT cancelRequested(m_taskId);
    });
    header->addWidget(m_cancel);

    root->addLayout(header);

    // A small caption above the body so the user knows whether they are reading
    // the delegated task or the sub-agent's answer.
    m_bodyCaption = new QLabel(this);
    m_bodyCaption->setStyleSheet(QStringLiteral("color: %1; font-size: 9px; font-weight: 600;"
                                                " text-transform: uppercase; letter-spacing: 0.5px;")
                                     .arg(ChatTheme::textMuted()));
    m_bodyCaption->setText(i18n("Task"));
    root->addWidget(m_bodyCaption);

    // The task description the orchestrator wrote, so the user can see what
    // was actually delegated without expanding anything.
    m_answer = new QTextBrowser(this);
    m_answer->setOpenExternalLinks(true);
    m_answer->setFrameShape(QFrame::NoFrame);
    m_answer->setMinimumHeight(0);
    root->addWidget(m_answer);

    // What the sub-agent is doing right now: the newest step it reported. The
    // status line in the header says how long the run has taken and how many
    // steps it has made, which is not the same thing as saying which step it is
    // on -- without this the card reads as a spinner with no reason.
    m_current = new QLabel(this);
    m_current->setWordWrap(false);
    // No colour in the stylesheet: the label uses rich text with inline spans
    // so the "Doing:" prefix and the step text can carry different colours.
    m_current->setStyleSheet(u"font-size: 10px;"_s);
    m_current->setVisible(false);
    root->addWidget(m_current);

    // Live activity log, hidden until the user asks for it.
    m_activityContainer = new QWidget(this);
    auto *activityLayout = new QVBoxLayout(m_activityContainer);
    activityLayout->setContentsMargins(0, 3, 0, 0);
    activityLayout->setSpacing(3);

    m_toggle = new QPushButton(i18n("Activity  ▸"), m_activityContainer);
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->setToolTip(i18n("Show the sub-agent's tool calls and progress"));
    activityLayout->addWidget(m_toggle);

    m_activity = new QTextBrowser(m_activityContainer);
    m_activity->setOpenExternalLinks(false);
    m_activity->setFrameShape(QFrame::NoFrame);
    m_activity->setVisible(false);
    activityLayout->addWidget(m_activity);

    connect(m_toggle, &QPushButton::clicked, this, [this] {
        const bool show = !m_activity->isVisible();
        m_activity->setVisible(show);
        m_toggle->setText(show ? i18n("Activity  ▾") : i18n("Activity  ▸"));
        m_toggle->setToolTip(show ? i18n("Hide the sub-agent's tool calls and progress")
                                  : i18n("Show the sub-agent's tool calls and progress"));
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
    m_bodyCaption->setText(i18n("Task"));
    m_bodyCaption->setVisible(!m_description.isEmpty());
    m_currentLine.clear();
    m_currentIsError = false;
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
    const QString color = isError ? ChatTheme::danger() : ChatTheme::textMuted();
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
    m_currentIsError = isError;
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
    m_bodyCaption->setText(result.ok ? i18n("Result") : i18n("Error"));
    m_bodyCaption->setVisible(true);
    m_currentLine.clear();
    m_currentIsError = false;
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
    m_bodyCaption->setText(i18n("Cancelled"));
    m_bodyCaption->setVisible(true);
    m_currentLine.clear();
    m_currentIsError = false;
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
    const QString elided = m_current->fontMetrics().elidedText(m_currentLine, Qt::ElideRight, available);
    // A prefix label makes it clear this is the current step, not a status.
    // The step text is coloured by whether it is an error or not.
    const QString stepColor = m_currentIsError ? ChatTheme::danger() : ChatTheme::accent();
    m_current->setTextFormat(Qt::RichText);
    m_current->setText(QStringLiteral("<span style=\"color:%1; font-weight:600;\">%2</span>"
                                      " <span style=\"color:%3;\">%4</span>")
                           .arg(ChatTheme::textMuted(), i18n("Doing:"),
                                stepColor, elided.toHtmlEscaped()));
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
    // The agent name is the primary identifier; "Sub-agent" is the role label
    // shown in the badge, so the title no longer repeats it.
    m_title->setText(QStringLiteral("<span style=\"color:%1\">%2</span>")
                         .arg(ChatTheme::textPrimary(), escapedName));

    // The mode chip shows which mode the sub-agent runs in. Hidden when no mode
    // was given, so a modeless delegation stays compact.
    if (m_modeId.isEmpty()) {
        m_modeChip->setVisible(false);
    } else {
        m_modeChip->setText(QStringLiteral("<span style=\"color:%1; font-size:9px;"
                                           " background:%2; border-radius:3px;"
                                           " padding:1px 5px;\">%3</span>")
                                .arg(ChatTheme::accent(), ChatTheme::mixColors(ChatTheme::accent(), ChatTheme::cardBg(), 18),
                                     m_modeId.toHtmlEscaped()));
        m_modeChip->setToolTip(i18n("Mode: %1", m_modeId));
        m_modeChip->setVisible(true);
    }
    updateElapsed();
}

void SubtaskWidget::updateElapsed()
{
    // The status line carries the state, the elapsed time and the step count.
    // The state word is coloured so a finished, failed or cancelled card can be
    // told apart from a running one without reading the text.
    QString stateWord;
    QString stateColor;
    if (m_running) {
        stateWord = i18n("Running");
        stateColor = ChatTheme::accent();
    } else if (m_cancelled) {
        stateWord = i18n("Cancelled");
        stateColor = ChatTheme::textMuted();
    } else if (m_failed) {
        stateWord = i18n("Failed");
        stateColor = ChatTheme::danger();
    } else {
        stateWord = i18n("Done");
        stateColor = ChatTheme::success();
    }

    const qint64 secs = m_elapsed.elapsed() / 1000;
    QString timeStr;
    if (secs < 60) {
        timeStr = QStringLiteral("%1s").arg(secs);
    } else {
        const int mins = static_cast<int>(secs / 60);
        const int remSecs = static_cast<int>(secs % 60);
        timeStr = i18n("%1m %2s", mins, remSecs);
    }

    QStringList parts;
    parts << timeStr;
    if (m_stepCount > 0) {
        parts << i18np("%1 step", "%1 steps", m_stepCount);
    }

    m_status->setTextFormat(Qt::RichText);
    m_status->setText(QStringLiteral("<span style=\"color:%1; font-weight:600;\">%2</span>"
                                     " <span style=\"color:%3;\">·</span> %4")
                          .arg(stateColor, stateWord.toHtmlEscaped(), ChatTheme::textMuted(),
                               parts.join(QStringLiteral(" \xC2\xB7 "))));
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
    QString sheet = ChatTheme::agentTaskCard(u"subtaskWidget"_s);
    sheet += QStringLiteral("QWidget#subtaskWidget { border-color: %1; }"
                            "QWidget#subtaskWidget QLabel { background: transparent; }")
                 .arg(border);
    setStyleSheet(sheet);

    m_badge->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: 700; color: %1;"
                                          " background: %2; border-radius: 4px; padding: 1px 6px;")
                               .arg(ChatTheme::accent(), ChatTheme::mixColors(ChatTheme::accent(), ChatTheme::cardBg(), 18)));
    m_title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: 600; }")
                               .arg(ChatTheme::textPrimary()));
    m_modeChip->setStyleSheet(QStringLiteral("QLabel { background: transparent; }"));
    m_status->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(ChatTheme::textPrimary()));
    m_bodyCaption->setStyleSheet(QStringLiteral("color: %1; font-size: 9px; font-weight: 600;"
                                                " letter-spacing: 0.5px; background: transparent;")
                                     .arg(ChatTheme::textMuted()));
    m_cancel->setStyleSheet(
        QStringLiteral(
            "QPushButton { background: transparent; color: %1; border: 1px solid %2;"
            " border-radius: 5px; padding: 0 8px; font-size: 10px; }"
            "QPushButton:hover { background-color: %3; border-color: %1; }")
        .arg(ChatTheme::danger(), ChatTheme::border(), ChatTheme::hoverBg()));
    m_answer->setStyleSheet(
        QStringLiteral("QTextBrowser { background: transparent; border: none; color: %1; font-size: 11px; padding: 0; }")
            .arg(ChatTheme::textPrimary()));
    m_toggle->setStyleSheet(
        QStringLiteral("QPushButton { background: transparent; color: %1; border: 1px solid %2;"
                       " border-radius: 5px; text-align: left; padding: 2px 7px; font-size: 10px; }"
                       "QPushButton:hover { background-color: %3; }")
            .arg(ChatTheme::accent(), ChatTheme::border(), ChatTheme::hoverBg()));
    m_activity->setStyleSheet(
        QStringLiteral("QTextBrowser { background: %1; border: 1px solid %2; border-radius: 5px;"
                       " color: %3; font-size: 10px; padding: 4px; }")
            .arg(ChatTheme::panelBg(), ChatTheme::border(), ChatTheme::textMuted()));
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
