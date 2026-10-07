/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "toolcallwidget.h"

#include "chattheme.h"

#include <KLocalizedString>

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
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

// How many lines a tool header, and a command preview, may occupy. A `bash`
// card carries the whole command line, and a multi-line script can run to
// hundreds of characters; left unbounded, one card grew tall enough to bury the
// conversation around it. Three lines is enough to recognise what ran.
constexpr int kMaxTitleLines = 3;
constexpr int kMaxPreviewLines = 3;

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

// Same idea for unwrapped source text, where a "line" is a newline.
QString clampSourceLines(const QString &text, int maxLines)
{
    const QStringList lines = text.split(u'\n');
    if (lines.size() <= maxLines) {
        return text;
    }
    QStringList kept = lines.mid(0, maxLines);
    kept.last() = kept.last().trimmed();
    kept << i18n("… %1 more lines", lines.size() - maxLines);
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

} // namespace

namespace KateAi
{

ToolCallWidget::ToolCallWidget(const QString &toolCallId, QWidget *parent)
    : QWidget(parent)
    , m_toolCallId(toolCallId)
{
    setObjectName(u"ToolCallWidget_%1"_s.arg(toolCallId));
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
    m_diffStat->setVisible(false);
    m_diffStat->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 11px; font-family: monospace; }")
        .arg(ChatTheme::success()));
    headerLayout->addWidget(m_diffStat);

    // Elapsed time. Hidden until show() is called for it, so short calls do
    // not add visual noise; a call that takes seconds earns the space.
    m_duration = new QLabel(this);
    m_duration->setVisible(false);
    m_duration->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 11px; }").arg(ChatTheme::textMuted()));
    headerLayout->addWidget(m_duration);

    m_expandBtn = new QPushButton(u"▸"_s, this);
    m_expandBtn->setFixedSize(20, 20);
    m_expandBtn->setFlat(true);
    m_expandBtn->setCursor(Qt::PointingHandCursor);
    m_expandBtn->setStyleSheet(
        QStringLiteral("QPushButton { color: %1; background: transparent; border: none; font-size: 11px; }"
                       "QPushButton:hover { color: #ffffff; }")
            .arg(ChatTheme::textMuted()));
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
        m_approvalRow->setStyleSheet(ChatTheme::intentApprovalRow());
        auto *approvalLayout = new QHBoxLayout(m_approvalRow);
        approvalLayout->setContentsMargins(9, 5, 9, 5);
        approvalLayout->setSpacing(6);

        auto *approvalHint = new QLabel(i18n("Needs approval"), m_approvalRow);
        approvalHint->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: 600; background: transparent; }").arg(ChatTheme::warning()));
        approvalLayout->addWidget(approvalHint);

        // What is being approved. This strip is docked at the bottom of the panel
        // while the diff it refers to sits up in the transcript, possibly
        // scrolled out of sight, so the decision must not depend on going to
        // find it.
        m_approvalSubject = new QLabel(m_approvalRow);
        m_approvalSubject->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 11px; background: transparent; }")
                .arg(ChatTheme::textPrimary()));
        approvalLayout->addWidget(m_approvalSubject, 1);

        approvalLayout->addStretch();

        auto *allowBtn = new QPushButton(i18n("Allow"), m_approvalRow);
        allowBtn->setCursor(Qt::PointingHandCursor);
        allowBtn->setStyleSheet(
            QStringLiteral(
                "QPushButton { background-color: %1; color: #ffffff; border: none;"
                " border-radius: 5px; padding: 3px 12px; font-size: 11px; font-weight: 600; }"
                "QPushButton:hover { background-color: %2; }")
            .arg(ChatTheme::accent(), ChatTheme::accentHover()));
        connect(allowBtn, &QPushButton::clicked, this, [this] {
            Q_EMIT approvalChosen(PermissionDecision::AllowOnce);
        });

        auto *alwaysBtn = new QPushButton(i18n("Always"), m_approvalRow);
        alwaysBtn->setCursor(Qt::PointingHandCursor);
        alwaysBtn->setToolTip(i18n("Allow this tool for the rest of the session"));
        alwaysBtn->setStyleSheet(
            QStringLiteral(
                "QPushButton { background-color: %1; color: %2; border: 1px solid %3;"
                " border-radius: 5px; padding: 3px 10px; font-size: 11px; }"
                "QPushButton:hover { background-color: %3; color: #ffffff; }")
            .arg(ChatTheme::cardBg(), ChatTheme::textPrimary(), ChatTheme::hoverBg()));
        connect(alwaysBtn, &QPushButton::clicked, this, [this] {
            Q_EMIT approvalChosen(PermissionDecision::AllowSession);
        });

        auto *denyBtn = new QPushButton(i18n("Deny"), m_approvalRow);
        denyBtn->setCursor(Qt::PointingHandCursor);
        denyBtn->setStyleSheet(
            QStringLiteral(
                "QPushButton { background: transparent; color: %1; border: 1px solid %2;"
                " border-radius: 5px; padding: 3px 10px; font-size: 11px; }"
                "QPushButton:hover { color: #ffffff; border-color: %1; }")
            .arg(ChatTheme::danger(), ChatTheme::danger()));
        connect(denyBtn, &QPushButton::clicked, this, [this] {
            Q_EMIT approvalChosen(PermissionDecision::Deny);
        });

        approvalLayout->addWidget(allowBtn);
        approvalLayout->addWidget(alwaysBtn);
        approvalLayout->addWidget(denyBtn);
        root->addWidget(m_approvalRow);
        m_approvalRow->hide();

    // Proposed edit diff — always shown in a highlighted box so the edited
    // code is visible in the chat transcript without needing to expand the
    // tool card. Only populated for edit_file / write_file tools.
    m_describeDiff = new QTextBrowser(this);
    m_describeDiff->setReadOnly(true);
    m_describeDiff->setOpenExternalLinks(false);
    m_describeDiff->setMinimumWidth(0);
    m_describeDiff->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_describeDiff->setLineWrapMode(QTextEdit::WidgetWidth);
    m_describeDiff->setWordWrapMode(QTextOption::WrapAnywhere);
    m_describeDiff->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_describeDiff->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_describeDiff->setStyleSheet(
        QStringLiteral(
            "QTextBrowser {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 6px;"
            "  padding: 8px;"
            "  font-family: monospace;"
            "  font-size: 11px;"
            "  line-height: 1.4;"
            "}")
        .arg(ChatTheme::codeBlockBg(), ChatTheme::textPrimary(), ChatTheme::border()));
    m_describeDiff->document()->setDefaultStyleSheet(
        QStringLiteral(
            "body { color: %1; font-family: monospace; font-size: 11px; margin: 0; padding: 0; }"
            ".removed { color: %2; }"
            ".added { color: %3; }"
            ".hunk { color: %4; }"
            "pre { margin: 0; white-space: pre-wrap; font-family: monospace; font-size: 11px; }"
            "p { margin: 0; white-space: pre-wrap; }")
            .arg(ChatTheme::textPrimary(),
                 QStringLiteral("#ff9a9a"),
                 QStringLiteral("#8ddb7a"),
                 ChatTheme::textMuted()));
    m_describeDiff->hide();
    root->addWidget(m_describeDiff);

    // Details container — initially hidden, holds the raw tool output
    m_detailsContainer = new QWidget(this);
    m_detailsContainer->setMaximumHeight(0);
    auto *detailsLayout = new QVBoxLayout(m_detailsContainer);
    detailsLayout->setContentsMargins(10, 0, 10, 8);
    detailsLayout->setSpacing(0);

    m_details = new QPlainTextEdit(this);
    m_details->setReadOnly(true);
    m_details->setMinimumWidth(0);
    m_details->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_details->setWordWrapMode(QTextOption::WrapAnywhere);
    m_details->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_details->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_details->setStyleSheet(
        QStringLiteral(
            "QPlainTextEdit {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: none;"
            "  padding: 8px;"
            "  font-family: monospace;"
            "  font-size: 11px;"
            "}")
        .arg(ChatTheme::codeBlockBg(), ChatTheme::textMuted()));
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
    if (!isDiffTool(toolName) && !summary.isEmpty()) {
        setPreviewText(summary);
    }
    updateStyle();
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
        m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }").arg(ChatTheme::danger()));
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
    m_status->setText(u"⟳"_s);
    m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }").arg(ChatTheme::accent()));

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
    m_diffStat->setText(QStringLiteral("+%1 −%2").arg(m_diffAdded).arg(m_diffRemoved));
    m_diffStat->setVisible(true);
    showPreviewHtml(diffToHtml(diff));
}

