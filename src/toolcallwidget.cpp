/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "toolcallwidget.h"

#include "chattheme.h"
#include "codehighlight.h"

#include <KLocalizedString>

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStringList>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace
{

// How many lines a tool header may occupy. A `bash` card carries the whole
// command line, and a multi-line script can run to
// hundreds of characters; left unbounded, one card grew tall enough to bury the
// conversation around it. Three lines is enough to recognise what ran.
constexpr int kMaxTitleLines = 3;

// How much of a tool's own output a card keeps. A chatty command can print
// thousands of lines, and left whole one card grew tall enough to bury the
// conversation around it.
constexpr int kMaxResultLines = 200;

// How much of the arguments a card shows before the tool has produced anything.
// An edit's old/new strings are long by nature, and this body is a placeholder
// for the result rather than the thing worth reading.
constexpr int kMaxInputLines = 12;

// Keeps at most `maxLines` of already-wrapped text, marking that some was
// dropped.
//
// The marker is appended first and the result elided, rather than eliding to a
// reduced width: eliding to a narrower budget makes the line *more* likely to
// fit whole, which silently drops the "…" and leaves the user with text that
// stops mid-word for no visible reason. Appending then eliding to the real width
// guarantees the marker and still respects the width.
QString clampToLines(const QString &text, int maxLines, const QFontMetrics &fm, int lineWidth)
{
    const QStringList lines = text.split(u'\n');
    if (lines.size() <= maxLines) {
        return text;
    }
    QStringList kept = lines.mid(0, maxLines);
    kept.last() = fm.elidedText(kept.last().trimmed() + QStringLiteral(" …"),
                                Qt::ElideRight,
                                std::max(24, lineWidth));
    return kept.join(u'\n');
}

// Wrap at spaces when possible, otherwise at the character that would overflow.
QString wrapToWidth(const QString &text, const QFontMetrics &fm, int firstWidth, int nextWidth)
{
    QString result;
    QString line;
    int maxW = std::max(8, firstWidth);

    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == u'\n') {
            result += line;
            result += u'\n';
            line.clear();
            maxW = std::max(8, nextWidth);
            continue;
        }
        const QString trial = line + ch;
        if (fm.horizontalAdvance(trial) <= maxW || line.isEmpty()) {
            line = trial;
            continue;
        }
        const int sp = line.lastIndexOf(u' ');
        if (sp > 0) {
            result += line.left(sp);
            result += u'\n';
            line = line.mid(sp + 1) + ch;
        } else {
            result += line;
            result += u'\n';
            line = QString(ch);
        }
        maxW = std::max(8, nextWidth);
    }
    result += line;
    return result;
}

int fittedDocumentHeight(QTextDocument *doc, int viewportWidth, int extra)
{
    if (!doc) {
        return extra;
    }
    doc->setTextWidth(std::max(40, viewportWidth));
    return std::max(1, static_cast<int>(doc->size().height()) + extra);
}

// Bounded by lines rather than characters, so a long but meaningful line is
// never cut in half and a chatty tool cannot grow a card without bound. The
// tail is marked instead of being dropped silently, so the user can tell that
// something was left out.
QString clampToLines(const QString &text, int maxLines)
{
    QStringList lines = text.split(u'\n');
    if (lines.size() <= maxLines) {
        return text;
    }
    const int hidden = lines.size() - maxLines;
    lines = lines.mid(0, maxLines);
    lines.append(i18n("… %1 more lines", hidden));
    return lines.join(u'\n');
}

// A unified diff starts with a "--- " file header and a "+++ " header within
// the next few lines. Prose that merely contains dashes is not a diff.
QString extractUnifiedDiff(const QString &text)
{
    const QStringList lines = text.split(u'\n');
    const qsizetype count = lines.size();
    qsizetype start = -1;
    for (qsizetype i = 0; i < count; ++i) {
        if (!lines.at(i).startsWith(u"--- "_s)) {
            continue;
        }
        const qsizetype window = std::min(count, i + 6);
        for (qsizetype j = i + 1; j < window; ++j) {
            if (lines.at(j).startsWith(u"+++ "_s)) {
                start = i;
                break;
            }
        }
        if (start >= 0) {
            break;
        }
    }
    if (start < 0) {
        return QString();
    }
    return lines.mid(start).join(u'\n');
}

QString textBeforeDiff(const QString &text)
{
    const QString diff = extractUnifiedDiff(text);
    if (diff.isEmpty()) {
        return text;
    }
    const int at = text.indexOf(diff);
    if (at <= 0) {
        return QString();
    }
    return text.left(at).trimmed();
}

} // namespace

namespace KateAi
{

ToolCallWidget::ToolCallWidget(const QString &toolCallId, QWidget *parent)
    : QWidget(parent)
    , m_toolCallId(toolCallId)
{
    setObjectName(u"ToolCallWidget_%1"_s.arg(toolCallId));
    // A plain QWidget paints neither a stylesheet background nor a border
    // until this is set, which left the coloured left rail invisible.
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 4, 0, 4);
    root->setSpacing(0);

    // Header row: icon + title + status + expand button
    m_header = new QWidget(this);
    m_header->setObjectName(u"toolHeader"_s);
    // A plain QWidget only paints a stylesheet background once styled-background
    // is set; without this the head box stays transparent and the card looks
    // bodyless while collapsed.
    m_header->setAttribute(Qt::WA_StyledBackground, true);
    m_header->setMinimumWidth(0);
    m_header->setCursor(Qt::PointingHandCursor);
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(10, 6, 10, 6);
    headerLayout->setSpacing(8);

    m_icon = new QLabel(this);
    // Named so the icon can be asserted from a test without reaching into
    // private members.
    m_icon->setObjectName(u"toolIcon"_s);
    m_icon->setFixedSize(16, 16);
    m_icon->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(m_icon);

    m_title = new QLabel(this);
    // Named so the line cap can be asserted from a test without reaching into
    // private members.
    m_title->setObjectName(u"toolTitle"_s);
    m_title->setWordWrap(true);
    m_title->setTextFormat(Qt::RichText);
    m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    headerLayout->addWidget(m_title, 1);

    m_status = new QLabel(this);
    m_status->setFixedWidth(20);
    m_status->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(m_status);

    // Per-call diff stat. Empty until a diff is attached, so a read-only tool
    // card does not carry a meaningless "+0 −0".
    m_diffStat = new QLabel(this);
    m_diffStat->setObjectName(u"toolDiffStat"_s);
    // Rich text so the added count can be green and the removed count red.
    m_diffStat->setTextFormat(Qt::RichText);
    m_diffStat->setVisible(false);
    headerLayout->addWidget(m_diffStat);

    // Elapsed time. Hidden until show() is called for it, so short calls do
    // not add visual noise; a call that takes seconds earns the space.
    m_duration = new QLabel(this);
    m_duration->setVisible(false);
    headerLayout->addWidget(m_duration);

