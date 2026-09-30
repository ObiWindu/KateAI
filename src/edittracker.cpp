/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "edittracker.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QTextBrowser>
#include <QWidget>
#include <QFrame>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

EditTracker::EditTracker(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"EditTracker"_s);
    // Don't set fixed height - let it expand when needed

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Compact bar (always visible when there are edits)
    m_barWidget = new QWidget(this);
    m_barWidget->setObjectName(u"EditTrackerBar"_s);
    m_barWidget->setFixedHeight(44);
    auto *barLayout = new QHBoxLayout(m_barWidget);
    barLayout->setContentsMargins(12, 4, 12, 4);
    barLayout->setSpacing(8);

    // Left side: icon and count
    m_iconLabel = new QLabel(u"✏️"_s, m_barWidget);
    m_iconLabel->setStyleSheet(u"QLabel { font-size: 16px; }"_s);
    barLayout->addWidget(m_iconLabel);

    m_countLabel = new QLabel(m_barWidget);
    m_countLabel->setStyleSheet(u"QLabel { color: #e4e4e4; font-size: 12px; font-weight: bold; }"_s);
    barLayout->addWidget(m_countLabel);

    barLayout->addStretch();

    // Expand/collapse button
    m_expandBtn = new QPushButton(u"▸"_s, m_barWidget);
    m_expandBtn->setCursor(Qt::PointingHandCursor);
    m_expandBtn->setFixedSize(28, 28);
    m_expandBtn->setFlat(true);
    m_expandBtn->setStyleSheet(
        u"QPushButton { color: #888; background: transparent; border: none; font-size: 12px; }"
        u"QPushButton:hover { color: #ccc; }"_s);
    connect(m_expandBtn, &QPushButton::clicked, this, [this]() { setExpanded(!m_expanded); });
    barLayout->addWidget(m_expandBtn);

    // Right side: Accept All / Reject All buttons
    m_acceptAllBtn = new QPushButton(i18n("Accept All"), m_barWidget);
    m_acceptAllBtn->setCursor(Qt::PointingHandCursor);
    m_acceptAllBtn->setFixedHeight(32);
    m_acceptAllBtn->setStyleSheet(
        u"QPushButton {"
        u"  background-color: #007acc;"
        u"  color: #ffffff;"
        u"  border: none;"
        u"  border-radius: 4px;"
        u"  padding: 0 16px;"
        u"  font-weight: bold;"
        u"  font-size: 12px;"
        u"}"
        u"QPushButton:hover { background-color: #0062a3; }"
        u"QPushButton:pressed { background-color: #004d80; }"_s);
    connect(m_acceptAllBtn, &QPushButton::clicked, this, &EditTracker::acceptAll);
    barLayout->addWidget(m_acceptAllBtn);

    m_rejectAllBtn = new QPushButton(i18n("Reject All"), m_barWidget);
    m_rejectAllBtn->setCursor(Qt::PointingHandCursor);
    m_rejectAllBtn->setFixedHeight(32);
    m_rejectAllBtn->setStyleSheet(
        u"QPushButton {"
        u"  background-color: transparent;"
        u"  color: #888;"
        u"  border: 1px solid #333;"
        u"  border-radius: 4px;"
        u"  padding: 0 16px;"
        u"  font-size: 12px;"
        u"}"
        u"QPushButton:hover { background-color: #2e1a1a; color: #ef4444; border-color: #ef4444; }"_s);
    connect(m_rejectAllBtn, &QPushButton::clicked, this, &EditTracker::rejectAll);
    barLayout->addWidget(m_rejectAllBtn);

    // Style the bar itself
    m_barWidget->setStyleSheet(
        u"#EditTrackerBar {"
        u"  background-color: #1f1f23;"
        u"  border-top: 1px solid #333338;"
        u"  border-bottom: 1px solid #333338;"
        u"}"_s);

    root->addWidget(m_barWidget);

    // Expanded list container (initially hidden)
    m_listContainer = new QWidget(this);
    m_listContainer->hide();
    m_listLayout = new QVBoxLayout(m_listContainer);
    m_listLayout->setContentsMargins(12, 8, 12, 8);
    m_listLayout->setSpacing(8);

    // Scroll area for the list
    m_scrollArea = new QScrollArea(m_listContainer);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet(
        u"QScrollArea { background-color: #1a1a1a; border: none; }"
        u"QScrollBar:vertical { background: transparent; width: 8px; }"
        u"QScrollBar::handle:vertical { background: #333338; border-radius: 4px; min-height: 24px; }"
        u"QScrollBar::handle:vertical:hover { background: #4a4a52; }"_s);

    QWidget *scrollContent = new QWidget(m_scrollArea);
    m_listLayout->addWidget(m_scrollArea);
    m_scrollArea->setWidget(scrollContent);

    // The actual list layout inside the scroll area
    m_editListLayout = new QVBoxLayout(scrollContent);
    m_editListLayout->setContentsMargins(0, 0, 0, 0);
    m_editListLayout->setSpacing(8);
    m_editListLayout->setAlignment(Qt::AlignTop);

    root->addWidget(m_listContainer);

    // Overall style
    setStyleSheet(
        u"#EditTracker {"
        u"  background-color: #1a1a1a;"
        u"  border-top: 1px solid #333338;"
        u"}"_s);

    // Initially hide the bar and list container - the EditTracker itself stays in layout
    m_barWidget->hide();
    m_listContainer->hide();
}

