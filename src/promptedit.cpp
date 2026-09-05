#include "promptedit.h"

#include <KLocalizedString>
#include <QKeyEvent>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

PromptEdit::PromptEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setPlaceholderText(i18n("Ask Kate AI to read, write, or explain code…"));
    setTabChangesFocus(true);
    setUndoRedoEnabled(true);
    document()->setDocumentMargin(8);
}

QSize PromptEdit::sizeHint() const
{
    const int h = fontMetrics().lineSpacing() * 4 + 16;
    return {QPlainTextEdit::sizeHint().width(), h};
}

QSize PromptEdit::minimumSizeHint() const
{
    const int h = fontMetrics().lineSpacing() * 3 + 12;
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