    // Copies what the card holds: the result once the tool finished, the live
    // stream while it runs. The whole reply has its own copy button; a card is
    // where a specific diff or command output lives, and that is the part a
    // user actually wants on its own.
    m_copyBtn = new QPushButton(u"⧉"_s, this);
    m_copyBtn->setObjectName(u"toolCopyButton"_s);
    m_copyBtn->setFixedSize(20, 20);
    m_copyBtn->setFlat(true);
    m_copyBtn->setCursor(Qt::PointingHandCursor);
    m_copyBtn->setToolTip(i18n("Copy output"));
    m_copyBtn->setStyleSheet(
        QStringLiteral("QPushButton { color: %1; background: transparent; border: none; font-size: 11px; }"
                       "QPushButton:hover { color: #ffffff; }")
            .arg(ChatTheme::textMuted()));
    m_copyBtn->hide();
    connect(m_copyBtn, &QPushButton::clicked, this, [this]() {
        const QString text = copyableText();
        if (text.isEmpty()) {
            return;
        }
        // The button itself is the only feedback available: a card has no room
        // for a toast, and the clipboard is silent by nature.
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(text);
        }
        m_copyBtn->setText(u"✓"_s);
        QTimer::singleShot(900, this, [this]() {
            if (m_copyBtn) {
                m_copyBtn->setText(u"⧉"_s);
            }
        });
    });
    headerLayout->addWidget(m_copyBtn);

    m_expandBtn = new QPushButton(u"▸"_s, this);
    m_expandBtn->setFixedSize(20, 20);
    m_expandBtn->setFlat(true);
    m_expandBtn->setCursor(Qt::PointingHandCursor);
    headerLayout->addWidget(m_expandBtn);

    root->addWidget(m_header);

        // --- Approval row ---------------------------------------------------------
        // Hidden until the agent asks for permission. ChatWidget reparents this
        // row into the intent dock above the input, so the buttons never scroll
        // away from the user; the card keeps the diff being judged.
        m_approvalRow = new QWidget(this);
        m_approvalRow->setObjectName(u"approvalRow"_s);
        // Plain QWidget subclasses ignore a stylesheet background unless this is
        // set, which would leave the docked approval strip invisible.
        m_approvalRow->setAttribute(Qt::WA_StyledBackground, true);
        auto *approvalLayout = new QHBoxLayout(m_approvalRow);
        approvalLayout->setContentsMargins(9, 5, 9, 5);
        approvalLayout->setSpacing(6);

        m_approvalHint = new QLabel(i18n("Needs approval"), m_approvalRow);
        approvalLayout->addWidget(m_approvalHint);

        // What is being approved. This strip is docked at the bottom of the panel
        // while the diff it refers to sits up in the transcript, possibly
        // scrolled out of sight, so the decision must not depend on going to
        // find it.
        m_approvalSubject = new QLabel(m_approvalRow);
        approvalLayout->addWidget(m_approvalSubject, 1);

        approvalLayout->addStretch();

        m_allowBtn = new QPushButton(i18n("Allow"), m_approvalRow);
        m_allowBtn->setCursor(Qt::PointingHandCursor);
        connect(m_allowBtn, &QPushButton::clicked, this, [this] {
            Q_EMIT approvalChosen(PermissionDecision::AllowOnce);
        });

        m_alwaysBtn = new QPushButton(i18n("Always"), m_approvalRow);
        m_alwaysBtn->setCursor(Qt::PointingHandCursor);
        m_alwaysBtn->setToolTip(i18n("Allow this tool for the rest of the session"));
        connect(m_alwaysBtn, &QPushButton::clicked, this, [this] {
            Q_EMIT approvalChosen(PermissionDecision::AllowSession);
        });

        m_denyBtn = new QPushButton(i18n("Deny"), m_approvalRow);
        m_denyBtn->setCursor(Qt::PointingHandCursor);
        connect(m_denyBtn, &QPushButton::clicked, this, [this] {
            Q_EMIT approvalChosen(PermissionDecision::Deny);
        });

        approvalLayout->addWidget(m_allowBtn);
        approvalLayout->addWidget(m_alwaysBtn);
        approvalLayout->addWidget(m_denyBtn);
        root->addWidget(m_approvalRow);
        m_approvalRow->hide();

    // A tool has one result surface. Keeping a second preview box below the
    // card duplicated information and made completed calls harder to scan.
    // The result itself is expanded when it arrives.
    m_detailsContainer = new QWidget(this);
    m_detailsContainer->setMaximumHeight(0);
    auto *detailsLayout = new QVBoxLayout(m_detailsContainer);
    detailsLayout->setContentsMargins(10, 0, 10, 8);
    detailsLayout->setSpacing(0);

    // Names what the body below holds. Different tools produce different kinds
    // of result and the caption is what separates a diff from file contents or
    // a command's output without the user having to infer it from the header.
    m_viewCaption = new QLabel(m_detailsContainer);
    m_viewCaption->setObjectName(u"toolViewCaption"_s);
    m_viewCaption->hide();
    detailsLayout->addWidget(m_viewCaption);

    // The diff surface for file edits and writes. A change is not "output":
    // pushed through the plain result view, the added and removed lines sat
    // among the file text as flat monospace and the user had to read them to
    // see what moved. Here each line carries a green or red background, which
    // is the one thing that makes a long diff scannable.
    m_describeDiff = new QTextBrowser(this);
    // Named so the diff surface can be found from a test without reaching into
    // private members.
    m_describeDiff->setObjectName(u"toolDiff"_s);
    m_describeDiff->setReadOnly(true);
    m_describeDiff->setOpenExternalLinks(true);
    connect(m_describeDiff, &QTextBrowser::anchorClicked, this, [this](const QUrl &url){ if(url.isLocalFile()) Q_EMIT openFileRequested(url.toLocalFile(), 0); else if(url.scheme()==u"file"_s) Q_EMIT openFileRequested(url.path(), 0); });
    m_describeDiff->setMinimumWidth(0);
    m_describeDiff->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_describeDiff->setLineWrapMode(QTextEdit::WidgetWidth);
    m_describeDiff->setWordWrapMode(QTextOption::WrapAnywhere);
    m_describeDiff->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_describeDiff->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_describeDiff->hide();
    detailsLayout->addWidget(m_describeDiff);

    m_details = new QTextBrowser(this);
    // Named so the result surface can be found from a test without reaching
    // into private members.
    m_details->setObjectName(u"toolDetails"_s);
    m_details->setReadOnly(true);
    m_details->setOpenExternalLinks(true);
    connect(m_details, &QTextBrowser::anchorClicked, this, [this](const QUrl &url){ if(url.isLocalFile()) Q_EMIT openFileRequested(url.toLocalFile(), 0); else if(url.scheme()==u"file"_s) Q_EMIT openFileRequested(url.path(), 0); });
    m_details->setMinimumWidth(0);
    m_details->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_details->setLineWrapMode(QTextEdit::WidgetWidth);
    m_details->setWordWrapMode(QTextOption::WrapAnywhere);
    m_details->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_details->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailsLayout->addWidget(m_details);

    root->addWidget(m_detailsContainer);

    // Animation for expand/collapse
    m_animation = new QPropertyAnimation(m_detailsContainer, "maximumHeight", this);
    m_animation->setDuration(200);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);

    // Connections
    connect(m_expandBtn, &QPushButton::clicked, this, &ToolCallWidget::toggleExpand);

    // Make the entire header clickable for expand/collapse
    m_header->installEventFilter(this);

    // Default state
    setRunning();
    updateStyle();
    applyStyle();
}

