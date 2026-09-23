/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "permissionbar.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

PermissionBar::PermissionBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"PermissionBar"_s);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    auto *headerLayout = new QHBoxLayout;
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);

    m_riskBadge = new QLabel(this);
    headerLayout->addWidget(m_riskBadge);

    m_title = new QLabel(this);
    m_title->setWordWrap(true);
    m_title->setStyleSheet(u"QLabel { color: #e4e4e4; font-size: 12px; font-weight: bold; }"_s);
    headerLayout->addWidget(m_title, 1);

    root->addLayout(headerLayout);

    m_details = new QPlainTextEdit(this);
    m_details->setReadOnly(true);
    m_details->setMaximumHeight(140);
    m_details->setPlaceholderText(i18n("Details"));
    m_details->setStyleSheet(
        u"QPlainTextEdit {"
        u"  background-color: #161616;"
        u"  color: #ccc;"
        u"  border: 1px solid #2e2e2e;"
        u"  border-radius: 4px;"
        u"  padding: 8px;"
        u"  font-family: monospace;"
        u"  font-size: 11px;"
        u"}"
        u"QMenu { background-color: #252528; color: #cccccc; border: 1px solid #3c3c40; border-radius: 6px; padding: 4px; }"
        u"QMenu::item { padding: 6px 18px 6px 12px; border-radius: 4px; }"
        u"QMenu::item:selected { background-color: #007acc; color: #ffffff; }"
        u"QMenu::separator { height: 1px; background-color: #38383e; margin: 4px 0; }"_s);
    root->addWidget(m_details);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 4, 0, 0);
    buttons->setSpacing(8);

    m_allowBtn = new QPushButton(i18n("Allow"), this);
    m_allowBtn->setCursor(Qt::PointingHandCursor);
    m_allowBtn->setStyleSheet(
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

    m_sessionBtn = new QPushButton(i18n("Allow for Session"), this);
    m_sessionBtn->setCursor(Qt::PointingHandCursor);
    m_sessionBtn->setStyleSheet(
        u"QPushButton {"
        u"  background-color: #2a2a2a;"
        u"  color: #ccc;"
        u"  border: 1px solid #3c3c3c;"
        u"  border-radius: 4px;"
        u"  padding: 5px 12px;"
        u"  font-size: 12px;"
        u"}"
        u"QPushButton:hover { background-color: #333; color: #fff; border-color: #555; }"
        u"QPushButton:pressed { background-color: #222; }"_s);

    m_denyBtn = new QPushButton(i18n("Deny"), this);
    m_denyBtn->setCursor(Qt::PointingHandCursor);
    m_denyBtn->setStyleSheet(
        u"QPushButton {"
        u"  background-color: transparent;"
        u"  color: #888;"
        u"  border: 1px solid #333;"
        u"  border-radius: 4px;"
        u"  padding: 5px 12px;"
        u"  font-size: 12px;"
        u"}"
        u"QPushButton:hover { background-color: #2e1a1a; color: #ef4444; border-color: #ef4444; }"_s);

    buttons->addWidget(m_allowBtn);
    buttons->addWidget(m_sessionBtn);
    buttons->addWidget(m_denyBtn);
    buttons->addStretch();
    root->addLayout(buttons);

    connect(m_allowBtn, &QPushButton::clicked, this, [this]() {
        hideBar();
        Q_EMIT decided(PermissionDecision::AllowOnce);
    });
    connect(m_sessionBtn, &QPushButton::clicked, this, [this]() {
        hideBar();
        Q_EMIT decided(PermissionDecision::AllowSession);
    });
    connect(m_denyBtn, &QPushButton::clicked, this, [this]() {
        hideBar();
        Q_EMIT decided(PermissionDecision::Deny);
    });

    hide();
}

void PermissionBar::updateStyle(ToolRisk risk)
{
    QString riskColor;
    QString riskText;
    QString badgeBg;

    switch (risk) {
    case ToolRisk::Read:
        riskColor = u"#22c55e"_s;
        riskText = i18n("READ");
        badgeBg = u"rgba(34, 197, 94, 0.15)"_s;
        break;
    case ToolRisk::Write:
        riskColor = u"#eab308"_s;
        riskText = i18n("EDIT");
        badgeBg = u"rgba(234, 179, 8, 0.15)"_s;
        break;
    case ToolRisk::Execute:
        riskColor = u"#ef4444"_s;
        riskText = i18n("EXECUTE");
        badgeBg = u"rgba(239, 68, 68, 0.15)"_s;
        break;
    }

    m_riskBadge->setText(riskText);
    m_riskBadge->setStyleSheet(
        u"QLabel {"
        u"  background-color: %1;"
        u"  color: %2;"
        u"  font-size: 10px;"
        u"  font-weight: bold;"
        u"  padding: 2px 6px;"
        u"  border-radius: 3px;"
        u"  border: 1px solid %2;"
        u"}"_s.arg(badgeBg, riskColor));

    setStyleSheet(
        u"#PermissionBar {"
        u"  background-color: #1f1f23;"
        u"  border: 1px solid #333338;"
        u"  border-left: 4px solid %1;"
        u"  border-radius: 6px;"
        u"  margin: 6px 8px;"
        u"}"_s.arg(riskColor));
}

void PermissionBar::showRequest(const PermissionRequest &request)
{
    updateStyle(request.risk);
    m_title->setText(request.summary.isEmpty() ? i18n("Permission needed for %1", request.toolName) : request.summary);
    m_details->setPlainText(request.details);
    show();
}

void PermissionBar::hideBar()
{
    hide();
    m_details->clear();
}

} // namespace KateAi
