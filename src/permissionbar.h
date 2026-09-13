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
};

} // namespace KateAi