void ToolCallWidget::applyStyle()
{
    // Card chrome: rail, header box, title and icon, with the tool's hue
    // blended in. Every colour here comes from ChatTheme::ToolCard or the
    // tool's fixed hue, so a theme change re-runs this to the same look:
    // the card never follows the theme palette.
    updateStyle();

    // The status glyph's colour is state-dependent: the tool's hue while
    // running, the outcome colour once finished.
    if (m_denied) {
        m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }").arg(ChatTheme::ToolCard::danger()));
    } else if (m_finished) {
        m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }")
                                    .arg(m_ok ? ChatTheme::ToolCard::success() : ChatTheme::ToolCard::danger()));
    } else {
        applyRunningStatusColor();
    }

    // No colour in the stylesheet: the label uses rich text so the added count
    // is green and the removed count is red, each in its own span.
    m_diffStat->setStyleSheet(
        QStringLiteral("QLabel { font-size: 11px; font-family: monospace; background: transparent; }"));
    m_duration->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 11px; }").arg(ChatTheme::ToolCard::textMuted()));
    m_expandBtn->setStyleSheet(
        QStringLiteral("QPushButton { color: %1; background: transparent; border: none; font-size: 11px; }"
                       "QPushButton:hover { color: #ffffff; }")
            .arg(ChatTheme::ToolCard::textMuted()));
    m_viewCaption->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 10px; font-weight: 600;"
                       " letter-spacing: 0.4px; background: transparent; }")
            .arg(ChatTheme::ToolCard::textMuted()));

    m_approvalRow->setStyleSheet(ChatTheme::intentApprovalRow());
    m_approvalHint->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: 600; background: transparent; }")
            .arg(ChatTheme::ToolCard::warning()));
    m_approvalSubject->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 11px; background: transparent; }")
            .arg(ChatTheme::ToolCard::textPrimary()));
    m_allowBtn->setStyleSheet(
        QStringLiteral(
            "QPushButton { background-color: %1; color: #ffffff; border: none;"
            " border-radius: 5px; padding: 3px 12px; font-size: 11px; font-weight: 600; }"
            "QPushButton:hover { background-color: %2; }")
        .arg(ChatTheme::ToolCard::accent(), ChatTheme::ToolCard::accentHover()));
    m_alwaysBtn->setStyleSheet(
        QStringLiteral(
            "QPushButton { background-color: %1; color: %2; border: 1px solid %3;"
            " border-radius: 5px; padding: 3px 10px; font-size: 11px; }"
            "QPushButton:hover { background-color: %3; color: #ffffff; }")
        .arg(ChatTheme::ToolCard::cardBg(), ChatTheme::ToolCard::textPrimary(), ChatTheme::ToolCard::hoverBg()));
    m_denyBtn->setStyleSheet(
        QStringLiteral(
            "QPushButton { background: transparent; color: %1; border: 1px solid %2;"
            " border-radius: 5px; padding: 3px 10px; font-size: 11px; }"
            "QPushButton:hover { color: #ffffff; border-color: %1; }")
        .arg(ChatTheme::ToolCard::danger(), ChatTheme::ToolCard::danger()));

    // The result surface, re-derived for the view the card is in: a terminal
    // sheet for commands, the inset sheet for everything else.
    applyDetailsSurface(m_detailView);

    // The diff surface of a file change. The text colour is white because the
    // surface is one of the card's fixed dark insets, in every theme.
    m_describeDiff->setStyleSheet(
        QStringLiteral(
            "QTextBrowser {"
            "  background-color: %1;"
            "  color: #ffffff;"
            "  border: none;"
            "  padding: 0;"
            "  font-family: monospace;"
            "  font-size: 11px;"
            "}")
        .arg(ChatTheme::ToolCard::codeBlockBg()));
    m_describeDiff->document()->setDefaultStyleSheet(
        QStringLiteral(
            "body { color: %1; font-family: monospace; font-size: 11px; margin: 0; padding: 0; }"
            "table { width: 100%; }"
            "td { padding: 0 8px; }"
            "pre { margin: 0; white-space: pre-wrap; font-family: monospace; font-size: 11px; }")
            .arg(QStringLiteral("#ffffff")));
    // setDefaultStyleSheet() only reaches content parsed after it, so re-parse
    // the surfaces that already hold a body: syntax spans keep their inline
    // colours, the body colour updates.
    if (!m_describeDiff->toPlainText().isEmpty()) {
        m_describeDiff->setHtml(m_describeDiff->toHtml());
        scheduleReflow();
    }
}

void ToolCallWidget::setToolInfo(const QString &toolName, const QString &summary, ToolRisk risk)
{
    m_toolName = toolName;
    m_risk = risk;
    m_icon->setText(iconForTool(toolName));
    m_titleText = summary.isEmpty() ? i18n("Running…") : summary;
    // The header now clamps to three lines, so the untruncated summary and the
    // raw tool name both live in the tooltip.
    m_title->setToolTip(ChatTheme::toolLabel(m_toolName) + u"  ·  "_s + m_toolName
                        + (m_titleText.isEmpty() ? QString() : u"\n"_s + m_titleText));
    updateTitleText();
    updateStyle();
    // The tool name is what decides the shape of the expanded body, so this is
    // the earliest point the card can style itself for the kind of result it
    // will hold.
    applyDetailView();
    if (!m_finished) {
        // The in-flight glyph was coloured before the tool name was known, so
        // give it the tool's hue now that it is.
        applyRunningStatusColor();
    }
}

void ToolCallWidget::applyRunningStatusColor()
{
    if (!m_status) {
        return;
    }
    QString running = ChatTheme::toolColor(m_toolName);
    if (running == ChatTheme::ToolCard::textMuted()) {
        running = colorForRisk(m_risk);
    }
    m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }").arg(running));
}

void ToolCallWidget::setApprovalSubject(const QString &subject)
{
    if (m_approvalSubject) {
        m_approvalSubject->setText(subject);
    }
}

void ToolCallWidget::showApproval()
{
    m_awaitingApproval = true;
    m_approvalRow->show();
    // The diff is the thing being judged, so it has to be on screen when the
    // buttons are, whatever the user expanded before.
    setExpanded(true);
    syncPreviewVisibility();
    updateStyle();
}