void ToolCallWidget::setPreviewText(const QString &text)
{
    if (m_hasDiffPreview) {
        return;
    }
    if (text.trimmed().isEmpty()) {
        return;
    }
    // Only ever a command or a path: setToolInfo routes diffs to
    // setDescribeDiff instead, and a diff is left whole because it is the thing
    // being judged when an edit is approved.
    showPreviewHtml(plainToHtml(clampSourceLines(text, kMaxPreviewLines)));
}

void ToolCallWidget::showPreviewHtml(const QString &html)
{
    if (!m_describeDiff) {
        return;
    }

    m_describeDiff->setHtml(html);
    syncPreviewVisibility();
    scheduleReflow();
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
    QString out = u"<body>"_s;
    for (const QString &line : diff.split(u'\n')) {
        if (line.startsWith(u"---"_s) || line.startsWith(u"+++"_s)) {
            out += u"<p class='hunk'>%1</p>"_s.arg(escapeHtml(line));
        } else if (line.startsWith(u"+"_s)) {
            out += u"<p class='added'>%1</p>"_s.arg(escapeHtml(line));
        } else if (line.startsWith(u"-"_s)) {
            out += u"<p class='removed'>%1</p>"_s.arg(escapeHtml(line));
        } else {
            out += u"<p>%1</p>"_s.arg(escapeHtml(line));
        }
    }
    out += u"</body>"_s;
    return out;
}

