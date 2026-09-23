/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "toolcallwidget.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

ToolCallWidget::ToolCallWidget(const QString &toolCallId, QWidget *parent)
    : QWidget(parent)
    , m_toolCallId(toolCallId)
{
    setObjectName(u"ToolCallWidget_%1"_s.arg(toolCallId));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 4, 0, 4);
    root->setSpacing(0);

    // Header row: icon + title + status + expand button
    m_header = new QWidget(this);
    m_header->setCursor(Qt::PointingHandCursor);
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(10, 6, 10, 6);
    headerLayout->setSpacing(8);

    m_icon = new QLabel(this);
    m_icon->setFixedSize(16, 16);
    m_icon->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(m_icon);

    m_title = new QLabel(this);
    m_title->setWordWrap(false);
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
        u"p { margin: 0; }"_s);
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
    m_details->setMaximumHeight(200);
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
    m_title->setText(u"<b>%1</b> — %2"_s.arg(toolName, summary.isEmpty() ? i18n("Running…") : summary));
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
    if (!m_describeDiff) {
        return;
    }

    // Only show diff box for edit_file and write_file tools
    if (!isDiffTool(m_toolName)) {
        m_describeDiff->hide();
        return;
    }

    if (diff.trimmed().isEmpty()) {
        m_describeDiff->hide();
        return;
    }

    m_describeDiff->setHtml(diffToHtml(diff));

    // Ensure the document is laid out before measuring
    m_describeDiff->document()->adjustSize();
    const int h = static_cast<int>(m_describeDiff->document()->size().height()) + 16;
    m_describeDiff->setFixedHeight(std::min(400, std::max(50, h)));
    m_describeDiff->show();

    // The diff box now lives outside the collapsible details container, so it
    // is always visible. Just update the expanded height if the details are
    // currently shown.
    if (m_expanded) {
        m_detailsContainer->setMaximumHeight(m_details->sizeHint().height() + 16);
    }
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

    // Populate details with the output (truncated for very long outputs)
    const QString output = result.output.length() > 4000
        ? result.output.left(4000) + i18n("\n\n… (truncated)")
        : result.output;
    m_details->setPlainText(output);

    // Update expanded height if currently expanded
    if (m_expanded) {
        m_detailsContainer->setMaximumHeight(m_details->sizeHint().height() + 16);
    }
    updateStyle();
}

void ToolCallWidget::setExpandedHeight(int h)
{
    m_expandedHeight = h;
    m_detailsContainer->setMaximumHeight(h);
}

void ToolCallWidget::toggleExpand()
{
    m_expanded = !m_expanded;
    m_expandBtn->setText(m_expanded ? u"▾"_s : u"▸"_s);

    m_animation->stop();
    if (m_expanded) {
        m_animation->setStartValue(0);
        m_animation->setEndValue(m_details->sizeHint().height() + 16);
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
        u"  margin: 4px 8px;"
        u"}"_s.arg(bgColor, borderColor));

    m_title->setStyleSheet(u"QLabel { color: #ccc; font-size: 12px; }"_s);
}

QString ToolCallWidget::iconForTool(const QString &toolName) const
{
    if (toolName == u"read_file"_s) return u"📄"_s;
    if (toolName == u"write_file"_s) return u"📝"_s;
    if (toolName == u"edit_file"_s) return u"✏️"_s;
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
    return toolName == u"edit_file"_s || toolName == u"write_file"_s;
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