void ToolCallWidget::setApprovalResolved(PermissionDecision decision)
{
    m_awaitingApproval = false;
    m_approvalRow->hide();
    if (decision == PermissionDecision::Deny) {
        m_denied = true;
        m_status->setText(u"✗"_s);
        m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }").arg(ChatTheme::ToolCard::danger()));
    }
    updateStyle();
}

void ToolCallWidget::setDurationVisible(bool visible)
{
    // Track the intent in a flag rather than asking isVisible(): a card is
    // often configured before its parent is shown, and isVisible() lies then.
    m_durationEnabled = visible;
    m_duration->setVisible(visible);
    if (visible) {
        updateDuration();
    }
}

void ToolCallWidget::updateDuration()
{
    if (!m_durationEnabled) {
        return;
    }
    const qint64 ms = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;
    if (ms < 1000) {
        m_duration->setText(QStringLiteral("%1ms").arg(ms));
        return;
    }
    if (ms < 60000) {
        m_duration->setText(QStringLiteral("%1s").arg(ms / 1000));
        return;
    }
    m_duration->setText(QStringLiteral("%1m").arg(ms / 60000));
}

void ToolCallWidget::setRunning()
{
    m_finished = false;
    m_denied = false;
    m_ok = true;
    m_status->setText(u"⟳"_s);
    // The running glyph takes the tool's own hue, so a card in flight is easy
    // to pick out of a wall of finished ones without a second colour system.
    applyRunningStatusColor();

    // A running call is worth timing. The label stays hidden for the first
    // second so instant calls do not flicker a duration into the transcript.
    m_elapsed.start();
    if (!m_durationTimer) {
        m_durationTimer = new QTimer(this);
        m_durationTimer->setInterval(100);
        connect(m_durationTimer, &QTimer::timeout, this, [this] {
            if (m_elapsed.elapsed() > 1000) {
                m_duration->setVisible(true);
            }
            updateDuration();
        });
    }
    m_durationTimer->start();
    updateStyle();
}

void ToolCallWidget::setDescribeDiff(const QString &diff)
{
    if (diff.trimmed().isEmpty() || !isDiffTool(m_toolName)) {
        return;
    }
    m_hasDiffPreview = true;
    // Count the hunks so the header can carry a one-glance size. A diff is the
    // single most useful thing to know before deciding whether to expand.
    m_diffAdded = 0;
    m_diffRemoved = 0;
    for (const QString &line : diff.split(u'\n')) {
        if (line.startsWith(u"+++"_s) || line.startsWith(u"---"_s)) {
            continue;
        }
        if (line.startsWith(u"+"_s)) {
            ++m_diffAdded;
        } else if (line.startsWith(u"-"_s)) {
            ++m_diffRemoved;
        }
    }
    // The stat carries both counts in one label: additions in green, deletions
    // in red, so the net change is readable at a glance without opening the card.
    m_diffStat->setText(QStringLiteral("<span style=\"color:%1\">+%2</span>"
                                       " <span style=\"color:%3\">−%4</span>")
                            .arg(ChatTheme::ToolCard::success(), QString::number(m_diffAdded),
                                 ChatTheme::ToolCard::danger(), QString::number(m_diffRemoved)));
    m_diffStat->setVisible(true);

    if (m_describeDiff) {
        m_describeDiff->setHtml(diffToHtml(diff));
        m_describeDiff->show();
        applyDetailView();
        // The change *is* the card's content, so it opens itself; otherwise a
        // diff would sit inside a collapsed card that only ever showed the
        // tool's one-line acknowledgement of the write.
        setExpanded(true);
    }
}

void ToolCallWidget::setFilePath(const QString &path)
{
    m_filePath = path;
}

void ToolCallWidget::setRunningInput(const QString &input)
{
    if (m_finished || input.trimmed().isEmpty()) {
        return;
    }
    m_runningInput = input;
    // A diff or a live stream is a better body than the arguments, so the input
    // only fills the card while nothing else has arrived. hasDiffBody() rather
    // than the diff surface's visibility: a card is built before it has ever
    // been shown, so nothing about it is visible yet.
    if (hasDiffBody()) {
        return;
    }
    // The arguments are the card's own content, so they open it: a card in
    // flight that says nothing but the tool name tells the user nothing about
    // the work it is doing.
    m_showingInput = true;
    m_details->setHtml(partialToHtml(clampToLines(input, kMaxInputLines)));
    applyDetailView();
    if (!m_details->toPlainText().isEmpty()) {
        setExpanded(true);
    }
    applyDetailsHeight();
    scheduleReflow();
}

QString ToolCallWidget::copyableText() const
{
    // The result is the truth once there is one; the live stream is the best
    // there is while the tool runs.
    return m_resultOutput.isEmpty() ? m_partialOutput : m_resultOutput;
}

void ToolCallWidget::updateCopyButton()
{
    if (!m_copyBtn) {
        return;
    }
    m_copyBtn->setVisible(!copyableText().trimmed().isEmpty());
}

void ToolCallWidget::flashAttention()
{
    if (!m_header) {
        return;
    }
    // Keyboard navigation lands on a card that may be far off screen, so the
    // jump needs a marker that survives the scroll. An id-qualified rule beats
    // the card's own, so the ring is visible until the timer drops it.
    const QString ring = QStringLiteral("ToolCallWidget#%1 { border: 1px solid %2; border-radius: 6px; }")
                             .arg(objectName(), ChatTheme::accent());
    setStyleSheet(styleSheet() + u'\n' + ring);
    QTimer::singleShot(800, this, [this]() {
        if (this) {
            updateStyle();
        }
    });
}

QString ToolCallWidget::plainToHtml(const QString &text) const
{
    QString out = u"<body>"_s;
    const QStringList lines = text.split(u'\n');
    if (lines.isEmpty()) {
        return u"<body><p> </p></body>"_s;
    }
    for (const QString &line : lines) {
        out += u"<p>%1</p>"_s.arg(line.isEmpty() ? u"&nbsp;"_s : escapeHtml(line));
    }
    out += u"</body>"_s;
    return out;
}

