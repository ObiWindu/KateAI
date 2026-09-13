#include "promptedit.h"

#include <KLocalizedString>
#include <QKeyEvent>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

PromptEdit::PromptEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setPlaceholderText(i18n("Ask anything, @ to mention…"));
    setTabChangesFocus(true);
    setUndoRedoEnabled(true);
    document()->setDocumentMargin(8);

    connect(this, &QPlainTextEdit::textChanged, this, &PromptEdit::autoGrow);
    autoGrow();
}

void PromptEdit::autoGrow()
{
    const int docH = static_cast<int>(document()->size().height());
    const int lineH = fontMetrics().lineSpacing();
    const int minH = lineH * 2 + 16;
    const int maxH = lineH * 8 + 16;
    const int targetH = std::clamp(docH + 16, minH, maxH);
    setFixedHeight(targetH);
}

QSize PromptEdit::sizeHint() const
{
    const int h = fontMetrics().lineSpacing() * 2 + 16;
    return {QPlainTextEdit::sizeHint().width(), h};
}

QSize PromptEdit::minimumSizeHint() const
{
    const int h = fontMetrics().lineSpacing() * 2 + 16;
    return {QPlainTextEdit::minimumSizeHint().width(), h};
}

void PromptEdit::keyPressEvent(QKeyEvent *event)
{
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (enter && !(event->modifiers() & Qt::ShiftModifier)) {
        Q_EMIT submitRequested();
        event->accept();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

} // namespace KateAi
