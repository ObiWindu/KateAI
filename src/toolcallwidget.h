/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPropertyAnimation;
class QPushButton;
class QTextBrowser;

namespace KateAi
{

/**
 * A collapsible card that renders a single tool call inline in the chat transcript.
 *
 * Zed's agent panel shows each tool call as a compact summary (icon + name + path)
 * that can be expanded to reveal arguments and output. This widget replicates that
 * pattern inside a QTextBrowser-based transcript by being embedded as a widget via
 * QTextBrowser::document()->addResource() — or, more practically, inserted into the
 * transcript layout outside the QTextBrowser.
 */
class ToolCallWidget : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(int expandedHeight READ expandedHeight WRITE setExpandedHeight)

public:
    explicit ToolCallWidget(const QString &toolCallId, QWidget *parent = nullptr);

    void setToolInfo(const QString &toolName, const QString &summary, ToolRisk risk);
    void setRunning();
    void setFinished(const ToolResult &result);

    /**
     * Attach the pre-formatted unified diff (write_file / edit_file) so the
     * proposed change is visible in the chat transcript before the user
     * approves it. Empty when the tool is not a mutation.
     */
    void setDescribeDiff(const QString &diff);

    QString toolCallId() const { return m_toolCallId; }
    int expandedHeight() const { return m_expandedHeight; }
    void setExpandedHeight(int h);
    bool isRunning() const { return !m_finished; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void toggleExpand();
    void updateStyle();
    QString diffToHtml(const QString &diff) const;
    QString escapeHtml(const QString &s) const;
    QString iconForTool(const QString &toolName) const;
    QString colorForRisk(ToolRisk risk) const;
    bool isDiffTool(const QString &toolName) const;

    QString m_toolCallId;
    QString m_toolName;
    ToolRisk m_risk = ToolRisk::Read;
    bool m_expanded = true;
    bool m_finished = false;
    bool m_ok = true;
    int m_expandedHeight = 0;

    QWidget *m_header = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_expandBtn = nullptr;
    QWidget *m_detailsContainer = nullptr;
    QPlainTextEdit *m_details = nullptr;
    QTextBrowser *m_describeDiff = nullptr;
    QPropertyAnimation *m_animation = nullptr;
};

} // namespace KateAi
