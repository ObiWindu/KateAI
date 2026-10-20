/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace KateAi
{

class PermissionBar : public QWidget
{
    Q_OBJECT

public:
    explicit PermissionBar(QWidget *parent = nullptr);

    void showRequest(const PermissionRequest &request);
    void hideBar();
    // Re-applies every theme-derived stylesheet, so a theme change repaints a
    // bar that is currently showing, not just the next one.
    void applyStyle();

Q_SIGNALS:
    void decided(PermissionDecision decision);

private:
    void updateStyle(ToolRisk risk);

    QLabel *m_riskBadge = nullptr;
    QLabel *m_title = nullptr;
    QPlainTextEdit *m_details = nullptr;
    QPushButton *m_allowBtn = nullptr;
    QPushButton *m_sessionBtn = nullptr;
    QPushButton *m_denyBtn = nullptr;
    ToolRisk m_risk = ToolRisk::Read;
};

} // namespace KateAi
