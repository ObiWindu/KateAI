/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "subtaskwidget.h"

#include "agenttext.h"
#include "chattheme.h"

#include <KLocalizedString>

#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QResizeEvent>
#include <QStringList>
#include <QTextBrowser>
#include <QTextCursor>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{
// Keeps very chatty sub-agents from growing the transcript without bound.
constexpr int MaxActivityLines = 200;
constexpr int MaxActivityChars = 12000;
// The live answer is replaced by the final result. Keeping the whole stream in
// the card duplicates what the parent already holds and grows without bound.
constexpr qsizetype MaxStreamedTextChars = 256 * 1024;
constexpr qsizetype MaxPendingStreamChars = 64 * 1024;
// How much of the delegated task is shown while the agent works. The answer
// replaces this text at the end anyway, and an unbounded description would
// push the dock (and the input with it) off the screen.
constexpr int RunningDescriptionChars = 400;

// Mode ids are stable identifiers ("code", "architect"), not display text.
// Show a readable form without turning a custom id into a translation.
QString displayMode(const QString &id)
{
    QString text = id.trimmed();
    if (text.isEmpty()) {
        return text;
    }
    text[0] = text[0].toUpper();
    text.replace(u'_', u' ');
    return text;
}

struct ActivityParts {
    QString prefix;
    QString tool;
    QString detail;
    bool isFailure = false;
};

