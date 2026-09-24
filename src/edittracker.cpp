/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "edittracker.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBrowser>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

EditTracker::EditTracker(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"EditTracker"_s);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Scroll area for edit entries
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setStyleSheet(
        u"QScrollArea {"
        u"  background-color: transparent;"
        u"  border: none;"
        u"}"
        u"QScrollBar:vertical {"
        u"  background-color: #1a1a1a;"
        u"  width: 8px;"
        u"  border: none;"
        u"}"
        u"QScrollBar::handle:vertical {"
        u"  background-color: #444;"
        u"  border-radius: 4px;"
        u"  min-height: 30px;"
        u"}"
        u"QScrollBar::handle:vertical:hover {"
        u"  background-color: #555;"
        u"}"
        u"QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        u"  height: 0;"
        u"}"_s);

    m_container = new QWidget();
    m_container->setStyleSheet(u"QWidget { background-color: transparent; }"_s);
    m_layout = new QVBoxLayout(m_container);
    m_layout->setContentsMargins(8, 8, 8, 8);
    m_layout->setSpacing(8);
    m_layout->addStretch();

    m_scrollArea->setWidget(m_container);
    root->addWidget(m_scrollArea);

    // Global accept/reject bar
    m_globalBar = new QWidget(this);
    m_globalBar->setObjectName(u"EditTrackerGlobalBar"_s);
    m_globalBar->setStyleSheet(
        u"#EditTrackerGlobalBar {"
        u"  background-color: #1f1f23;"
        u"  border-top: 1px solid #333338;"
        u"  border-radius: 0;"
        u"}"_s);
    m_globalBar->hide();

    auto *globalLayout = new QHBoxLayout(m_globalBar);
    globalLayout->setContentsMargins(12, 8, 12, 8);
    globalLayout->setSpacing(12);

    m_countLabel = new QLabel(this);
    m_countLabel->setStyleSheet(u"QLabel { color: #e4e4e4; font-size: 12px; font-weight: bold; }"_s);
    globalLayout->addWidget(m_countLabel);

    globalLayout->addStretch();

    m_acceptAllBtn = new QPushButton(i18n("Accept All"), this);
    m_acceptAllBtn->setCursor(Qt::PointingHandCursor);
    m_acceptAllBtn->setStyleSheet(
        u"QPushButton {"
        u"  background-color: #007acc;"
        u"  color: #ffffff;"
        u"  border: none;"
        u"  border-radius: 4px;"
        u"  padding: 6px 16px;"
        u"  font-weight: bold;"
        u"  font-size: 12px;"
        u"}"
        u"QPushButton:hover { background-color: #0062a3; }"
        u"QPushButton:pressed { background-color: #004d80; }"_s);
    connect(m_acceptAllBtn, &QPushButton::clicked, this, &EditTracker::acceptAll);
    globalLayout->addWidget(m_acceptAllBtn);

    m_rejectAllBtn = new QPushButton(i18n("Reject All"), this);
    m_rejectAllBtn->setCursor(Qt::PointingHandCursor);
    m_rejectAllBtn->setStyleSheet(
        u"QPushButton {"
        u"  background-color: transparent;"
        u"  color: #888;"
        u"  border: 1px solid #333;"
        u"  border-radius: 4px;"
        u"  padding: 6px 16px;"
        u"  font-size: 12px;"
        u"}"
        u"QPushButton:hover { background-color: #2e1a1a; color: #ef4444; border-color: #ef4444; }"_s);
    connect(m_rejectAllBtn, &QPushButton::clicked, this, &EditTracker::rejectAll);
    globalLayout->addWidget(m_rejectAllBtn);

    root->addWidget(m_globalBar);

    hide(); // Initially hidden
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
    rebuildUI();
    show();
    Q_EMIT editsChanged(true);
}

