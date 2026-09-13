#pragma once

#include "types.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPropertyAnimation;
class QPushButton;

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

    QString toolCallId() const { return m_toolCallId; }
    int expandedHeight() const { return m_expandedHeight; }
    void setExpandedHeight(int h);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void toggleExpand();
    void updateStyle();
    QString iconForTool(const QString &toolName) const;
    QString colorForRisk(ToolRisk risk) const;

    QString m_toolCallId;
    ToolRisk m_risk = ToolRisk::Read;
    bool m_expanded = false;
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
    QPropertyAnimation *m_animation = nullptr;
};

} // namespace KateAi
