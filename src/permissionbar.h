#pragma once

#include "types.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;

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
    QLabel *m_title = nullptr;
    QPlainTextEdit *m_details = nullptr;
};

} // namespace KateAi