QString ToolCallWidget::diffToHtml(const QString &diff) const
{
    // One row per line, each cell carrying its own background: QTextDocument
    // honours cell backgrounds (the `bgcolor` attribute), while the
    // block-level CSS that would colour a bare <pre> is only partly supported
    // and left the highlight out on every other line.
    const QString addBg = ChatTheme::mixColors(ChatTheme::ToolCard::success(), ChatTheme::ToolCard::codeBlockBg(), 24);
    const QString delBg = ChatTheme::mixColors(ChatTheme::ToolCard::danger(), ChatTheme::ToolCard::codeBlockBg(), 24);
    const QString ctxBg = ChatTheme::ToolCard::codeBlockBg();
    const QString addFg = ChatTheme::ToolCard::success();
    const QString delFg = ChatTheme::ToolCard::danger();
    const QString ctxFg = ChatTheme::ToolCard::textMuted();

    // A write of a two-thousand-line file carries a two-thousand-line diff, and
    // left whole it grew a card tall enough to bury the conversation around it.
    // Bounded by lines like the result surface, with the marker in place of the
    // tail rather than a silent cut.
    constexpr int maxDiffLines = 400;
    const QStringList all = diff.split(u'\n');
    QStringList shown = all;
    if (shown.size() > maxDiffLines) {
        const int hidden = shown.size() - maxDiffLines;
        shown = shown.mid(0, maxDiffLines);
        shown.append(i18n("… %1 more lines", hidden));
    }

    QString out = u"<body>"_s;
    for (const QString &line : shown) {
        QString bg = ctxBg;
        QString fg = ChatTheme::ToolCard::textPrimary();
        if (line.startsWith(u"+++"_s) || line.startsWith(u"---"_s) || line.startsWith(u"@"_s)) {
            // File headers and hunk marks are navigation, not content.
            fg = ctxFg;
        } else if (line.startsWith(u"+"_s)) {
            bg = addBg;
            fg = addFg;
        } else if (line.startsWith(u"-"_s)) {
            bg = delBg;
            fg = delFg;
        }
        const QString text = line.isEmpty() ? QStringLiteral("&#160;") : escapeHtml(line);
        out += QStringLiteral("<table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\">"
                              "<tr><td bgcolor=\"%1\"><pre style=\"color:%2;\">%3</pre></td></tr>"
                              "</table>")
                   .arg(bg, fg, text);
    }
    out += u"</body>"_s;
    return out;
}

QString ToolCallWidget::escapeHtml(const QString &s) const
{
    // Quotes too: the text goes inside HTML attributes (bgcolor, style) as well
    // as element bodies, and a stray one would end the attribute early.
    QString out = s;
    out.replace(u"&"_s, u"&amp;"_s);
    out.replace(u"<"_s, u"&lt;"_s);
    out.replace(u">"_s, u"&gt;"_s);
    out.replace(u"\""_s, u"&quot;"_s);
    return out;
}

void ToolCallWidget::setPartialOutput(const QString &partialOutput)
{
    if (m_finished || partialOutput.isEmpty()) {
        return;
    }
    m_partialOutput = partialOutput;
    updateCopyButton();
    // The stream has arrived: it is the body from here on, not the arguments.
    m_showingInput = false;
    m_runningInput.clear();

    // An edit can stream its unified diff before the tool reports done. Colour
    // that portion as it arrives, and keep only the preamble in the details so
    // the change is not drawn twice. The full stream stays in m_partialOutput
    // for the copy button.
    const QString diff = isDiffTool(m_toolName) ? extractUnifiedDiff(partialOutput) : QString();
    if (!diff.isEmpty()) {
        setDescribeDiff(diff);
    }
    // The stream is rendered in the muted colour so the card reads as "this is
    // still arriving" while it runs, and setFinished's white text is clearly
    // the final answer. Command output is capped by lines; file reads stay
    // complete so the user can inspect the full contents in the transcript.
    const QString body = diff.isEmpty() ? partialOutput : textBeforeDiff(partialOutput);
    m_details->setHtml(partialToHtml(m_risk == ToolRisk::Read ? body : clampToLines(body, kMaxResultLines)));
    // A stream is worth watching, so it opens itself; a card the user collapsed
    // themselves stays collapsed, because setExpanded tracks their choice.
    if (!m_details->toPlainText().isEmpty() || m_hasDiffPreview) {
        setExpanded(true);
    }
    applyDetailsHeight();
    scheduleReflow();
}

QString ToolCallWidget::partialToHtml(const QString &text) const
{
    QString out = u"<body style=\"color: %1; font-family: monospace; font-size: 11px;\">"_s
                      .arg(ChatTheme::ToolCard::textMuted());
    const QStringList lines = text.split(u'\n');
    for (const QString &line : lines) {
        out += u"<p style=\"margin: 0; white-space: pre-wrap;\">%1</p>"_s.arg(line.isEmpty() ? u"&nbsp;"_s : escapeHtml(line));
    }
    out += u"</body>"_s;
    return out;
}

void ToolCallWidget::setActivityFrame(int frame)
{
    if (m_finished || !m_status) {
        return;
    }
    static const QChar kFrames[] = {u'◐', u'◓', u'◑', u'◒'};
    m_status->setText(QString(kFrames[frame & 3]));
}

void ToolCallWidget::setFinished(const ToolResult &result)
{
    m_finished = true;
    m_ok = result.ok;
    // The live stream is over: whatever it showed is now history.
    m_partialOutput.clear();
    m_runningInput.clear();
    m_showingInput = false;
    // Kept whole so a copy of the result is never corrupted by the display cap.
    m_resultOutput = result.output;
    updateCopyButton();
    if (m_durationTimer) {
        m_durationTimer->stop();
    }
    updateDuration();
    // Anything over a tenth of a second is worth reporting; below that the
    // timing is noise.
    if (m_elapsed.isValid() && m_elapsed.elapsed() >= 100) {
        m_duration->setVisible(true);
        updateDuration();
    }

    if (m_awaitingApproval) {
        // Finished without an answer: the turn was aborted mid-approval.
        m_awaitingApproval = false;
        m_approvalRow->hide();
    }

    if (result.ok) {
        m_status->setText(u"✓"_s);
        m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }").arg(ChatTheme::ToolCard::success()));
    } else {
        m_status->setText(u"✗"_s);
        m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }").arg(ChatTheme::ToolCard::danger()));
    }

    // Keep the complete result for normal commands and reads, bounded by lines
    // rather than characters so a long but meaningful line is never silently
    // cut in half. The card opens on completion: output belongs in the tool
    // call itself, not in a second preview box below it.
    //
    // ACP edits often carry the unified diff only in this result, not in the
    // arguments. Colour that portion, and leave an args-built diff in place
    // when the result has no "--- / +++" of its own. The copy button still
    // hands over m_resultOutput, which is the whole result.
    const QString diff = isDiffTool(m_toolName) ? extractUnifiedDiff(result.output) : QString();
    if (!diff.isEmpty()) {
        setDescribeDiff(diff);
    }
    const QString shownSource = diff.isEmpty() ? result.output : textBeforeDiff(result.output);
    const QString shown = m_risk == ToolRisk::Read ? shownSource : clampToLines(shownSource, kMaxResultLines);
    const QString language = languageForTool(m_toolName);
    // Nothing to show must not paint an empty code block: a tool that reports
    // no output (an edit whose result never arrived, an ACP turn whose
    // arguments were never recorded) would otherwise render the block's frame
    // -- surface and language label -- with no code inside it. The surface is
    // cleared instead, so the card settles on its header (and its diff, if the
    // arguments produced one) rather than an empty box.
    if (!shown.trimmed().isEmpty()) {
        m_details->setHtml(CodeHighlight::htmlForCode(shown, language));
    } else if (m_details && !m_details->toPlainText().isEmpty()) {
        m_details->clear();
    }
    setExpanded(!shown.trimmed().isEmpty() || m_hasDiffPreview);
    applyDetailView();
    applyDetailsHeight();
    scheduleReflow();
    updateStyle();
}

