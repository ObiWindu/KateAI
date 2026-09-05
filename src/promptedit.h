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

Q_SIGNALS:
    void submitRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
};

} // namespace KateAi