QString ToolCallWidget::escapeHtml(const QString &s) const
{
    QString out = s;
    out.replace(u"&"_s, u"&amp;"_s);
    out.replace(u"<"_s, u"&lt;"_s);
    out.replace(u">"_s, u"&gt;"_s);
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
        m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }").arg(ChatTheme::success()));
    } else {
        m_status->setText(u"✗"_s);
        m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 14px; }").arg(ChatTheme::danger()));
    }

    // Truncate very long outputs. File-edit cards keep the diff in the marine
    // preview and put the tool result in details. Other tools put output only
    // in the preview so expanding does not stack a duplicate copy.
    const QString output = result.output.length() > 4000
        ? result.output.left(4000) + i18n("\n\n… (truncated)")
        : result.output;
    if (m_hasDiffPreview) {
        m_details->setPlainText(output);
    } else {
        m_details->clear();
        if (!output.trimmed().isEmpty()) {
            setPreviewText(output);
        }
    }

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
    if (!m_describeDiff) {
        return;
    }
    // File-edit diffs stay visible even when the card is collapsed.
    const bool show = m_hasDiffPreview || m_expanded;
    if (show && !m_describeDiff->toPlainText().isEmpty()) {
        m_describeDiff->show();
        reflowPreview();
    } else if (!m_hasDiffPreview) {
        m_describeDiff->hide();
    }
}

void ToolCallWidget::toggleExpand()
{
    m_expanded = !m_expanded;
    m_expandBtn->setText(m_expanded ? u"▾"_s : u"▸"_s);
    Q_EMIT expandedChanged(m_expanded);
    syncPreviewVisibility();

    if (!m_details || m_details->toPlainText().isEmpty()) {
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
    // One accent rail on the left, carrying both the risk and the outcome, so
    // the card reads the same whether it is running, done or failed. The body
    // stays on a single surface: tinting the whole card by risk made a long
    // transcript read as stripes. The card sits on its own surface rather than
    // the panel backdrop, so a collapsed tool call is still findable.
    const QString rail = colorForRisk(m_risk);

    setStyleSheet(
        QStringLiteral(
            "ToolCallWidget {"
            "  background-color: %1;"
            "  border-left: 2px solid %2;"
            "  border-radius: 6px;"
            "  margin: 2px 0;"
            "}")
        .arg(ChatTheme::toolBg(), rail));

    m_header->setStyleSheet(ChatTheme::toolHeader());
    m_title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 12px; }").arg(ChatTheme::textPrimary()));
}