void ToolCallWidget::setExpandedHeight(int h)
{
    m_expandedHeight = h;
    m_detailsContainer->setMaximumHeight(h);
}

void ToolCallWidget::setExpanded(bool expanded)
{
    if (m_expanded == expanded) {
        syncPreviewVisibility();
        return;
    }
    m_expanded = expanded;
    if (m_expandBtn) {
        m_expandBtn->setText(m_expanded ? u"▾"_s : u"▸"_s);
    }
    if (m_animation) {
        m_animation->stop();
    }
    Q_EMIT expandedChanged(m_expanded);
    applyDetailsHeight();
    syncPreviewVisibility();
    scheduleReflow();
}

bool ToolCallWidget::isFileEditTool() const
{
    return isDiffTool(m_toolName) || m_hasDiffPreview;
}

void ToolCallWidget::syncPreviewVisibility()
{
}

void ToolCallWidget::toggleExpand()
{
    m_expanded = !m_expanded;
    m_expandBtn->setText(m_expanded ? u"▾"_s : u"▸"_s);
    Q_EMIT expandedChanged(m_expanded);
    syncPreviewVisibility();

    if (!hasDetailsBody()) {
        applyDetailsHeight();
        scheduleReflow();
        return;
    }

    m_animation->stop();
    if (m_expanded) {
        reflowDetails();
        m_animation->setStartValue(0);
        m_animation->setEndValue(detailsFitHeight());
    } else {
        m_animation->setStartValue(m_detailsContainer->height());
        m_animation->setEndValue(0);
    }
    m_animation->start();
}

void ToolCallWidget::updateStyle()
{
    // One accent rail on the left, plus the tool's own hue blended into the
    // card and header surfaces. The colour is per tool rather than per risk:
    // scans down a long transcript are by tool (what ran, what changed), and a
    // transcript of cards all the same shade gives the eye nothing to hold on
    // to. Failure still overrides everything in red, because that is the one
    // state that must never be missed.
    QString rail = ChatTheme::toolColor(m_toolName);
    if (rail == ChatTheme::ToolCard::textMuted()) {
        // An unknown tool (an MCP or ACP call with no built-in colour) still
        // needs a rail, so fall back to the risk.
        rail = colorForRisk(m_risk);
    }
    if ((m_finished && !m_ok) || m_denied) {
        rail = ChatTheme::ToolCard::danger();
    }

    setStyleSheet(
        QStringLiteral(
            "ToolCallWidget {"
            "  background-color: %1;"
            "  border-left: 2px solid %2;"
            "  border-radius: 6px;"
            "  margin: 2px 0;"
            "}")
        .arg(ChatTheme::toolCardBg(m_toolName), rail));

    m_header->setStyleSheet(ChatTheme::toolHeader(m_toolName));
    m_title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 12px; }").arg(ChatTheme::ToolCard::textPrimary()));
    if (m_icon) {
        m_icon->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 13px; background: transparent; }")
                                  .arg(rail));
    }
}

QString ToolCallWidget::iconForTool(const QString &toolName) const
{
    // Short glyphs rather than emoji: emoji render at wildly different sizes
    // and baselines across themes, which is what made the column ragged.
    const QString key = toolName.trimmed().toLower();

    // File operations - both KateAI and ACP tool names. Agents capitalise
    // these ("Edit", "Write") and also invent their own ("apply_patch").
    if (key == u"read_file"_s || key == u"fs/read_text_file"_s || key == u"read"_s) return u"📄"_s;
    if (key == u"write_file"_s || key == u"fs/write_text_file"_s || key == u"write"_s) return u"📝"_s;
    if (key == u"edit_file"_s || key == u"multi_edit_file"_s || key == u"multi_replace_file_content"_s
        || key == u"edit"_s || key == u"str_replace"_s || key == u"apply_patch"_s
        || key == u"search_replace"_s || key == u"replace"_s) return u"✏"_s;
    if (key == u"delete"_s) return u"✕"_s;
    if (ChatTheme::isEditToolName(toolName)) {
        if (key.contains(u"delete"_s) || key.contains(u"remove"_s)) return u"✕"_s;
        if (key.contains(u"write"_s) && !key.contains(u"edit"_s) && !key.contains(u"replace"_s)
            && !key.contains(u"patch"_s)) {
            return u"📝"_s;
        }
        return u"✏"_s;
    }
    if (toolName == u"list_dir"_s) return u"📁"_s;
    if (toolName == u"grep"_s || toolName == u"search"_s) return u"🔍"_s;
    if (toolName == u"glob"_s) return u"🔎"_s;
    if (toolName == u"bash"_s || toolName == u"execute"_s) return u"⚡"_s;
        if (toolName == u"web_search"_s) return u"🌐"_s;
        if (toolName == u"web_fetch"_s) return u"📖"_s;
    if (toolName == u"query_project_graph"_s) return u"🕸"_s;
    if (toolName == u"write_plan_file"_s) return u"📋"_s;
    if (toolName == u"suggest"_s) return u"💡"_s;
    if (toolName == u"question"_s) return u"❓"_s;
    if (toolName == u"goal_report"_s) return u"🎯"_s;
    if (toolName == u"new_task"_s) return u"🧩"_s;
    if (toolName.startsWith(u"mcp__"_s)) return u"🔌"_s;
    if (toolName.startsWith(u"terminal/"_s) || toolName == u"terminal"_s) return u"🖳"_s;
    return u"⚙"_s;
}