void EditTracker::clear()
{
    m_edits.clear();
    rebuildUI();
    hide();
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

void EditTracker::rebuildUI()
{
    // Clear existing widgets
    QLayoutItem *item;
    while ((item = m_layout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    int pendingCount = 0;
    for (const auto &entry : m_edits) {
        if (!entry.accepted && !entry.rejected) {
            createEditWidget(entry);
            pendingCount++;
        }
    }

    m_layout->addStretch();

    // Update global bar
    if (pendingCount > 0) {
        m_countLabel->setText(i18np("1 pending edit", "%n pending edits", pendingCount));
        m_globalBar->show();
    } else {
        m_globalBar->hide();
    }
}

void EditTracker::createEditWidget(const EditEntry &entry)
{
    auto *widget = new QWidget(m_container);
    widget->setStyleSheet(
        u"QWidget {"
        u"  background-color: #232326;"
        u"  border: 1px solid #38383e;"
        u"  border-radius: 6px;"
        u"}"_s);

    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    // Header: file path and tool type
    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);

    QString icon = entry.toolName == u"write_file"_s ? u"📝"_s : u"✏️"_s;
    auto *iconLabel = new QLabel(icon, widget);
    iconLabel->setStyleSheet(u"QLabel { font-size: 16px; }"_s);
    headerLayout->addWidget(iconLabel);

    auto *pathLabel = new QLabel(entry.path, widget);
    pathLabel->setStyleSheet(u"QLabel { color: #e4e4e4; font-size: 12px; font-family: monospace; }"_s);
    pathLabel->setWordWrap(true);
    headerLayout->addWidget(pathLabel, 1);

    auto *toolBadge = new QLabel(entry.toolName == u"write_file"_s ? i18n("New File") : i18n("Edit"), widget);
    toolBadge->setStyleSheet(
        u"QLabel {"
        u"  background-color: #eab308;"
        u"  color: #1a1a1a;"
        u"  font-size: 10px;"
        u"  font-weight: bold;"
        u"  padding: 2px 8px;"
        u"  border-radius: 3px;"
        u"}"_s);
    headerLayout->addWidget(toolBadge);

    layout->addLayout(headerLayout);

    // Diff preview
    if (!entry.diff.trimmed().isEmpty()) {
        auto *diffBrowser = new QTextBrowser(widget);
        diffBrowser->setReadOnly(true);
        diffBrowser->setOpenExternalLinks(false);
        diffBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        diffBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        diffBrowser->setMaximumHeight(200);
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
            u"}"
            u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
            u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
            u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
            u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);
        diffBrowser->document()->setDefaultStyleSheet(
            u"body { color: #d4d4d4; font-family: monospace; font-size: 11px; margin: 0; padding: 0; }"
            u".removed { color: #ef9999; background-color: #3a1a1a; }"
            u".added { color: #9ed36a; background-color: #1a3a1a; }"
            u".hunk { color: #888; }"
            u"p { margin: 0; }"_s);

        // Convert diff to HTML
        QString html = u"<body>"_s;
        for (const QString &line : entry.diff.split(u'\n')) {
            if (line.startsWith(u"---"_s) || line.startsWith(u"+++"_s)) {
                html += u"<p class='hunk'>%1</p>"_s.arg(line.toHtmlEscaped());
            } else if (line.startsWith(u"+"_s)) {
                html += u"<p class='added'>%1</p>"_s.arg(line.toHtmlEscaped());
            } else if (line.startsWith(u"-"_s)) {
                html += u"<p class='removed'>%1</p>"_s.arg(line.toHtmlEscaped());
            } else {
                html += u"<p>%1</p>"_s.arg(line.toHtmlEscaped());
            }
        }
        html += u"</body>"_s;
        diffBrowser->setHtml(html);

        // Adjust height to content
        diffBrowser->document()->adjustSize();
        const int h = static_cast<int>(diffBrowser->document()->size().height()) + 16;
        diffBrowser->setFixedHeight(std::min(200, std::max(50, h)));

        layout->addWidget(diffBrowser);
    }

    // Accept/Reject buttons
    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 4, 0, 0);
    buttonLayout->setSpacing(8);
    buttonLayout->addStretch();

    auto *acceptBtn = new QPushButton(i18n("Accept"), widget);
    acceptBtn->setCursor(Qt::PointingHandCursor);
    acceptBtn->setStyleSheet(
        u"QPushButton {"
        u"  background-color: #007acc;"
        u"  color: #ffffff;"
        u"  border: none;"
        u"  border-radius: 4px;"
        u"  padding: 5px 14px;"
        u"  font-weight: bold;"
        u"  font-size: 12px;"
        u"}"
        u"QPushButton:hover { background-color: #0062a3; }"
        u"QPushButton:pressed { background-color: #004d80; }"_s);
    connect(acceptBtn, &QPushButton::clicked, this, [this, path = entry.path, toolName = entry.toolName, newContent = entry.newContent]() {
        if (auto it = m_edits.find(path); it != m_edits.end()) {
            it->accepted = true;
            Q_EMIT editAccepted(path, toolName, newContent);
            rebuildUI();
        }
    });
    buttonLayout->addWidget(acceptBtn);

    auto *rejectBtn = new QPushButton(i18n("Reject"), widget);
    rejectBtn->setCursor(Qt::PointingHandCursor);
    rejectBtn->setStyleSheet(
        u"QPushButton {"
        u"  background-color: transparent;"
        u"  color: #888;"
        u"  border: 1px solid #333;"
        u"  border-radius: 4px;"
        u"  padding: 5px 14px;"
        u"  font-size: 12px;"
        u"}"
        u"QPushButton:hover { background-color: #2e1a1a; color: #ef4444; border-color: #ef4444; }"_s);
    connect(rejectBtn, &QPushButton::clicked, this, [this, path = entry.path, toolName = entry.toolName, oldContent = entry.oldContent]() {
        if (auto it = m_edits.find(path); it != m_edits.end()) {
            it->rejected = true;
            Q_EMIT editRejected(path, toolName, oldContent);
            rebuildUI();
        }
    });
    buttonLayout->addWidget(rejectBtn);

    layout->addLayout(buttonLayout);

    m_layout->insertWidget(m_layout->count() - 1, widget); // Insert before stretch
}

} // namespace KateAi