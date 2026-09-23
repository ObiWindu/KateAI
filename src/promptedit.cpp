/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "promptedit.h"

#include <KLocalizedString>
#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>

using namespace Qt::Literals::StringLiterals;

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
    m_completer->setMaxVisibleItems(8);

    if (m_completer->popup()) {
        m_completer->popup()->setStyleSheet(
            u"QListView {"
            u"  background-color: #232326;"
            u"  color: #e4e4e4;"
            u"  border: 1px solid #38383e;"
            u"  border-radius: 6px;"
            u"  padding: 4px;"
            u"  font-size: 12px;"
            u"}"
            u"QListView::item {"
            u"  padding: 4px 8px;"
            u"  border-radius: 4px;"
            u"}"
            u"QListView::item:selected {"
            u"  background-color: #007acc;"
            u"  color: #ffffff;"
            u"}"_s);
    }

    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated),
            this, &PromptEdit::insertCompletion);

    connect(this, &QPlainTextEdit::textChanged, this, &PromptEdit::autoGrow);
    connect(this, &QPlainTextEdit::textChanged, this, [this]() {
        if (!m_navigatingHistory && m_historyIndex >= 0) {
            // Programmatic changes (for example a quick-start prompt) and
            // normal edits both leave history-navigation mode.
            m_historyIndex = -1;
            m_draft.clear();
        }
    });
    autoGrow();
}

void PromptEdit::setCompletionWords(const QStringList &words)
{
    if (!m_completionModel) {
        return;
    }

    QStringList normalized;
    normalized.reserve(words.size());
    for (QString word : words) {
        word = word.trimmed();
        if (word.startsWith(u'@')) {
            word.remove(0, 1);
        }
        if (!word.isEmpty() && !normalized.contains(word)) {
            normalized.append(word);
        }
    }
    m_completionModel->setStringList(normalized);
}

void PromptEdit::addHistory(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    // Keep history useful and bounded. Reusing a prompt moves it to the most
    // recent position instead of allowing repeated entries to grow forever.
    m_history.removeAll(trimmed);
    m_history.append(trimmed);
    constexpr qsizetype kMaxHistoryEntries = 100;
    if (m_history.size() > kMaxHistoryEntries) {
        m_history.remove(0, m_history.size() - kMaxHistoryEntries);
    }

    m_historyIndex = -1;
    m_draft.clear();
}

void PromptEdit::resetHistoryNavigation()
{
    if (m_historyIndex < 0) {
        return;
    }
    // Once the user edits a recalled prompt, it becomes a normal draft again.
    m_draft = toPlainText();
    m_historyIndex = -1;
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

    // The mention token must begin the current whitespace-delimited word.
    int tokenStart = curPos - 1;
    while (tokenStart >= 0 && !fullText.at(tokenStart).isSpace()) {
        --tokenStart;
    }
    ++tokenStart;
    if (tokenStart < curPos && fullText.at(tokenStart) == u'@') {
        return tokenStart;
    }
    return -1;
}

void PromptEdit::insertCompletion(const QString &completion)
{
    if (completion.isEmpty()) {
        return;
    }
    const int atPos = atSymbolPosition();
    if (atPos < 0) {
        return;
    }

    QTextCursor tc = textCursor();
    tc.setPosition(atPos);
    tc.setPosition(textCursor().position(), QTextCursor::KeepAnchor);
    tc.insertText(u"@"_s + completion);
    tc.insertText(u" "_s);
    setTextCursor(tc);
}

void PromptEdit::keyPressEvent(QKeyEvent *event)
{
    // Completer popup handling
    if (m_completer && m_completer->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab: {
            const QString completion = m_completer->currentCompletion();
            if (!completion.isEmpty()) {
                insertCompletion(completion);
                m_completer->popup()->hide();
                event->accept();
                return;
            }
            m_completer->popup()->hide();
            // A Return with no completion falls through to normal submit
            // behavior; Tab simply dismisses an empty popup.
            if (event->key() == Qt::Key_Tab) {
                event->accept();
                return;
            }
            break;
        }
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

    // History navigation with Up/Down arrow keys. Only navigate when the
    // caret is at the logical boundary so multi-line prompts remain editable.
    if (event->key() == Qt::Key_Up && !(event->modifiers() & Qt::ShiftModifier)) {
        const QTextCursor tc = textCursor();
        const bool isEmpty = toPlainText().trimmed().isEmpty();
        if ((tc.atStart() || isEmpty) && !m_history.isEmpty()) {
            if (m_historyIndex == -1) {
                m_draft = toPlainText();
                m_historyIndex = m_history.size() - 1;
            } else if (m_historyIndex > 0) {
                --m_historyIndex;
            }
            if (m_historyIndex >= 0 && m_historyIndex < m_history.size()) {
                m_navigatingHistory = true;
                setPlainText(m_history.at(m_historyIndex));
                moveCursor(QTextCursor::End);
                m_navigatingHistory = false;
            }
            event->accept();
            return;
        }
    } else if (event->key() == Qt::Key_Down && !(event->modifiers() & Qt::ShiftModifier)) {
        const QTextCursor tc = textCursor();
        const bool isEmpty = toPlainText().trimmed().isEmpty();
        if (m_historyIndex >= 0 && (tc.atEnd() || isEmpty)) {
            ++m_historyIndex;
            m_navigatingHistory = true;
            if (m_historyIndex < m_history.size()) {
                setPlainText(m_history.at(m_historyIndex));
            } else {
                m_historyIndex = -1;
                setPlainText(m_draft);
            }
            moveCursor(QTextCursor::End);
            m_navigatingHistory = false;
            event->accept();
            return;
        }
    }

    // Typing, deleting, or pasting after a history recall starts a new draft.
    if (m_historyIndex >= 0 && !m_navigatingHistory) {
        const bool editsText = !event->text().isEmpty() || event->key() == Qt::Key_Backspace
            || event->key() == Qt::Key_Delete || event->key() == Qt::Key_V || event->key() == Qt::Key_X;
        if (editsText) {
            resetHistoryNavigation();
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