QString ToolCallWidget::languageForTool(const QString &toolName) const
{
    // If we have a file path, try to determine the language from the extension
    if (!m_filePath.isEmpty()) {
        const QString extension = QFileInfo(m_filePath).suffix().toLower();
        if (!extension.isEmpty()) {
            // Map common extensions to KSyntaxHighlighting language names
            static const QHash<QString, QString> extensionMap = {
                {u"cpp"_s, u"C++"_s}, {u"cc"_s, u"C++"_s}, {u"cxx"_s, u"C++"_s}, {u"c"_s, u"C"_s}, {u"h"_s, u"C"_s},
                {u"hpp"_s, u"C++"_s}, {u"hxx"_s, u"C++"_s},
                {u"py"_s, u"Python"_s}, {u"pyw"_s, u"Python"_s},
                {u"js"_s, u"JavaScript"_s}, {u"ts"_s, u"TypeScript"_s}, {u"jsx"_s, u"JavaScript"_s}, {u"tsx"_s, u"TypeScript"_s},
                {u"java"_s, u"Java"_s}, {u"kt"_s, u"Kotlin"_s}, {u"rs"_s, u"Rust"_s}, {u"go"_s, u"Go"_s},
                {u"cs"_s, u"C#"_s}, {u"php"_s, u"PHP"_s}, {u"rb"_s, u"Ruby"_s}, {u"swift"_s, u"Swift"_s},
                {u"sh"_s, u"Bash"_s}, {u"bash"_s, u"Bash"_s}, {u"zsh"_s, u"Bash"_s}, {u"fish"_s, u"Bash"_s},
                {u"html"_s, u"HTML"_s}, {u"htm"_s, u"HTML"_s}, {u"css"_s, u"CSS"_s}, {u"scss"_s, u"CSS"_s}, {u"sass"_s, u"CSS"_s},
                {u"xml"_s, u"XML"_s}, {u"json"_s, u"JSON"_s}, {u"yaml"_s, u"YAML"_s}, {u"yml"_s, u"YAML"_s},
                {u"toml"_s, u"TOML"_s}, {u"ini"_s, u"INI"_s}, {u"cfg"_s, u"INI"_s}, {u"conf"_s, u"INI"_s},
                {u"md"_s, u"Markdown"_s}, {u"markdown"_s, u"Markdown"_s}, {u"txt"_s, u"Text"_s},
                {u"sql"_s, u"SQL"_s}, {u"lua"_s, u"Lua"_s}, {u"pl"_s, u"Perl"_s}, {u"pm"_s, u"Perl"_s},
                {u"r"_s, u"R"_s}, {u"m"_s, u"Objective-C"_s}, {u"mm"_s, u"Objective-C++"_s},
                {u"vue"_s, u"Vue"_s}, {u"svelte"_s, u"Svelte"_s}, {u"dart"_s, u"Dart"_s},
                {u"fs"_s, u"F#"_s}, {u"ml"_s, u"OCaml"_s}, {u"clj"_s, u"Clojure"_s}, {u"scala"_s, u"Scala"_s},
                {u"groovy"_s, u"Groovy"_s}, {u"gradle"_s, u"Groovy"_s},
                {u"dockerfile"_s, u"Dockerfile"_s}, {u"makefile"_s, u"Makefile"_s}, {u"cmake"_s, u"CMake"_s},
            };

            if (extensionMap.contains(extension)) {
                return extensionMap.value(extension);
            }
        }
    }

    // Map tool names to syntax highlighting languages
    if (toolName == u"bash"_s || toolName == u"execute"_s) return u"Bash"_s;
    
    // Text-based tools - file operations, search, etc.
    if (toolName == u"read_file"_s || toolName == u"write_file"_s
        || toolName == u"edit_file"_s || toolName == u"multi_edit_file"_s
        || toolName == u"multi_replace_file_content"_s || toolName == u"list_dir"_s
        || toolName == u"glob"_s || toolName == u"grep"_s
        || toolName == u"fs/read_text_file"_s || toolName == u"fs/write_text_file"_s
        || toolName == u"read"_s || toolName == u"write"_s || toolName == u"edit"_s
        || toolName == u"str_replace"_s || toolName == u"delete"_s || toolName == u"search"_s) {
        // File operations and search return text content, not JSON
        return u"Text"_s;
    }

    // JSON data tools
    if (toolName == u"web_search"_s || toolName == u"web_fetch"_s) return u"JSON"_s;
    if (toolName == u"query_project_graph"_s) return u"JSON"_s;
    if (toolName == u"new_task"_s) return u"Text"_s;  // Sub-agent results are typically text
    if (toolName.startsWith(u"mcp__"_s)) return u"JSON"_s;

    // Terminal tools
    if (toolName.startsWith(u"terminal/"_s) || toolName == u"terminal"_s) return u"Text"_s;

    // ACP tools that don't match specific patterns - default to Text for readability
    // ACP agents often return human-readable results, not raw JSON
    if (toolName.contains(u"/"_s) || toolName.startsWith(u"fs/"_s)) {
        return u"Text"_s;
    }

    // Tool protocols commonly return key/value text even when they are not a
    // built-in tool. Give those calls a real definition too instead of leaving
    // them as uncoloured plain text.
    return u"Text"_s;
}

QString ToolCallWidget::colorForRisk(ToolRisk risk) const
{
    switch (risk) {
    case ToolRisk::Read:
        return ChatTheme::ToolCard::success();
    case ToolRisk::Write:
        return ChatTheme::ToolCard::warning();
    case ToolRisk::Execute:
        return ChatTheme::ToolCard::danger();
    }
    return ChatTheme::ToolCard::textMuted();
}

bool ToolCallWidget::isDiffTool(const QString &toolName) const
{
    return ChatTheme::isEditToolName(toolName);
}

QSize ToolCallWidget::minimumSizeHint() const
{
    const QSize hint = QWidget::minimumSizeHint();
    return QSize(0, hint.height());
}

QSize ToolCallWidget::sizeHint() const
{
    const QSize hint = QWidget::sizeHint();
    return QSize(0, hint.height());
}

void ToolCallWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (event->oldSize().width() > 0 && event->size().width() == event->oldSize().width()) {
        return;
    }
    scheduleReflow();
}

void ToolCallWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    scheduleReflow();
}

void ToolCallWidget::scheduleReflow()
{
    if (!m_reflowTimer) {
        m_reflowTimer = new QTimer(this);
        m_reflowTimer->setSingleShot(true);
        m_reflowTimer->setInterval(0);
        connect(m_reflowTimer, &QTimer::timeout, this, &ToolCallWidget::reflowNow);
    }
    m_reflowTimer->start();
}

void ToolCallWidget::reflowNow()
{
    updateTitleText();
    applyDetailsHeight();
    updateGeometry();
}

void ToolCallWidget::applyDetailsHeight()
{
    if (!m_detailsContainer) {
        return;
    }
    if (m_animation) {
        m_animation->stop();
    }
    if (m_expanded && hasDetailsBody()) {
        reflowDetails();
        m_detailsContainer->setMaximumHeight(detailsFitHeight());
    } else {
        m_detailsContainer->setMaximumHeight(0);
    }
}

ToolDetailView ToolCallWidget::detailViewForTool(const QString &toolName)
{
    // One shape per kind of work. A card that edits a file, one that reads it
    // and one that runs a command produce different things, and the expanded
    // body should say which without the header being read first.
    if (ChatTheme::isEditToolName(toolName)) {
        return ToolDetailView::Diff;
    }
    if (toolName == u"bash"_s || toolName == u"execute"_s || toolName.startsWith(u"terminal"_s)) {
        return ToolDetailView::Terminal;
    }
    if (toolName == u"read_file"_s || toolName == u"fs/read_text_file"_s
        || toolName == u"read"_s
        || toolName == u"write_plan_file"_s || toolName == u"fetch_rules"_s
        || toolName == u"project_graph"_s) {
        return ToolDetailView::Code;
    }
    if (toolName == u"grep"_s || toolName == u"glob"_s || toolName == u"search"_s) {
        return ToolDetailView::Search;
    }
    return ToolDetailView::Plain;
}

