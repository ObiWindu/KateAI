/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "toolcallwidget.h"

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

    m_expandBtn = new QPushButton(u"▸"_s, this);
    m_expandBtn->setFixedSize(20, 20);
    m_expandBtn->setFlat(true);
    m_expandBtn->setCursor(Qt::PointingHandCursor);
    m_expandBtn->setStyleSheet(
        u"QPushButton { color: #888; background: transparent; border: none; font-size: 11px; }"
        u"QPushButton:hover { color: #ccc; }"_s);
    headerLayout->addWidget(m_expandBtn);

    root->addWidget(m_header);

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
        u"QTextBrowser {"
        u"  background-color: #11131a;"
        u"  color: #d4d4d4;"
        u"  border: 1px solid #2a3a22;"
        u"  border-radius: 4px;"
        u"  padding: 8px;"
        u"  font-family: monospace;"
        u"  font-size: 11px;"
        u"  line-height: 1.4;"
        u"}"
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
        u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);
    m_describeDiff->document()->setDefaultStyleSheet(
        u"body { color: #d4d4d4; font-family: monospace; font-size: 11px; margin: 0; padding: 0; }"
        u".removed { color: #ef9999; background-color: #3a1a1a; }"
        u".added { color: #9ed36a; background-color: #1a3a1a; }"
        u".hunk { color: #888; }"
        u"pre { margin: 0; white-space: pre-wrap; font-family: monospace; font-size: 11px; }"
        u"p { margin: 0; white-space: pre-wrap; }"_s);
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
        u"QPlainTextEdit {"
        u"  background-color: #1a1a1a;"
        u"  color: #aaa;"
        u"  border: none;"
        u"  border-radius: 4px;"
        u"  padding: 8px;"
        u"  font-family: monospace;"
        u"  font-size: 11px;"
        u"}"
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
        u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);
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
    m_title->setToolTip(u"%1 - %2"_s.arg(toolName, m_titleText));
    updateTitleText();
    if (!isDiffTool(toolName) && !summary.isEmpty()) {
        setPreviewText(summary);
    }
    updateStyle();
}

void ToolCallWidget::setRunning()
{
    m_finished = false;
    m_status->setText(u"⟳"_s);
    m_status->setStyleSheet(u"QLabel { color: #3b82f6; font-size: 14px; }"_s);
}

void ToolCallWidget::setDescribeDiff(const QString &diff)
{
    if (diff.trimmed().isEmpty() || !isDiffTool(m_toolName)) {
        return;
    }
    m_hasDiffPreview = true;
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
    showPreviewHtml(plainToHtml(text));
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

    if (result.ok) {
        m_status->setText(u"✓"_s);
        m_status->setStyleSheet(u"QLabel { color: #22c55e; font-size: 14px; }"_s);
    } else {
        m_status->setText(u"✗"_s);
        m_status->setStyleSheet(u"QLabel { color: #ef4444; font-size: 14px; }"_s);
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
    const QString borderColor = colorForRisk(m_risk);
    const QString bgColor = m_finished ? (m_ok ? u"#1a1f1a"_s : u"#1f1a1a"_s) : u"#1a1a2e"_s;

    setStyleSheet(
        u"ToolCallWidget {"
        u"  background-color: %1;"
        u"  border-left: 3px solid %2;"
        u"  border-radius: 6px;"
        u"  margin: 4px 0;"
        u"}"_s.arg(bgColor, borderColor));

    m_title->setStyleSheet(u"QLabel { color: #ccc; font-size: 12px; }"_s);
}

QString ToolCallWidget::iconForTool(const QString &toolName) const
{
    if (toolName == u"read_file"_s) return u"📄"_s;
    if (toolName == u"write_file"_s) return u"📝"_s;
    if (toolName == u"edit_file"_s || toolName == u"multi_edit_file"_s || toolName == u"multi_replace_file_content"_s) return u"✏️"_s;
    if (toolName == u"list_dir"_s) return u"📁"_s;
    if (toolName == u"grep"_s) return u"🔍"_s;
    if (toolName == u"glob"_s) return u"🔎"_s;
    if (toolName == u"bash"_s) return u"⚡"_s;
    return u"🔧"_s;
}

QString ToolCallWidget::colorForRisk(ToolRisk risk) const
{
    switch (risk) {
    case ToolRisk::Read:
        return u"#22c55e"_s;    // green
    case ToolRisk::Write:
        return u"#eab308"_s;    // yellow
    case ToolRisk::Execute:
        return u"#ef4444"_s;    // red
    }
    return u"#888"_s;
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
    const int prefixW = fm.horizontalAdvance(m_toolName + u" - "_s) + 8;
    const int firstW = std::max(24, avail - prefixW);
    const QString wrapped = wrapToWidth(m_titleText, fm, firstW, avail);
    QString cmdHtml = escapeHtml(wrapped);
    cmdHtml.replace(u'\n', u"<br>"_s);
    m_title->setText(u"<b>%1</b> - %2"_s.arg(escapeHtml(m_toolName), cmdHtml));

    const int lines = std::max(1, static_cast<int>(wrapped.count(u'\n')) + 1);
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
    return fittedDocumentHeight(m_describeDiff->document(), vw, 20);
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
