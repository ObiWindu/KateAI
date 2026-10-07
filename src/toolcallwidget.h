/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPropertyAnimation;
class QPushButton;
class QShowEvent;
class QTextBrowser;
class QTimer;
class QTimer;

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

    // --- Inline approval -----------------------------------------------------
    // The card that triggered the permission request shows the decision here,
    // at the point of the action, instead of in a bar below the transcript.
    // The card is created by the permissionNeeded signal, which fires before
    // toolStarted, so approval and execution share one widget.
    void showApproval();
    void setApprovalResolved(PermissionDecision decision);
    bool isAwaitingApproval() const
    {
        return m_awaitingApproval;
    }

    // Labels the docked strip with the target of the request, so the buttons
    // carry enough context to decide on without scrolling back to the card.
    void setApprovalSubject(const QString &subject);

    // The Allow / Always / Deny strip. ChatWidget reparents this into the
    // intent dock so the buttons sit above the input instead of wherever the
    // card happens to be scrolled to; the card keeps the diff being judged.
    QWidget *approvalRow() const
    {
        return m_approvalRow;
    }

    // --- Live timing ---------------------------------------------------------
    // Elapsed time per call, shown while running and frozen afterwards so a
    // slow tool is obvious in the transcript without opening it.
    void setDurationVisible(bool visible);

    /**
     * Attach the pre-formatted unified diff (write_file / edit_file) so the
     * proposed change is visible in the chat transcript before the user
     * approves it. Empty when the tool is not a mutation.
     */
    void setDescribeDiff(const QString &diff);
    void setPreviewText(const QString &text);

    QString toolCallId() const { return m_toolCallId; }
        QString toolName() const { return m_toolName; }
    int expandedHeight() const { return m_expandedHeight; }
    void setExpandedHeight(int h);
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }
    bool isRunning() const { return !m_finished && !m_denied; }
    bool hasDiffPreview() const { return m_hasDiffPreview; }
    bool isFileEditTool() const;
    void setActivityFrame(int frame);
    void reflowNow();

Q_SIGNALS:
    void approvalChosen(PermissionDecision decision);
    void expandedChanged(bool expanded);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

private:
    void toggleExpand();
    void updateStyle();
    void updateTitleText();
    void syncPreviewVisibility();
    void updateDuration();
    void scheduleReflow();
    void reflowPreview();
    void reflowDetails();
    void applyDetailsHeight();
    int previewFitHeight() const;
    int detailsFitHeight() const;
    QString diffToHtml(const QString &diff) const;
    QString plainToHtml(const QString &text) const;
    QString escapeHtml(const QString &s) const;
    QString iconForTool(const QString &toolName) const;
    QString colorForRisk(ToolRisk risk) const;
    bool isDiffTool(const QString &toolName) const;
    void showPreviewHtml(const QString &html);

    QString m_toolCallId;
    QString m_toolName;
    QString m_titleText;
    ToolRisk m_risk = ToolRisk::Read;
    bool m_expanded = false;
    bool m_finished = false;
    bool m_ok = true;
    bool m_hasDiffPreview = false;
    int m_expandedHeight = 0;
    bool m_denied = false;
    bool m_awaitingApproval = false;
    bool m_durationEnabled = false;
    qint64 m_diffAdded = 0;
    qint64 m_diffRemoved = 0;

    QElapsedTimer m_elapsed;
    QTimer *m_durationTimer = nullptr;

    QWidget *m_header = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_diffStat = nullptr;
    QLabel *m_duration = nullptr;
    QPushButton *m_expandBtn = nullptr;
    QWidget *m_approvalRow = nullptr;
    QLabel *m_approvalSubject = nullptr;
    QWidget *m_detailsContainer = nullptr;
    QPlainTextEdit *m_details = nullptr;
    QTextBrowser *m_describeDiff = nullptr;
    QPropertyAnimation *m_animation = nullptr;
    QTimer *m_reflowTimer = nullptr;
};

} // namespace KateAi