QString ToolCallWidget::detailViewCaption() const
{
    switch (m_detailView) {
    case ToolDetailView::Diff:
        return m_hasDiffPreview ? i18n("Changes") : i18n("Output");
    case ToolDetailView::Code:
        return i18n("File contents");
    case ToolDetailView::Terminal:
        return i18n("Output");
    case ToolDetailView::Search:
        return i18n("Matches");
    case ToolDetailView::Plain:
        break;
    }
    return i18n("Output");
}

// What the body below holds right now. A card is one shape per tool, but the
// same shape holds different things at different moments, and the caption is
// what says which: the arguments while the tool runs, its output once it has
// reported, the question and then the answer for the one tool that asks.
QString ToolCallWidget::bodyCaption() const
{
    if (m_toolName == u"question"_s) {
        return m_finished ? i18n("Answer") : i18n("Question");
    }
    if (m_showingInput) {
        return i18n("Input");
    }
    return detailViewCaption();
}

void ToolCallWidget::applyDetailsSurface(ToolDetailView view)
{
    if (!m_details) {
        return;
    }
    // A command's output is a terminal, not a code sample: one step darker
    // than the inset used everywhere else, so running and reading are told
    // apart at a glance.
    const QString surface = view == ToolDetailView::Terminal ? QStringLiteral("#101014")
                                                             : ChatTheme::ToolCard::codeBlockBg();
    m_details->setStyleSheet(
        QStringLiteral(
            "QTextBrowser {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: none;"
            "  padding: 8px;"
            "  font-family: monospace;"
            "  font-size: 11px;"
            "}")
        .arg(surface, QStringLiteral("#ffffff")));
    m_details->document()->setDefaultStyleSheet(
        QStringLiteral(
            "body { color: %1; font-family: monospace; font-size: 11px; margin: 0; padding: 0; }"
            "pre { margin: 0; white-space: pre-wrap; font-family: monospace; font-size: 11px; }"
            "p { margin: 0; white-space: pre-wrap; }")
            .arg(QStringLiteral("#ffffff")));
    // setDefaultStyleSheet() only reaches content parsed after it, so re-parse
    // the current output: syntax spans keep their inline colours, the body
    // colour updates.
    if (!m_details->toPlainText().isEmpty()) {
        m_details->setHtml(m_details->toHtml());
        scheduleReflow();
    }
}

void ToolCallWidget::applyDetailView()
{
    const ToolDetailView view = detailViewForTool(m_toolName);
    if (view != m_detailView && m_details) {
        m_detailView = view;
        applyDetailsSurface(view);
    }
    if (m_viewCaption) {
        const QString caption = bodyCaption();
        if (caption.isEmpty()) {
            m_viewCaption->hide();
        } else {
            m_viewCaption->setText(caption);
            m_viewCaption->show();
        }
    }
    applyDetailsHeight();
    scheduleReflow();
}

bool ToolCallWidget::hasDetailsBody() const
{
    const bool result = m_details && !m_details->toPlainText().isEmpty();
    return hasDiffBody() || result;
}

// Whether a diff is attached to the card at all. m_hasDiffPreview rather than
// the diff surface's visibility: the card is created and filled before it is
// ever shown, so visibility says nothing about the state of the body.
bool ToolCallWidget::hasDiffBody() const
{
    return m_hasDiffPreview && m_describeDiff && !m_describeDiff->toPlainText().isEmpty();
}

int ToolCallWidget::captionHeight() const
{
    if (!m_viewCaption || !m_viewCaption->isVisible()) {
        return 0;
    }
    return m_viewCaption->sizeHint().height() + 4;
}

int ToolCallWidget::browserFitHeight(QTextBrowser *browser, int padding) const
{
    if (!browser || !browser->isVisible() || browser->toPlainText().isEmpty()) {
        return 0;
    }
    int vw = browser->viewport()->width();
    if (vw < 40) {
        vw = std::max(40, width() - 28);
    }
    // The browser's own padding has to be added to the document, or the last
    // line of a result sits under the card's edge.
    return fittedDocumentHeight(browser->document(), vw, padding);
}

void ToolCallWidget::updateTitleText()
{
    if (!m_title) {
        return;
    }

    // Icon 16 + status 20 + expand 20 + 3× spacing 8 + header margins 20.
    const int chrome = 100;
    const int avail = std::max(48, width() - chrome);
    const QFontMetrics fm(m_title->font());
    // "multi_replace_file_content" is 27 characters of snake_case. The
    // transcript should read "Edit file", with the raw tool name kept in the
    // tooltip for anyone who needs it.
    const QString label = ChatTheme::toolLabel(m_toolName);
    const int prefixW = fm.horizontalAdvance(label + u" "_s) + 8;
    const int firstW = std::max(24, avail - prefixW);
    // Clamp before measuring, so the height below matches what is drawn.
    const QString wrapped = clampToLines(wrapToWidth(m_titleText, fm, firstW, avail), kMaxTitleLines, fm, avail);
    QString cmdHtml = escapeHtml(wrapped);
    cmdHtml.replace(u'\n', u"<br>"_s);
    m_title->setText(QStringLiteral("<span style=\"color:%1\"><b>%2</b></span>&nbsp; <span style=\"color:%3\">%4</span>")
                         .arg(ChatTheme::toolColor(m_toolName), escapeHtml(label),
                              ChatTheme::ToolCard::textPrimary(), cmdHtml));

    const int lines = std::max(1, static_cast<int>(wrapped.count(u'\n')) + 1);
    m_title->setMaximumHeight(fm.lineSpacing() * lines + 2);
    m_title->setMinimumHeight(fm.lineSpacing() * lines + 2);
}

int ToolCallWidget::detailsFitHeight() const
{
    // Everything the card currently shows, plus the container's bottom margin:
    // the diff of a file change, the tool's own result, or both.
    int h = captionHeight();
    if (hasDiffBody()) {
        h += browserFitHeight(m_describeDiff, 4);
    }
    if (m_details && !m_details->toPlainText().isEmpty()) {
        h += browserFitHeight(m_details, 16);
    }
    return h + 8;
}

void ToolCallWidget::reflowDetails()
{
    if (hasDiffBody()) {
        m_describeDiff->setFixedHeight(browserFitHeight(m_describeDiff, 4));
    }
    if (m_details) {
        // The result surface owns the height it needs and no more: a browser
        // left at its size hint grows the card to fill the panel on the first
        // expand.
        m_details->setFixedHeight(m_details->toPlainText().isEmpty() ? 0 : browserFitHeight(m_details, 16));
    }
    if (m_expanded && m_detailsContainer) {
        m_detailsContainer->setMaximumHeight(detailsFitHeight());
    }
    updateGeometry();
}

bool ToolCallWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_header && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            toggleExpand();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace KateAi