// The activity feed carries three shapes: a tool starting ("→ bash ls"), a
// tool finishing ("  ✓ bash" / "  ✗ bash"), and a plain status note. Splitting
// them lets the headline colour and label the tool instead of printing an
// arrow and a raw protocol string.
ActivityParts splitActivityLine(const QString &line)
{
    ActivityParts parts;
    QString rest = line;
    bool done = false;
    bool failed = false;
    if (rest.startsWith(u"→ "_s)) {
        rest = rest.mid(2);
    } else if (rest.startsWith(u"  ✓ "_s)) {
        rest = rest.mid(4);
        done = true;
    } else if (rest.startsWith(u"  ✗ "_s)) {
        rest = rest.mid(4);
        failed = true;
    }

    if (done || failed || line.startsWith(u"→ "_s)) {
        const int space = rest.indexOf(u' ');
        parts.tool = (space < 0) ? rest : rest.left(space);
        parts.detail = (space < 0) ? QString() : rest.mid(space + 1).trimmed();
        parts.isFailure = failed;
        parts.prefix = failed ? i18n("Failed:") : (done ? i18n("Done:") : i18n("Doing:"));
    } else {
        // A plain note ("Starting…", "Waiting for approval: …") is its own
        // sentence; a generic "Status:" prefix only adds noise.
        parts.detail = line.trimmed();
    }
    return parts;
}
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
    m_badge->setObjectName(u"subtaskBadge"_s);
    m_badge->setToolTip(i18n("Delegated sub-agent"));
    header->addWidget(m_badge);

    m_icon = new QLabel(u"◐"_s, this);
    m_icon->setObjectName(u"subtaskState"_s);
    m_icon->setStyleSheet(u"font-size: 12px;"_s);
    header->addWidget(m_icon);

    m_title = new QLabel(this);
    m_title->setObjectName(u"subtaskTitle"_s);
    m_title->setTextFormat(Qt::RichText);
    m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_title->setToolTip(i18n("Sub-agent"));
    header->addWidget(m_title);

    // The mode the sub-agent runs in, shown as a small chip between the title
    // and the status line. A delegated task is only meaningful with the mode
    // that was chosen for it, and burying it in the title makes it easy to miss.
    m_modeChip = new QLabel(this);
    m_modeChip->setObjectName(u"subtaskMode"_s);
    m_modeChip->setTextFormat(Qt::RichText);
    m_modeChip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_modeChip->setVisible(false);
    header->addWidget(m_modeChip);

    m_status = new QLabel(this);
    m_status->setObjectName(u"subtaskStatus"_s);
    m_status->setMinimumWidth(0);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setToolTip(i18n("Sub-agent state"));
    // textPrimary rather than textMuted: the card surface is now a warm yellow,
    // and a grey-on-gold status line drops under a readable contrast.
    m_status->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(ChatTheme::textPrimary()));
    header->addWidget(m_status);
    header->addStretch();

    m_cancel = new QPushButton(i18n("Cancel"), this);
    m_cancel->setObjectName(u"subtaskCancel"_s);
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setFixedHeight(20);
    m_cancel->setToolTip(i18n("Stop this sub-agent"));
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        Q_EMIT cancelRequested(m_taskId);
    });
    header->addWidget(m_cancel);

    root->addLayout(header);

    // A small caption above the body so the user knows whether they are reading
    // the delegated task or the sub-agent's answer.
    m_bodyCaption = new QLabel(this);
    m_bodyCaption->setObjectName(u"subtaskCaption"_s);
    m_bodyCaption->setStyleSheet(QStringLiteral("color: %1; font-size: 9px; font-weight: 600;"
                                                " text-transform: uppercase; letter-spacing: 0.5px;")
                                     .arg(ChatTheme::textMuted()));
    m_bodyCaption->setText(i18n("Task"));
    root->addWidget(m_bodyCaption);

    // The task description the orchestrator wrote, so the user can see what
    // was actually delegated without expanding anything.
    m_answer = new QTextBrowser(this);
    m_answer->setObjectName(u"subtaskBody"_s);
    m_answer->setOpenExternalLinks(false);
    connect(m_answer, &QTextBrowser::anchorClicked, this, [](const QUrl &url) { openSafeExternalUrl(url); });
    m_answer->setFrameShape(QFrame::NoFrame);
    m_answer->setMinimumHeight(0);
    root->addWidget(m_answer);

    // What the sub-agent is doing right now: the newest step it reported. The
    // status line in the header says how long the run has taken and how many
    // tools it has used, which is not the same thing as saying which step it is
    // on -- without this the card reads as a spinner with no reason.
    m_current = new QLabel(this);
    m_current->setObjectName(u"subtaskCurrent"_s);
    m_current->setWordWrap(false);
    // No colour in the stylesheet: the label uses rich text with inline spans
    // so the "Doing:" prefix and the step text can carry different colours.
    m_current->setStyleSheet(u"font-size: 10px;"_s);
    m_current->setVisible(false);
    root->addWidget(m_current);

    // Live activity log, hidden until the user asks for it. The toggle shares
    // its row with Copy, so the result is one click away without another row
    // eating the dock's height.
    m_activityContainer = new QWidget(this);
    m_activityContainer->setObjectName(u"subtaskActivityContainer"_s);
    auto *activityLayout = new QVBoxLayout(m_activityContainer);
    activityLayout->setContentsMargins(0, 3, 0, 0);
    activityLayout->setSpacing(3);

    auto *activityFooter = new QHBoxLayout;
    activityFooter->setContentsMargins(0, 0, 0, 0);
    activityFooter->setSpacing(6);

    m_toggle = new QPushButton(i18n("Activity  ▸"), m_activityContainer);
    m_toggle->setObjectName(u"subtaskActivityToggle"_s);
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->setToolTip(i18n("Show the sub-agent's tool calls and progress"));
    activityFooter->addWidget(m_toggle);
    activityFooter->addStretch();

    m_copyResult = new QPushButton(i18n("Copy"), m_activityContainer);
    m_copyResult->setObjectName(u"subtaskCopyResult"_s);
    m_copyResult->setCursor(Qt::PointingHandCursor);
    m_copyResult->setToolTip(i18n("Copy the sub-agent's answer"));
    m_copyResult->setVisible(false);
    connect(m_copyResult, &QPushButton::clicked, this, [this] {
        if (m_resultOutput.isEmpty() || !QGuiApplication::clipboard()) {
            return;
        }
        QGuiApplication::clipboard()->setText(m_resultOutput);
        m_copyResult->setText(i18n("Copied"));
        QTimer::singleShot(1200, m_copyResult, [thisWeak = QPointer<SubtaskWidget>(this)] {
            if (thisWeak && thisWeak->m_copyResult) {
                thisWeak->m_copyResult->setText(i18n("Copy"));
            }
        });
    });
    activityFooter->addWidget(m_copyResult);
    activityLayout->addLayout(activityFooter);

    // Isolated worktree result: nothing is merged until the user chooses.
    auto *worktreeRow = new QHBoxLayout;
    worktreeRow->setContentsMargins(0, 0, 0, 0);
    worktreeRow->setSpacing(6);
    m_worktreeNote = new QLabel(this);
    m_worktreeNote->setObjectName(u"subtaskWorktreeNote"_s);
    m_worktreeNote->setWordWrap(true);
    m_worktreeNote->setVisible(false);
    worktreeRow->addWidget(m_worktreeNote, 1);
    m_applyWorktree = new QPushButton(i18n("Apply patch"), this);
    m_applyWorktree->setObjectName(u"subtaskApplyWorktree"_s);
    m_applyWorktree->setCursor(Qt::PointingHandCursor);
    m_applyWorktree->setVisible(false);
    m_discardWorktree = new QPushButton(i18n("Discard"), this);
    m_discardWorktree->setObjectName(u"subtaskDiscardWorktree"_s);
    m_discardWorktree->setCursor(Qt::PointingHandCursor);
    m_discardWorktree->setVisible(false);
    worktreeRow->addWidget(m_applyWorktree);
    worktreeRow->addWidget(m_discardWorktree);
    activityLayout->addLayout(worktreeRow);
    connect(m_applyWorktree, &QPushButton::clicked, this, [this] {
        Q_EMIT applyWorktreeRequested(m_taskId);
    });
    connect(m_discardWorktree, &QPushButton::clicked, this, [this] {
        Q_EMIT discardWorktreeRequested(m_taskId);
    });

    m_activity = new QTextBrowser(m_activityContainer);
    m_activity->setObjectName(u"subtaskActivity"_s);
    m_activity->setOpenExternalLinks(false);
    m_activity->setFrameShape(QFrame::NoFrame);
    m_activity->setVisible(false);
    activityLayout->addWidget(m_activity);

    connect(m_toggle, &QPushButton::clicked, this, [this] {
        m_activityUserSet = true;
        setActivityVisible(m_activity->isHidden());
    });
    m_expandedActivity = m_activityContainer;

    root->addWidget(m_activityContainer);

    // Drives the live "running · 12s · 5 steps" line.
    m_ticker = new QTimer(this);
    m_ticker->setInterval(1000);
    connect(m_ticker, &QTimer::timeout, this, &SubtaskWidget::updateElapsed);

    // Child tool output can arrive in hundreds of small chunks. Coalescing the
    // complete lines keeps the activity document from being rebuilt per chunk
    // while still reading as live.
    m_streamTimer = new QTimer(this);
    m_streamTimer->setSingleShot(true);
    m_streamTimer->setInterval(80);
    connect(m_streamTimer, &QTimer::timeout, this, &SubtaskWidget::flushStreamOutput);

    // The answer body is a Markdown document, and rebuilding it costs far more
    // than appending the delta. A child that streams hundreds of tokens per
    // second rebuilt the whole document for each one, so the text is
    // accumulated and rendered at most every animation interval.
    m_answerTimer = new QTimer(this);
    m_answerTimer->setSingleShot(true);
    m_answerTimer->setInterval(80);
    connect(m_answerTimer, &QTimer::timeout, this, &SubtaskWidget::flushStreamedAnswer);

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
    setMarkdownContent(m_answer, closeMarkdown(m_description.left(RunningDescriptionChars)),
                       ChatTheme::messageCss());
    m_answer->setToolTip(m_description);
    m_answer->setVisible(!m_description.isEmpty());
    m_bodyCaption->setText(i18n("Task"));
    m_bodyCaption->setVisible(!m_description.isEmpty());
    m_currentLine.clear();
    m_streamedMessage.clear();
    m_pendingStreamed.clear();
    m_partialOutput.clear();
    m_pendingStreamDelta.clear();
    m_stepCount = 0;
    m_toolCount = 0;
    m_errorCount = 0;
    m_resultOutput.clear();
    if (m_streamTimer) {
        m_streamTimer->stop();
    }
    m_currentIsError = false;
    // Never show a blank card while the child is spinning up: until its first
    // tool report arrives the headline names the wait itself.
    m_currentLine = i18n("Starting…");
    m_current->clear();
    updateCurrentActivity();
    if (m_copyResult) {
        m_copyResult->setVisible(false);
        m_copyResult->setText(i18n("Copy"));
    }
    if (m_applyWorktree) {
        m_applyWorktree->setVisible(false);
        m_applyWorktree->setEnabled(true);
    }
    if (m_discardWorktree) {
        m_discardWorktree->setVisible(false);
        m_discardWorktree->setEnabled(true);
    }
    if (m_worktreeNote) {
        m_worktreeNote->clear();
        m_worktreeNote->setVisible(false);
    }
    // A fresh run starts a fresh log; the previous run's steps left with its
    // card. The disclosure starts closed and opens itself on the first step,
    // unless the user has started choosing for themselves.
    m_activityLog.clear();
    m_activityUserSet = false;
    rebuildActivityLog();
    setActivityVisible(false);
    updateHeader();
    updateElapsed();
    applyStyle();
    reflowNow();
    // Deltas that arrived between toolStarted and subtaskStarted were buffered;
    // replay them now that the card is running.
    if (!m_pendingStreamed.isEmpty()) {
        const QString buffered = m_pendingStreamed;
        m_pendingStreamed.clear();
        appendMessageDelta(buffered);
    }
}

