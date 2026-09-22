/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QPlainTextEdit>

namespace KateAi
{

class PromptEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit PromptEdit(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public Q_SLOTS:
    void autoGrow();

Q_SIGNALS:
    void submitRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
};

} // namespace KateAi
