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

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

EditTracker::EditTracker(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"EditTracker"_s);
    setFixedHeight(44); // Compact bar height

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(12, 4, 12, 4);
    root->setSpacing(8);

    // Left side: icon and count
    m_iconLabel = new QLabel(u"✏️"_s, this);
    m_iconLabel->setStyleSheet(u"QLabel { font-size: 16px; }"_s);
    root->addWidget(m_iconLabel);

    m_countLabel = new QLabel(this);
    m_countLabel->setStyleSheet(u"QLabel { color: #e4e4e4; font-size: 12px; font-weight: bold; }"_s);
    root->addWidget(m_countLabel);

    root->addStretch();

    // Right side: Accept All / Reject All buttons
    m_acceptAllBtn = new QPushButton(i18n("Accept All"), this);
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
    root->addWidget(m_acceptAllBtn);

    m_rejectAllBtn = new QPushButton(i18n("Reject All"), this);
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
    root->addWidget(m_rejectAllBtn);

    // Style the bar itself
    setStyleSheet(
        u"#EditTracker {"
        u"  background-color: #1f1f23;"
        u"  border-top: 1px solid #333338;"
        u"  border-bottom: 1px solid #333338;"
        u"}"_s);

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
    updateUI();
    show();
    Q_EMIT editsChanged(true);
}

void EditTracker::clear()
{
    m_edits.clear();
    updateUI();
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
    } else {
        m_countLabel->clear();
        m_acceptAllBtn->setEnabled(false);
        m_rejectAllBtn->setEnabled(false);
    }
}

} // namespace KateAi