void SubtaskWidget::setWorktreeReady(const QStringList &filesChanged)
{
    if (!m_applyWorktree || !m_discardWorktree || !m_worktreeNote) {
        return;
    }
    m_worktreeNote->setText(i18n("Isolated worktree: %1 file(s) changed. Nothing was merged.",
                                 filesChanged.size()));
    m_worktreeNote->setToolTip(filesChanged.join(u"\n"_s));
    m_worktreeNote->setVisible(true);
    m_applyWorktree->setVisible(true);
    m_discardWorktree->setVisible(true);
    reflowNow();
}

void SubtaskWidget::setWorktreeResolved(bool applied)
{
    if (m_applyWorktree) {
        m_applyWorktree->setVisible(false);
    }
    if (m_discardWorktree) {
        m_discardWorktree->setVisible(false);
    }
    if (m_worktreeNote) {
        m_worktreeNote->setText(applied ? i18n("Worktree patch applied.")
                                        : i18n("Worktree patch discarded."));
        m_worktreeNote->setVisible(true);
    }
    reflowNow();
}

void SubtaskWidget::appendActivity(const QString &line, bool isError)
{
    // A new tool starts with "→ tool …". Flush the previous tool's output
    // first, then forget the old cumulative buffer so the next stream is
    // measured from zero instead of being mistaken for a continuation.
    if (line.startsWith(u"→"_s) || line.startsWith(u"  ✓"_s) || line.startsWith(u"  ✗"_s)) {
        flushStreamOutput();
        m_partialOutput.clear();
        m_pendingStreamDelta.clear();
    }
    if (line.startsWith(u"→ "_s)) {
        ++m_toolCount;
    }
    if (isError) {
        ++m_errorCount;
    }
    appendLogLine(line, isError, true);
}

