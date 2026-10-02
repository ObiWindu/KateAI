/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "permissionbar.h"

#include "chattheme.h"

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
    m_title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 13px; font-weight: 600; }")
                               .arg(ChatTheme::textPrimary()));
    headerLayout->addWidget(m_title, 1);

    root->addLayout(headerLayout);

    m_details = new QPlainTextEdit(this);
    m_details->setReadOnly(true);
    m_details->setMaximumHeight(140);
    m_details->setPlaceholderText(i18n("Details"));
    m_details->setStyleSheet(
        QStringLiteral(
            "QPlainTextEdit {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 6px;"
            "  padding: 8px;"
            "  font-family: monospace;"
            "  font-size: 11px;"
            "}")
        .arg(ChatTheme::panelBg(), ChatTheme::textMuted(), ChatTheme::border()));
    root->addWidget(m_details);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 4, 0, 0);
    buttons->setSpacing(8);

    // Primary action first, then the durable variant, then the escape hatch.
    // Approving is the expected response, so it is the filled button.
    m_allowBtn = new QPushButton(i18n("Allow"), this);
    m_allowBtn->setCursor(Qt::PointingHandCursor);
    m_allowBtn->setDefault(true);
    m_allowBtn->setStyleSheet(
        QStringLiteral(
            "QPushButton {"
            "  background-color: %1;"
            "  color: #ffffff;"
            "  border: none;"
            "  border-radius: 6px;"
            "  padding: 6px 16px;"
            "  font-weight: 600;"
            "  font-size: 12px;"
            "}"
            "QPushButton:hover { background-color: %2; }"
            "QPushButton:pressed { background-color: %2; }")
            .arg(ChatTheme::accent(), ChatTheme::accentHover()));

    m_sessionBtn = new QPushButton(i18n("Always"), this);
    m_sessionBtn->setCursor(Qt::PointingHandCursor);
    m_sessionBtn->setToolTip(i18n("Approve this tool for the rest of the session"));
    m_sessionBtn->setStyleSheet(
        QStringLiteral(
            "QPushButton {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 6px;"
            "  padding: 6px 14px;"
            "  font-size: 12px;"
            "}"
            "QPushButton:hover { background-color: %3; color: #ffffff; }")
            .arg(ChatTheme::cardBg(), ChatTheme::textPrimary(), ChatTheme::hoverBg()));

    m_denyBtn = new QPushButton(i18n("Deny"), this);
    m_denyBtn->setCursor(Qt::PointingHandCursor);
    m_denyBtn->setStyleSheet(
        QStringLiteral(
            "QPushButton {"
            "  background-color: transparent;"
            "  color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 6px;"
            "  padding: 6px 14px;"
            "  font-size: 12px;"
            "}"
            "QPushButton:hover { color: #ffffff; border-color: %1; }")
            .arg(ChatTheme::danger(), ChatTheme::danger()));

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
        riskColor = ChatTheme::success();
        riskText = i18n("Read");
        badgeBg = QStringLiteral("rgba(78, 201, 160, 0.15)");
        break;
    case ToolRisk::Write:
        riskColor = ChatTheme::warning();
        riskText = i18n("Edit");
        badgeBg = QStringLiteral("rgba(226, 179, 65, 0.15)");
        break;
    case ToolRisk::Execute:
        riskColor = ChatTheme::danger();
        riskText = i18n("Execute");
        badgeBg = QStringLiteral("rgba(242, 109, 109, 0.15)");
        break;
    }

    m_riskBadge->setText(riskText);
    m_riskBadge->setStyleSheet(
        QStringLiteral(
            "QLabel {"
            "  background-color: %1;"
            "  color: %2;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.5px;"
            "  padding: 2px 7px;"
            "  border-radius: 4px;"
            "  border: 1px solid %2;"
            "}")
        .arg(badgeBg, riskColor));

    setStyleSheet(
        QStringLiteral(
            "#PermissionBar {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-left: 3px solid %3;"
            "  border-radius: 8px;"
            "}")
        .arg(ChatTheme::cardBg(), ChatTheme::border(), riskColor));
}

void PermissionBar::showRequest(const PermissionRequest &request)
{
    updateStyle(request.risk);
    m_title->setText(request.summary.isEmpty()
                         ? i18n("Permission needed for %1", ChatTheme::toolLabel(request.toolName))
                         : request.summary);
    m_details->setPlainText(request.details);
    // Empty details should not leave a dead box in the card.
    m_details->setVisible(!request.details.trimmed().isEmpty());
    show();
}

void PermissionBar::hideBar()
{
    hide();
    m_details->clear();
}

} // namespace KateAi