void EditTracker::addEdit(const QString &path, const QString &toolName, const QString &diff,
                          const QString &oldContent, const QString &newContent)
{
    EditEntry entry;
    entry.path = path;
    entry.toolName = toolName;
    entry.diff = diff;
    entry.oldContent = oldContent;
    entry.newContent = newContent;
    entry.accepted = false;
    entry.rejected = false;

    m_edits[path] = entry;
    updateUI();
    Q_EMIT editsChanged(true);
}

void EditTracker::clear()
{
    m_edits.clear();
    updateUI();
    m_barWidget->hide();
    m_listContainer->hide();
    Q_EMIT editsChanged(false);
}

bool EditTracker::hasPendingEdits() const
{
    for (const auto &entry : m_edits) {
        if (!entry.accepted && !entry.rejected) {
            return true;
        }
    }
    return false;
}

QList<EditEntry> EditTracker::pendingEdits() const
{
    QList<EditEntry> result;
    for (const auto &entry : m_edits) {
        if (!entry.accepted && !entry.rejected) {
            result.append(entry);
        }
    }
    return result;
}

void EditTracker::acceptAll()
{
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        if (!it->accepted && !it->rejected) {
            it->accepted = true;
            Q_EMIT editAccepted(it->path, it->toolName, it->newContent);
        }
    }
    clear();
}

void EditTracker::rejectAll()
{
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        if (!it->accepted && !it->rejected) {
            it->rejected = true;
            Q_EMIT editRejected(it->path, it->toolName, it->oldContent);
        }
    }
    clear();
}

void EditTracker::updateUI()
{
    int pendingCount = 0;
    for (const auto &entry : m_edits) {
        if (!entry.accepted && !entry.rejected) {
            pendingCount++;
        }
    }

    if (pendingCount > 0) {
        m_countLabel->setText(i18np("1 pending edit", "%n pending edits", pendingCount));
        m_acceptAllBtn->setEnabled(true);
        m_rejectAllBtn->setEnabled(true);
        m_barWidget->show();
    } else {
        m_countLabel->clear();
        m_acceptAllBtn->setEnabled(false);
        m_rejectAllBtn->setEnabled(false);
        m_barWidget->hide();
    }

    rebuildEditList();
}