void SubtaskWidget::appendLogLine(const QString &line, bool isError, bool countStep)
{
    if (countStep) {
        ++m_stepCount;
    }
    // The log is one markdown document rebuilt from the accumulated lines: a
    // sub-agent that summarises its step with emphasis, inline code or a
    // fenced block renders, instead of the card showing literal asterisks
    // and backticks. Capped by lines first, then by characters.
    m_activityLog.append(qMakePair(line, isError));
    while (m_activityLog.size() > MaxActivityLines) {
        m_activityLog.removeFirst();
    }
    rebuildActivityLog();
    // The log is where the data is, so it opens itself on the sub-agent's first
    // step: a running card should show the work, not a disclosure hiding it.
    // From then on the user decides.
    if (!m_activityUserSet && !m_activityLog.isEmpty()) {
        setActivityVisible(true);
    }
    // Mirror the step onto the card itself: the Doing: line names what the
    // sub-agent is on even once the log below is closed again. Tool output
    // lines do not replace the tool's own Doing: label.
    if (countStep) {
        m_currentLine = line;
        m_currentIsError = isError;
        updateCurrentActivity();
    }
    updateHeader();
    updateElapsed();
    reflowNow();
}

void SubtaskWidget::flushStreamOutput()
{
    if (m_pendingStreamDelta.isEmpty()) {
        return;
    }
    const QString pending = m_pendingStreamDelta;
    const int lastNewline = pending.lastIndexOf(u'\n');
    // Keep the incomplete tail for the next chunk; one line of output is not
    // enough to re-render on every arrival.
    if (lastNewline < 0) {
        return;
    }
    m_pendingStreamDelta = pending.mid(lastNewline + 1);
    const QStringList lines = pending.left(lastNewline).split(u'\n');
    m_streamTimer->stop();

    QStringList rendered;
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        rendered.append(u"│ "_s + line);
    }
    if (rendered.isEmpty()) {
        return;
    }

    // Batch the whole flush into one document rebuild.
    for (const QString &line : std::as_const(rendered)) {
        m_activityLog.append(qMakePair(line, false));
        while (m_activityLog.size() > MaxActivityLines) {
            m_activityLog.removeFirst();
        }
    }
    rebuildActivityLog();
    if (!m_activityUserSet) {
        setActivityVisible(true);
    }
    updateHeader();
    updateElapsed();
    reflowNow();
}

void SubtaskWidget::rebuildActivityLog()
{
    auto markdown = [this] {
        QString joined;
        for (const auto &entry : std::as_const(m_activityLog)) {
            if (entry.second) {
                joined += u"**%1** %2\n"_s.arg(i18n("Error:"), entry.first);
            } else {
                joined += entry.first + u'\n';
            }
        }
        return joined;
    };
    QString joined = markdown();
    while (joined.size() > MaxActivityChars && m_activityLog.size() > 1) {
        m_activityLog.removeFirst();
        joined = markdown();
    }
    setMarkdownContent(m_activity, closeMarkdown(joined), ChatTheme::messageCss());
    m_activity->verticalScrollBar()->setValue(m_activity->verticalScrollBar()->maximum());
    updateActivityToggle();
}

