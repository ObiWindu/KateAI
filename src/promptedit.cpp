/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "promptedit.h"

#include "chattheme.h"

#include <KLocalizedString>
#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>

using namespace Qt::Literals::StringLiterals;

namespace
{

// How many lines of the composer are visible when it is empty, and how many
// before a scrollbar takes over from the growth. The empty height is half
// again what the composer settled at before (4.5 against 3); the ceiling was
// raised to 250 lines so a long pasted transcript stays readable in the box
// instead of scrolling almost immediately.
constexpr qreal minLines = 4.5;
constexpr qreal maxLines = 250.0;
// Breathing room above and below the text, also half again the old 16, so the
// empty box grows by half rather than only its text.
constexpr int padding = 24;

// The empty composer took more of the panel than the transcript below it, so
// the box is 10% shorter than the numbers above work out to: both the resting
// height and the ceiling a pasted transcript grows to, so the two still agree.
// One constant rather than shaved-off line counts, because the padding is part
// of the height and only the text was not.
constexpr qreal heightScale = 0.9;

} // namespace

namespace KateAi
{

PromptEdit::PromptEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setPlaceholderText(i18n("Ask anything, @ to mention…"));
    setTabChangesFocus(false);
    setUndoRedoEnabled(true);
    document()->setDocumentMargin(8);

    m_completionModel = new QStringListModel(this);
    m_completer = new QCompleter(m_completionModel, this);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);

    if (m_completer->popup()) {
        m_completer->popup()->setStyleSheet(
            QStringLiteral(
                u"QListView {"
                u"  background-color: %1;"
                u"  color: %2;"
                u"  border: 1px solid %3;"
                u"  border-radius: 6px;"
                u"  padding: 4px;"
                u"  font-size: 12px;"
                u"}"
                u"QListView::item {"
                u"  padding: 4px 8px;"
                u"  border-radius: 4px;"
                u"}"
                u"QListView::item:selected {"
                u"  background-color: %4;"
                u"  color: %5;"
                u"}")
            .arg(ChatTheme::surfaceBg(), ChatTheme::textPrimary(), ChatTheme::border(), ChatTheme::accent(),
                 ChatTheme::textOn(ChatTheme::accent())));
    }

    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated),
            this, &PromptEdit::insertCompletion);

    connect(this, &QPlainTextEdit::textChanged, this, &PromptEdit::autoGrow);
    autoGrow();
}

void PromptEdit::setCompletionWords(const QStringList &words)
{
    if (m_completionModel) {
        m_completionModel->setStringList(words);
    }
}

void PromptEdit::addHistory(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    if (m_history.isEmpty() || m_history.last() != trimmed) {
        m_history.append(trimmed);
    }
    m_historyIndex = -1;
    m_draft.clear();
}

int PromptEdit::minHeight() const
{
    return qRound((fontMetrics().lineSpacing() * minLines + padding) * heightScale);
}

int PromptEdit::maxHeight() const
{
    return qRound((fontMetrics().lineSpacing() * maxLines + padding) * heightScale);
}

void PromptEdit::autoGrow()
{
    // The composer gives four and a half lines of head-room when empty (a
    // pasted stack trace stays readable) and 250 lines of growth before
    // scrolling. Both bounds move together, so raising the empty height does
    // not quietly shrink how much may be typed before the scrollbar appears.
    // The text itself still gets its full height plus padding: only the empty
    // and the maximal box are scaled, so nothing the user typed is ever
    // cropped.
    const int docH = static_cast<int>(document()->size().height());
    const int targetH = std::clamp(docH + padding, minHeight(), maxHeight());
    setFixedHeight(targetH);
}

QSize PromptEdit::sizeHint() const
{
    return {QPlainTextEdit::sizeHint().width(), minHeight()};
}

QSize PromptEdit::minimumSizeHint() const
{
    return {QPlainTextEdit::minimumSizeHint().width(), minHeight()};
}