QString ToolCallWidget::iconForTool(const QString &toolName) const
{
    // Short glyphs rather than emoji: emoji render at wildly different sizes
    // and baselines across themes, which is what made the column ragged.
    if (toolName == u"read_file"_s) return u"\u{1F4C4}"_s;
    if (toolName == u"write_file"_s) return u"\u{1F4DD}"_s;
    if (toolName == u"edit_file"_s || toolName == u"multi_edit_file"_s || toolName == u"multi_replace_file_content"_s) return u"\u{270F}"_s;
    if (toolName == u"list_dir"_s) return u"\u{1F4C1}"_s;
    if (toolName == u"grep"_s) return u"\u{1F50D}"_s;
    if (toolName == u"glob"_s) return u"\u{1F50E}"_s;
    if (toolName == u"bash"_s) return u"\u{26A1}"_s;
        if (toolName == u"web_search"_s) return u"\u{1F310}"_s;
        if (toolName == u"web_fetch"_s) return u"\u{1F4D6}"_s;
    if (toolName == u"query_project_graph"_s) return u"\u{1F578}"_s;
    if (toolName == u"new_task"_s) return u"\u{1F9E9}"_s;
    if (toolName.startsWith(u"mcp__"_s)) return u"\u{1F50C}"_s;
    return u"\u{2699}"_s;
}

QString ToolCallWidget::colorForRisk(ToolRisk risk) const
{
    switch (risk) {
    case ToolRisk::Read:
        return ChatTheme::success();
    case ToolRisk::Write:
        return ChatTheme::warning();
    case ToolRisk::Execute:
        return ChatTheme::danger();
    }
    return ChatTheme::textMuted();
}

bool ToolCallWidget::isDiffTool(const QString &toolName) const
{
    return toolName == u"edit_file"_s || toolName == u"write_file"_s
        || toolName == u"multi_edit_file"_s || toolName == u"multi_replace_file_content"_s;
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
    reflowPreview();
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
    if (m_expanded && m_details && !m_details->toPlainText().isEmpty()) {
        reflowDetails();
        m_detailsContainer->setMaximumHeight(detailsFitHeight());
    } else {
        m_detailsContainer->setMaximumHeight(0);
    }
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
    m_title->setText(QStringLiteral("<span style=\"color:%1\"><b>%2</b></span>&nbsp; %3")
                         .arg(ChatTheme::textMuted(), escapeHtml(label), cmdHtml));

    const int lines = std::max(1, static_cast<int>(wrapped.count(u'\n')) + 1);
    m_title->setMaximumHeight(fm.lineSpacing() * lines + 2);
    m_title->setMinimumHeight(fm.lineSpacing() * lines + 2);
}

int ToolCallWidget::previewFitHeight() const
{
    if (!m_describeDiff) {
        return 0;
    }
    int vw = m_describeDiff->viewport()->width();
    if (vw < 40) {
        vw = std::max(40, width() - 8);
    }
    const int fitted = fittedDocumentHeight(m_describeDiff->document(), vw, 20);
    if (m_hasDiffPreview) {
        return fitted;
    }
    // Command preview: hard-cap the box as well as the text, because a long
    // line that the panel wraps could still push past three visual rows.
    const QFontMetrics fm(m_describeDiff->font());
    return std::min(fitted, fm.lineSpacing() * kMaxPreviewLines + 20);
}

int ToolCallWidget::detailsFitHeight() const
{
    if (!m_details) {
        return 16;
    }
    int vw = m_details->viewport()->width();
    if (vw < 40) {
        vw = std::max(40, width() - 28);
    }
    return fittedDocumentHeight(m_details->document(), vw, 16) + 16;
}

void ToolCallWidget::reflowPreview()
{
    if (!m_describeDiff || m_describeDiff->isHidden()) {
        return;
    }
    m_describeDiff->setFixedHeight(previewFitHeight());
    updateGeometry();
}

void ToolCallWidget::reflowDetails()
{
    if (!m_details) {
        return;
    }
    const int containerH = detailsFitHeight();
    m_details->setFixedHeight(std::max(1, containerH - 16));
    if (m_expanded) {
        m_detailsContainer->setMaximumHeight(containerH);
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