void SubtaskWidget::appendMessageDelta(const QString &delta)
{
    if (delta.isEmpty()) {
        return;
    }
    if (!m_running) {
        // The card exists (toolStarted) but the agent has not started yet
        // (subtaskStarted): buffer instead of dropping, so early streaming
        // text survives. This buffer is short-lived, but a stalled stream must
        // not grow it without bound.
        const qsizetype room = MaxPendingStreamChars - m_pendingStreamed.size();
        if (room > 0) {
            m_pendingStreamed += delta.size() > room ? delta.left(room) : delta;
        }
        return;
    }
    const qsizetype room = MaxStreamedTextChars - m_streamedMessage.size();
    if (room <= 0) {
        return;
    }
    m_streamedMessage += delta.size() > room ? delta.left(room) : delta;
    m_answer->setToolTip(QString());
    m_answer->setVisible(true);
    m_bodyCaption->setText(i18n("Progress"));
    m_bodyCaption->setVisible(true);
    // Coalesce: the document is rebuilt by flushStreamedAnswer(), which also
    // runs when the run settles so no text is ever left unrendered.
    m_answerDirty = true;
    if (!m_answerTimer->isActive()) {
        m_answerTimer->start();
    }
}

void SubtaskWidget::flushStreamedAnswer()
{
    m_answerTimer->stop();
    if (!m_answerDirty) {
        return;
    }
    m_answerDirty = false;
    setMarkdownContent(m_answer, closeMarkdown(m_streamedMessage), ChatTheme::messageCss());
    reflowNow();
}

void SubtaskWidget::setPartialOutput(const QString &partial)
{
    if (partial.isEmpty() || !m_running) {
        // Buffer the latest chunk when the card is not running yet; it will
        // surface as the Doing: line once the run starts.
        if (!partial.isEmpty() && !m_running) {
            m_partialOutput = partial;
        }
        return;
    }
    const QString previous = m_partialOutput;
    m_partialOutput = partial;

    // Feed complete lines into the Activity log as the command runs. Child
    // tools stream either the accumulated output or a fragment; handle both so
    // the log never repeats the whole accumulated buffer on every chunk.
    QString delta;
    if (previous.isEmpty()) {
        delta = partial;
    } else if (partial.startsWith(previous)) {
        delta = partial.mid(previous.size());
    } else if (previous.startsWith(partial)) {
        delta.clear();
    } else if (partial.contains(previous)) {
        delta = partial.mid(partial.indexOf(previous) + previous.size());
    } else {
        delta = partial;
    }
    if (!delta.isEmpty()) {
        m_pendingStreamDelta += delta;
        if (m_pendingStreamDelta.size() > MaxActivityChars) {
            m_pendingStreamDelta = m_pendingStreamDelta.right(MaxActivityChars);
        }
        if (!m_streamTimer->isActive()) {
            m_streamTimer->start();
        }
    }

    // Show the tail of the live output as the current step, so a long command
    // reads as it runs instead of only the tool name.
    QString tail = partial.trimmed();
    const int newline = tail.lastIndexOf(u'\n');
    if (newline >= 0) {
        tail = tail.mid(newline + 1);
    }
    tail = tail.left(200).trimmed();
    if (!tail.isEmpty()) {
        m_currentLine = tail;
        m_currentIsError = false;
        updateCurrentActivity();
        updateElapsed();
        reflowNow();
    }
}