void EditTracker::rebuildEditList()
{
    // Clear existing edit widgets
    QLayoutItem *item;
    while ((item = m_editListLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    // Add widgets for each pending edit
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        const EditEntry &entry = it.value();
        if (entry.accepted || entry.rejected) {
            continue;
        }

        QWidget *editWidget = new QWidget(m_scrollArea->widget());
        editWidget->setStyleSheet(
            u"QWidget {"
            u"  background-color: #232326;"
            u"  border: 1px solid #333338;"
            u"  border-radius: 6px;"
            u"}"_s);
        auto *editLayout = new QVBoxLayout(editWidget);
        editLayout->setContentsMargins(12, 10, 12, 10);
        editLayout->setSpacing(8);

        // Header: path and tool name
        auto *headerLayout = new QHBoxLayout;
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(8);

        QLabel *pathLabel = new QLabel(entry.path, editWidget);
        pathLabel->setStyleSheet(u"QLabel { color: #e4e4e4; font-size: 12px; font-weight: bold; font-family: monospace; }"_s);
        pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        pathLabel->setWordWrap(true);
        headerLayout->addWidget(pathLabel, 1);

        const QString labelText = entry.toolName == u"write_file"_s
            ? i18n("New File")
            : ((entry.toolName == u"multi_edit_file"_s || entry.toolName == u"multi_replace_file_content"_s)
                   ? i18n("Multi-Edit")
                   : i18n("Edited"));
        QLabel *toolLabel = new QLabel(labelText, editWidget);
        toolLabel->setStyleSheet(
            u"QLabel {"
            u"  color: #888;"
            u"  font-size: 10px;"
            u"  font-weight: bold;"
            u"  padding: 2px 8px;"
            u"  background-color: #2a2a2a;"
            u"  border-radius: 3px;"
            u"}"_s);
        headerLayout->addWidget(toolLabel);

        editLayout->addLayout(headerLayout);

        // Diff preview
        if (!entry.diff.trimmed().isEmpty()) {
            QTextBrowser *diffBrowser = new QTextBrowser(editWidget);
            diffBrowser->setReadOnly(true);
            diffBrowser->setOpenExternalLinks(false);
            diffBrowser->setFrameShape(QFrame::NoFrame);
            diffBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            diffBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            diffBrowser->setStyleSheet(
                u"QTextBrowser {"
                u"  background-color: #11131a;"
                u"  color: #d4d4d4;"
                u"  border: 1px solid #2a3a22;"
                u"  border-radius: 4px;"
                u"  padding: 8px;"
                u"  font-family: monospace;"
                u"  font-size: 11px;"
                u"  line-height: 1.4;"
                u"}"_s);
            diffBrowser->document()->setDefaultStyleSheet(
                u"body { color: #d4d4d4; font-family: monospace; font-size: 11px; margin: 0; padding: 0; }"
                u".removed { color: #ef9999; background-color: #3a1a1a; }"
                u".added { color: #9ed36a; background-color: #1a3a1a; }"
                u".hunk { color: #888; }"
                u"pre { margin: 0; white-space: pre-wrap; font-family: monospace; font-size: 11px; }"
                u"p { margin: 0; white-space: pre-wrap; }"_s);

            // Convert diff to HTML (reuse logic from ToolCallWidget)
            auto escapeHtml = [](const QString &s) -> QString {
                QString out = s;
                out.replace(u"&"_s, u"&"_s);
                out.replace(u"<"_s, u"<"_s);
                out.replace(u">"_s, u">"_s);
                return out;
            };

            QString html = u"<body>"_s;
            for (const QString &line : entry.diff.split(u'\n')) {
                if (line.startsWith(u"---"_s) || line.startsWith(u"+++"_s)) {
                    html += u"<p class='hunk'>%1</p>"_s.arg(escapeHtml(line));
                } else if (line.startsWith(u"+"_s)) {
                    html += u"<p class='added'>%1</p>"_s.arg(escapeHtml(line));
                } else if (line.startsWith(u"-"_s)) {
                    html += u"<p class='removed'>%1</p>"_s.arg(escapeHtml(line));
                } else {
                    html += u"<p>%1</p>"_s.arg(escapeHtml(line));
                }
            }
            html += u"</body>"_s;
            diffBrowser->setHtml(html);

            // Calculate height based on content
            diffBrowser->document()->setTextWidth(diffBrowser->viewport()->width());
            int docHeight = static_cast<int>(diffBrowser->document()->size().height()) + 16;
            diffBrowser->setFixedHeight(qMin(300, qMax(60, docHeight)));

            editLayout->addWidget(diffBrowser);
        }

        // Accept/Reject buttons for this edit
        auto *btnLayout = new QHBoxLayout;
        btnLayout->setContentsMargins(0, 4, 0, 0);
        btnLayout->setSpacing(8);
        btnLayout->addStretch();

        QPushButton *acceptBtn = new QPushButton(i18n("Accept"), editWidget);
        acceptBtn->setCursor(Qt::PointingHandCursor);
        acceptBtn->setFixedHeight(28);
        acceptBtn->setStyleSheet(
            u"QPushButton {"
            u"  background-color: #007acc;"
            u"  color: #ffffff;"
            u"  border: none;"
            u"  border-radius: 4px;"
            u"  padding: 0 16px;"
            u"  font-weight: bold;"
            u"  font-size: 11px;"
            u"}"
            u"QPushButton:hover { background-color: #0062a3; }"
            u"QPushButton:pressed { background-color: #004d80; }"_s);
        connect(acceptBtn, &QPushButton::clicked, this, [this, path = entry.path]() {
            acceptEdit(path);
        });
        btnLayout->addWidget(acceptBtn);

        QPushButton *rejectBtn = new QPushButton(i18n("Reject"), editWidget);
        rejectBtn->setCursor(Qt::PointingHandCursor);
        rejectBtn->setFixedHeight(28);
        rejectBtn->setStyleSheet(
            u"QPushButton {"
            u"  background-color: transparent;"
            u"  color: #888;"
            u"  border: 1px solid #333;"
            u"  border-radius: 4px;"
            u"  padding: 0 16px;"
            u"  font-size: 11px;"
            u"}"
            u"QPushButton:hover { background-color: #2e1a1a; color: #ef4444; border-color: #ef4444; }"_s);
        connect(rejectBtn, &QPushButton::clicked, this, [this, path = entry.path]() {
            rejectEdit(path);
        });
        btnLayout->addWidget(rejectBtn);

        editLayout->addLayout(btnLayout);

        m_editListLayout->addWidget(editWidget);
    }

    m_editListLayout->addStretch();
}

void EditTracker::setExpanded(bool expanded)
{
    m_expanded = expanded;
    m_expandBtn->setText(expanded ? u"▾"_s : u"▸"_s);
    m_listContainer->setVisible(expanded);
}

void EditTracker::acceptEdit(const QString &path)
{
    auto it = m_edits.find(path);
    if (it != m_edits.end() && !it->accepted && !it->rejected) {
        it->accepted = true;
        Q_EMIT editAccepted(it->path, it->toolName, it->newContent);
        rebuildEditList();
        updateUI();

        // If no more pending edits, clear
        if (!hasPendingEdits()) {
            clear();
        }
    }
}

void EditTracker::rejectEdit(const QString &path)
{
    auto it = m_edits.find(path);
    if (it != m_edits.end() && !it->accepted && !it->rejected) {
        it->rejected = true;
        Q_EMIT editRejected(it->path, it->toolName, it->oldContent);
        rebuildEditList();
        updateUI();

        // If no more pending edits, clear
        if (!hasPendingEdits()) {
            clear();
        }
    }
}

} // namespace KateAi