/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QPropertyAnimation;
class QPushButton;
class QShowEvent;
class QTextBrowser;
class QTimer;

namespace KateAi
{

/**
 * Which surface a tool call expands into.
 *
 * A card is not one shape for every tool: an edit shows what changed, a read
 * shows file contents, a command shows its output. The card picks one of these
 * when the tool name is known, so the expanded body matches the kind of thing
 * the call actually produced instead of dumping everything through the same
 * flat text view.
 */
enum class ToolDetailView {
    Plain,   ///< Anything without a dedicated surface.
    Diff,    ///< File edit / write: added and removed lines, colour-coded.
    Code,    ///< File reads and rule loads: the file itself.
    Terminal, ///< Commands: output on a terminal-coloured surface.
    Search,  ///< grep / glob: matches.
};

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
    // Streams the output of a running tool into the card as it arrives, so a
    // long command shows its progress instead of one lump at the end. Ignored
    // once the tool has finished: then setFinished's text is the truth.
    void setPartialOutput(const QString &partialOutput);
    // The call's own arguments, shown in the body while the tool runs so a card
    // in flight says what it is working on instead of only naming the tool.
    // Anything that arrives later -- a live stream or the result -- replaces it.
    void setRunningInput(const QString &input);

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
     * approves it. Empty when the tool is not a mutation. The diff is rendered
     * on its own surface with added lines green and removed lines red.
     */
    void setDescribeDiff(const QString &diff);
    void setFilePath(const QString &path);

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
    // Marks the card as the navigation target: a ring that fades after a
    // moment, so a card jumped to with the keyboard can be found again after
    // the scroll that carried it into view.
    void flashAttention();
    void reflowNow();
    // Re-applies the card's stylesheets. The card's colours are fixed and
    // theme-independent, so this exists to keep the restyle plumbing uniform
    // with the other cards, not because the card's look changes.
    void applyStyle();

Q_SIGNALS:
    void approvalChosen(PermissionDecision decision);
    void expandedChanged(bool expanded);
    void openFileRequested(const QString &path, int line);

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
    void reflowDetails();
    void applyDetailsHeight();
    void applyDetailView();
    // Styles the result surface for the view the card is in: the terminal
    // surface for commands, the inset surface for everything else. Called by
    // applyDetailView() when the view changes and by applyStyle() to re-derive
    // the sheet in place.
    void applyDetailsSurface(ToolDetailView view);
    bool hasDetailsBody() const;
    // What the copy button would put on the clipboard right now.
    QString copyableText() const;
    void updateCopyButton();
    // Whether a diff is attached to the card at all, independently of whether
    // the diff surface has been shown yet: a card is filled before it is ever
    // displayed, so its visibility says nothing about the state of its body.
    bool hasDiffBody() const;
    static ToolDetailView detailViewForTool(const QString &toolName);
    QString detailViewCaption() const;
    // What the body holds right now, as opposed to the shape the tool's results
    // take: the arguments while it runs, its output once it has reported.
    QString bodyCaption() const;
    int captionHeight() const;
    int browserFitHeight(QTextBrowser *browser, int padding) const;
    int detailsFitHeight() const;
    QString diffToHtml(const QString &diff) const;
    QString plainToHtml(const QString &text) const;
    QString escapeHtml(const QString &s) const;
    QString iconForTool(const QString &toolName) const;
    QString colorForRisk(ToolRisk risk) const;
    // Colours the in-flight glyph with the tool's own hue; called again once
    // setToolInfo has supplied the name, because cards are created before it.
    void applyRunningStatusColor();
    QString partialToHtml(const QString &text) const;
    bool isDiffTool(const QString &toolName) const;
    QString languageForTool(const QString &toolName) const;

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
    ToolDetailView m_detailView = ToolDetailView::Plain;
    QString m_filePath;

    QElapsedTimer m_elapsed;
    QTimer *m_durationTimer = nullptr;
    // Output captured from the running tool so far. Cleared by setFinished,
    // which replaces it with the final result.
    QString m_partialOutput;
    // The arguments shown while the tool runs, and the body they are rendered
    // on: bodyCaption() names what the body currently holds, so it reads
    // "Input" while the arguments are up and "Output" once real output arrives.
    QString m_runningInput;
    bool m_showingInput = false;

    QWidget *m_header = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_diffStat = nullptr;
    QLabel *m_duration = nullptr;
    QPushButton *m_expandBtn = nullptr;
    QPushButton *m_copyBtn = nullptr;
    QWidget *m_approvalRow = nullptr;
    QLabel *m_approvalHint = nullptr;
    QLabel *m_approvalSubject = nullptr;
    QPushButton *m_allowBtn = nullptr;
    QPushButton *m_alwaysBtn = nullptr;
    QPushButton *m_denyBtn = nullptr;
    QWidget *m_detailsContainer = nullptr;
    // The complete result, clamped only by the card's display rules. Held
    // separately from what is rendered so the copy button can hand over the
    // whole output rather than the truncated view of it.
    QString m_resultOutput;
    // Names the surface the card expanded into, so a diff, file contents and
    // command output are told apart before anything is read.
    QLabel *m_viewCaption = nullptr;
    // The diff of a file edit or write, on a surface of its own: the change is
    // the whole point of the card, and rendering it through the plain result
    // view left the added and removed lines as indistinguishable text.
    QTextBrowser *m_describeDiff = nullptr;
    QTextBrowser *m_details = nullptr;
    QPropertyAnimation *m_animation = nullptr;
    QTimer *m_reflowTimer = nullptr;
};

} // namespace KateAi