void SubtaskWidget::finishAgent(const ToolResult &result)
{
    m_running = false;
    m_failed = !result.ok && !result.cancelled;
    m_cancelled = result.cancelled;
    // Keep the last complete output lines that were still coalescing.
    flushStreamOutput();
    if (m_streamTimer) {
        m_streamTimer->stop();
    }
    // The answer is rendered below from the final result, so a pending
    // coalesced render must not fire afterwards and overwrite it.
    m_answerDirty = false;
    if (m_answerTimer) {
        m_answerTimer->stop();
    }
    m_pendingStreamed.clear();
    m_pendingStreamDelta.clear();
    m_partialOutput.clear();
    if (m_ticker) {
        m_ticker->stop();
    }
    m_cancel->setVisible(false);
    // A sub-agent's answer is markdown like any assistant turn, so it gets the
    // same body table styling for any fenced code it contains.
    m_resultOutput = result.output;
    setMarkdownContent(m_answer, closeMarkdown(result.output), ChatTheme::messageCss());
    m_answer->setToolTip(QString());
    m_answer->setVisible(true);
    if (m_cancelled) {
        m_bodyCaption->setText(i18n("Cancelled"));
    } else {
        m_bodyCaption->setText(result.ok ? i18n("Result") : i18n("Error"));
    }
    m_bodyCaption->setVisible(true);
    if (m_copyResult) {
        m_copyResult->setVisible(!m_resultOutput.trimmed().isEmpty());
        m_copyResult->setText(i18n("Copy"));
    }
    // The run is over, so the card's subject is the answer: the step-by-step
    // log folds away with it (unless the user opened it and wants it kept).
    if (!m_activityUserSet) {
        setActivityVisible(false);
    }
    m_currentLine.clear();
    m_streamedMessage.clear();
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
    flushStreamOutput();
    if (m_streamTimer) {
        m_streamTimer->stop();
    }
    m_answerDirty = false;
    if (m_answerTimer) {
        m_answerTimer->stop();
    }
    m_pendingStreamDelta.clear();
    m_partialOutput.clear();
    if (m_ticker) {
        m_ticker->stop();
    }
    m_cancel->setVisible(false);
    m_resultOutput = reason;
    setMarkdownContent(m_answer, closeMarkdown(reason), ChatTheme::messageCss());
    m_answer->setToolTip(QString());
    m_answer->setVisible(true);
    m_bodyCaption->setText(i18n("Cancelled"));
    m_bodyCaption->setVisible(true);
    if (m_copyResult) {
        m_copyResult->setVisible(!m_resultOutput.trimmed().isEmpty());
        m_copyResult->setText(i18n("Copy"));
    }
    // Same as a finished run: with the steps behind it, the log folds away.
    if (!m_activityUserSet) {
        setActivityVisible(false);
    }
    m_currentLine.clear();
    m_streamedMessage.clear();
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
    const ActivityParts parts = splitActivityLine(m_currentLine);
    // Elided to the card's own width: a raw step line can be long (a summary of
    // the arguments), and a label wider than the card would stretch the dock.
    // The full text stays reachable through the tooltip.
    const int available = qMax(80, width() - 24);
    const QString detail = parts.detail.isEmpty()
        ? QString()
        : m_current->fontMetrics().elidedText(parts.detail, Qt::ElideRight, available);
    // The tool's own colour is the one thing that says *what kind* of work this
    // is (read, edit, command) without spelling it out; a failure overrides it.
    const QString toolColor = m_currentIsError || parts.isFailure
        ? ChatTheme::danger()
        : ChatTheme::toolColor(parts.tool);
    QString tool;
    if (!parts.tool.isEmpty()) {
        tool = QStringLiteral("<span style=\"color:%1; font-weight:600;\">%2</span>")
                   .arg(toolColor, parts.tool.toHtmlEscaped());
    }
    QString body = tool;
    if (!detail.isEmpty()) {
        if (!body.isEmpty()) {
            body += u' ';
        }
        body += QStringLiteral("<span style=\"color:%1;\">%2</span>")
                    .arg(m_currentIsError ? ChatTheme::danger() : ChatTheme::textPrimary(), detail.toHtmlEscaped());
    }
    if (body.isEmpty()) {
        body = QStringLiteral("<span style=\"color:%1;\">%2</span>")
                   .arg(m_currentIsError ? ChatTheme::danger() : ChatTheme::textPrimary(),
                        m_currentLine.toHtmlEscaped());
    }
    // A prefix label makes it clear what the row is: work in flight or a step
    // that finished. A plain status note needs no prefix.
    QString prefix;
    if (!parts.prefix.isEmpty()) {
        prefix = QStringLiteral("<span style=\"color:%1; font-weight:600;\">%2</span> ")
                     .arg(ChatTheme::textMuted(), parts.prefix);
    }
    m_current->setTextFormat(Qt::RichText);
    m_current->setText(prefix + body);
    m_current->setToolTip(m_currentLine);
    m_current->setVisible(true);
}

void SubtaskWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The live step is elided against the card width, so it has to be measured
    // again whenever that width changes.
    updateCurrentActivity();
    // The width also decides how the two documents wrap, and their height is
    // what the card's minimum height is derived from. Only a width change can
    // change that, so a height-only resize (one this reflow would itself cause)
    // cannot feed back into another one.
    if (m_answer && event->oldSize().width() != event->size().width()) {
        reflowNow();
    }
}

void SubtaskWidget::updateHeader()
{
    const QString escapedName = m_agentName.toHtmlEscaped();
    // The agent name is the primary identifier; "Sub-agent" is the role label
    // shown in the badge, so the title no longer repeats it.
    m_title->setText(QStringLiteral("<span style=\"color:%1\">%2</span>")
                         .arg(ChatTheme::textPrimary(), escapedName));
    m_title->setToolTip(i18n("Sub-agent: %1", m_agentName));

    // The mode chip shows which mode the sub-agent runs in. Hidden when no mode
    // was given, so a modeless delegation stays compact.
    const QString modeText = displayMode(m_modeId);
    if (modeText.isEmpty()) {
        m_modeChip->setVisible(false);
    } else {
        m_modeChip->setText(QStringLiteral("<span style=\"color:%1; font-size:9px;"
                                           " background:%2; border-radius:3px;"
                                           " padding:1px 5px;\">%3</span>")
                                .arg(ChatTheme::accent(), ChatTheme::mixColors(ChatTheme::accent(), ChatTheme::cardBg(), 18),
                                     modeText.toHtmlEscaped()));
        m_modeChip->setToolTip(i18n("Mode: %1", modeText));
        m_modeChip->setVisible(true);
    }
    updateElapsed();
}