void PromptEdit::focusInEvent(QFocusEvent *event)
{
    if (m_completer) {
        m_completer->setWidget(this);
    }
    QPlainTextEdit::focusInEvent(event);
}

int PromptEdit::atSymbolPosition() const
{
    const QTextCursor tc = textCursor();
    const QString fullText = toPlainText();
    const int curPos = tc.position();
    if (curPos == 0) {
        return -1;
    }

    // Look backwards from cursor position
    for (int i = curPos - 1; i >= 0; --i) {
        const QChar ch = fullText.at(i);
        if (ch == u'@') {
            return i;
        }
        if (ch.isSpace()) {
            return -1;
        }
    }
    return -1;
}

void PromptEdit::insertCompletion(const QString &completion)
{
    const int atPos = atSymbolPosition();
    if (atPos < 0) {
        return;
    }

    QTextCursor tc = textCursor();
    tc.setPosition(atPos);
    tc.setPosition(textCursor().position(), QTextCursor::KeepAnchor);
    tc.insertText(u"@"_s + completion + u" "_s);
    setTextCursor(tc);
}

void PromptEdit::keyPressEvent(QKeyEvent *event)
{
    // Completer popup handling
    if (m_completer && m_completer->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
            event->ignore();
            return;
        case Qt::Key_Escape:
            m_completer->popup()->hide();
            event->accept();
            return;
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            QApplication::sendEvent(m_completer->popup(), event);
            return;
        default:
            break;
        }
    }

    // Escape cancels/stops
    if (event->key() == Qt::Key_Escape) {
        Q_EMIT escapePressed();
        event->accept();
        return;
    }

    // Enter submits (Shift+Enter for newline)
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (enter && !(event->modifiers() & Qt::ShiftModifier)) {
        if (m_completer && m_completer->popup()->isVisible()) {
            m_completer->popup()->hide();
        }
        Q_EMIT submitRequested();
        event->accept();
        return;
    }

    // History navigation with Up/Down arrow keys
    if (event->key() == Qt::Key_Up && !(event->modifiers() & Qt::ShiftModifier)) {
        const QTextCursor tc = textCursor();
        const bool isFirstLine = tc.blockNumber() == 0;
        const bool isEmpty = toPlainText().trimmed().isEmpty();
        if ((isFirstLine || isEmpty) && !m_history.isEmpty()) {
            if (m_historyIndex == -1) {
                m_draft = toPlainText();
                m_historyIndex = m_history.size() - 1;
            } else if (m_historyIndex > 0) {
                --m_historyIndex;
            }
            if (m_historyIndex >= 0 && m_historyIndex < m_history.size()) {
                setPlainText(m_history.at(m_historyIndex));
                moveCursor(QTextCursor::End);
            }
            event->accept();
            return;
        }
    } else if (event->key() == Qt::Key_Down && !(event->modifiers() & Qt::ShiftModifier)) {
        if (m_historyIndex >= 0) {
            ++m_historyIndex;
            if (m_historyIndex < m_history.size()) {
                setPlainText(m_history.at(m_historyIndex));
            } else {
                m_historyIndex = -1;
                setPlainText(m_draft);
            }
            moveCursor(QTextCursor::End);
            event->accept();
            return;
        }
    }

    QPlainTextEdit::keyPressEvent(event);

    // After key press, check if @ completion should be shown
    const int atPos = atSymbolPosition();
    if (atPos >= 0 && m_completer) {
        const int curPos = textCursor().position();
        const QString fullText = toPlainText();
        const QString prefix = fullText.mid(atPos + 1, curPos - (atPos + 1));
        if (m_completer->completionPrefix() != prefix) {
            m_completer->setCompletionPrefix(prefix);
            m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));
        }

        QRect cr = cursorRect();
        cr.setWidth(std::max(200, m_completer->popup()->sizeHintForColumn(0) + 30));
        m_completer->complete(cr);
    } else if (m_completer && m_completer->popup()->isVisible()) {
        m_completer->popup()->hide();
    }
}

} // namespace KateAi