void SubtaskWidget::updateElapsed()
{
    // The status line carries the state, the elapsed time and the step count.
    // The state word is coloured so a finished, failed or cancelled card can be
    // told apart from a running one without reading the text.
    const QString stateWord = this->stateWord();
    const QString stateColor = this->stateColor();

    QStringList parts;
    const QString elapsed = timeText();
    if (!elapsed.isEmpty()) {
        parts << elapsed;
    }
    if (m_toolCount > 0) {
        parts << i18np("%1 tool", "%1 tools", m_toolCount);
    } else if (m_stepCount > 0) {
        parts << i18np("%1 step", "%1 steps", m_stepCount);
    }
    if (m_errorCount > 0) {
        parts << i18np("%1 error", "%1 errors", m_errorCount);
    }

    m_status->setTextFormat(Qt::RichText);
    QString status = QStringLiteral("<span style=\"color:%1; font-weight:600;\">%2</span>")
                         .arg(stateColor, stateWord.toHtmlEscaped());
    if (!parts.isEmpty()) {
        status += QStringLiteral(" <span style=\"color:%1;\">·</span> %2")
                      .arg(ChatTheme::textMuted(), parts.join(QStringLiteral(" \xC2\xB7 ")));
    }
    m_status->setText(status);
    // The tab strip points at this headline; it repaints on every tick.
    Q_EMIT appearanceChanged();
}

QString SubtaskWidget::stateWord() const
{
    if (m_running) {
        return i18n("Running");
    }
    if (m_cancelled) {
        return i18n("Cancelled");
    }
    if (m_failed) {
        return i18n("Failed");
    }
    return i18n("Done");
}

QString SubtaskWidget::stateColor() const
{
    if (m_running) {
        return ChatTheme::accent();
    }
    if (m_cancelled) {
        return ChatTheme::textMuted();
    }
    if (m_failed) {
        return ChatTheme::danger();
    }
    return ChatTheme::success();
}

QString SubtaskWidget::stateGlyph() const
{
    if (m_running) {
        return u"◐"_s;
    }
    if (m_failed) {
        return u"✗"_s;
    }
    if (m_cancelled) {
        return u"⊘"_s;
    }
    return u"✓"_s;
}

QString SubtaskWidget::displayName() const
{
    return m_agentName.isEmpty() ? i18n("Sub-agent") : m_agentName;
}

QString SubtaskWidget::timeText() const
{
    const qint64 secs = m_elapsed.isValid() ? m_elapsed.elapsed() / 1000 : 0;
    if (secs < 60) {
        if (secs > 0) {
            return QStringLiteral("%1s").arg(secs);
        }
        // "Done · 0s" is noise on a restored card, where there is no duration
        // to report. A live run still says something while it warms up.
        return m_running ? i18n("<1s") : QString();
    }
    const int mins = static_cast<int>(secs / 60);
    const int remSecs = static_cast<int>(secs % 60);
    return i18n("%1m %2s", mins, remSecs);
}

QString SubtaskWidget::statusText() const
{
    QStringList parts;
    const QString elapsed = timeText();
    if (!elapsed.isEmpty()) {
        parts << elapsed;
    }
    if (m_toolCount > 0) {
        parts << i18np("%1 tool", "%1 tools", m_toolCount);
    } else if (m_stepCount > 0) {
        parts << i18np("%1 step", "%1 steps", m_stepCount);
    }
    if (m_errorCount > 0) {
        parts << i18np("%1 error", "%1 errors", m_errorCount);
    }
    if (parts.isEmpty()) {
        return stateWord();
    }
    return QStringLiteral("%1 \xC2\xB7 %2").arg(stateWord(), parts.join(QStringLiteral(" \xC2\xB7 ")));
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

    const QString glyph = stateGlyph();
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
    if (m_copyResult) {
        m_copyResult->setStyleSheet(
            QStringLiteral("QPushButton { background: transparent; color: %1; border: 1px solid %2;"
                           " border-radius: 5px; padding: 2px 7px; font-size: 10px; }"
                           "QPushButton:hover { background-color: %3; }")
                .arg(ChatTheme::textMuted(), ChatTheme::border(), ChatTheme::hoverBg()));
    }
    m_activity->setStyleSheet(
        QStringLiteral("QTextBrowser { background: %1; border: 1px solid %2; border-radius: 5px;"
                       " color: %3; font-size: 10px; padding: 4px; }")
            .arg(ChatTheme::panelBg(), ChatTheme::border(), ChatTheme::textMuted()));
}

void SubtaskWidget::setActivityVisible(bool open)
{
    if (!m_activity) {
        return;
    }
    m_activity->setVisible(open);
    updateActivityToggle();
    reflowNow();
}

void SubtaskWidget::updateActivityToggle()
{
    if (!m_toggle || !m_activity) {
        return;
    }
    const bool open = !m_activity->isHidden();
    // Once there is work behind it the disclosure says how much, so the user
    // can see there is data there without opening it first. Tools and errors
    // are the useful counts; the raw line count is noise.
    const QString arrow = open ? u"▾"_s : u"▸"_s;
    QStringList summary;
    if (m_toolCount > 0) {
        summary << i18np("%1 tool", "%1 tools", m_toolCount);
    }
    if (m_errorCount > 0) {
        summary << i18np("%1 error", "%1 errors", m_errorCount);
    }
    const QString label = summary.isEmpty() ? i18n("Activity")
                                            : i18n("Activity · %1", summary.join(QStringLiteral(" · ")));
    m_toggle->setText(QStringLiteral("%1  %2").arg(label, arrow));
    QString tip = open ? i18n("Hide the sub-agent's tool calls and progress")
                       : i18n("Show the sub-agent's tool calls and progress");
    if (m_toolCount > 0) {
        tip += u'\n' + i18np("%1 tool call recorded", "%1 tool calls recorded", m_toolCount);
    }
    m_toggle->setToolTip(tip);
}

void SubtaskWidget::setHeightBudget(int px)
{
    if (m_heightBudget == px) {
        return;
    }
    m_heightBudget = px;
    setMaximumHeight(px > 0 ? px : QWIDGETSIZE_MAX);
    reflowNow();
}

int SubtaskWidget::activityChromeHeight() const
{
    if (!m_activityContainer || !m_activityContainer->layout()) {
        return 0;
    }
    const QLayout *activityLayout = m_activityContainer->layout();
    const QMargins margins = activityLayout->contentsMargins();
    int height = margins.top() + margins.bottom();
    int rows = 0;
    if (m_toggle && !m_toggle->isHidden()) {
        height += m_toggle->sizeHint().height();
        ++rows;
    }
    // The log itself is body, but the gap above it is chrome.
    if (m_activity && !m_activity->isHidden() && rows > 0) {
        height += activityLayout->spacing();
    }
    return height;
}

int SubtaskWidget::chromeHeight() const
{
    const QLayout *root = layout();
    if (!root) {
        return 0;
    }
    const QMargins margins = root->contentsMargins();
    int height = margins.top() + margins.bottom();
    int rows = 0;
    for (int i = 0; i < root->count(); ++i) {
        QLayoutItem *item = root->itemAt(i);
        if (!item) {
            continue;
        }
        QWidget *widget = item->widget();
        if (widget && widget->isHidden()) {
            continue;
        }
        if (widget == m_answer) {
            // Body: it is sized from the height budget, not from the chrome.
            continue;
        }
        if (widget == m_activityContainer) {
            height += activityChromeHeight();
            ++rows;
            continue;
        }
        height += item->sizeHint().height();
        ++rows;
    }
    if (rows > 1) {
        height += root->spacing() * (rows - 1);
    }
    return height;
}

void SubtaskWidget::reflowNow()
{
    if (!m_answer || !m_activity) {
        return;
    }
    // Whether the user can see these at all: an explicitly hidden document
    // costs no height, whatever it once demanded.
    const bool showAnswer = !m_answer->isHidden();
    const bool showActivity = !m_activity->isHidden();

    int answerWant = 0;
    if (showAnswer) {
        m_answer->setMinimumHeight(0);
        m_answer->document()->setTextWidth(m_answer->viewport()->width());
        answerWant = qCeil(m_answer->document()->size().height());
    }
    int activityWant = 0;
    if (showActivity) {
        m_activity->setMinimumHeight(0);
        m_activity->document()->setTextWidth(m_activity->viewport()->width());
        activityWant = qCeil(m_activity->document()->size().height());
    }

    int answerMin = answerWant;
    int activityMin = activityWant;
    if (m_heightBudget > 0) {
        // Under a budget the card may not grow to fit its documents, so each
        // one is only asked for what is left after the chrome -- and scrolls
        // inside the card past that, instead of pushing the card over the strip
        // it lives in.
        const int body = qMax(0, m_heightBudget - chromeHeight());
        if (showAnswer && showActivity) {
            // The log is supporting detail; the task and the answer are the
            // card's subject, so the log takes at most a third of the body.
            activityMin = qMin(activityWant, body / 3);
            answerMin = qMin(answerWant, body - activityMin);
        } else if (showAnswer) {
            answerMin = qMin(answerWant, body);
        } else if (showActivity) {
            activityMin = qMin(activityWant, body);
        }
    }

    m_answer->setMinimumHeight(showAnswer ? answerMin : 0);
    m_activity->setMinimumHeight(showActivity ? activityMin : 0);
    updateGeometry();
}

} // namespace KateAi
