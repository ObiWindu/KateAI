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
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>
#include <QVBoxLayout>
#include <algorithm>

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

    // Slash-command suggestions live in their own popup because the command
    // needs two lines (name and description) and must sit above the cursor,
    // which QCompleter's popup cannot do.
    m_commandPopup = new QFrame(this, Qt::Tool | Qt::FramelessWindowHint
                                          | Qt::WindowDoesNotAcceptFocus
                                          | Qt::WindowStaysOnTopHint);
    m_commandPopup->setObjectName(u"promptCommandPopup"_s);
    m_commandPopup->setAttribute(Qt::WA_ShowWithoutActivating);
    m_commandPopup->setFocusPolicy(Qt::NoFocus);
    m_commandPopup->setStyleSheet(
        QStringLiteral("QFrame#promptCommandPopup { background-color: %1; border: 1px solid %2;"
                       " border-radius: 8px; }")
            .arg(ChatTheme::surfaceBg(), ChatTheme::border()));
    m_commandLayout = new QVBoxLayout(m_commandPopup);
    m_commandLayout->setContentsMargins(5, 5, 5, 5);
    m_commandLayout->setSpacing(2);
    m_commandPopup->hide();

    connect(this, &QPlainTextEdit::textChanged, this, &PromptEdit::autoGrow);
    connect(this, &QPlainTextEdit::textChanged, this, &PromptEdit::updateSlashCommandPopup);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this,
            &PromptEdit::updateSlashCommandPopup);
    autoGrow();
}

void PromptEdit::setCompletionWords(const QStringList &words)
{
    if (m_completionModel) {
        m_completionModel->setStringList(words);
    }
}

void PromptEdit::setSlashCommands(const QList<PromptCommand> &commands)
{
    m_slashCommands.clear();
    for (PromptCommand command : commands) {
        command.name = command.name.trimmed();
        if (command.name.isEmpty()) {
            continue;
        }
        if (!command.name.startsWith(u'/')) {
            command.name.prepend(u'/');
        }
        command.description = command.description.simplified();
        m_slashCommands.append(command);
    }

    // The placeholder is the in-input hint that replaced the old "cmds"
    // button. It only mentions "/" when an agent actually advertised commands.
    setPlaceholderText(m_slashCommands.isEmpty()
                           ? i18n("Ask anything, @ to mention…")
                           : i18n("Ask anything, @ to mention, / for commands…"));
    if (m_slashCommands.isEmpty()) {
        hideSlashCommandPopup();
    } else {
        m_lastCommandQuery.clear();
        updateSlashCommandPopup();
    }
}

void PromptEdit::updateSlashCommandPopup()
{
    if (m_slashCommands.isEmpty()) {
        hideSlashCommandPopup();
        return;
    }

    const QString fullText = toPlainText();
    const QTextCursor tc = textCursor();
    const int cursorPosition = tc.position();
    // Only the first character of the first line triggers the command menu.
    // Once the user starts typing arguments the popup gets out of the way.
    if (!fullText.startsWith(u'/') || cursorPosition < 1
        || cursorPosition > fullText.size() || tc.hasSelection()
        || tc.blockNumber() != 0) {
        hideSlashCommandPopup();
        return;
    }

    const QString query = slashQuery();
    if (query.contains(u' ') || query.contains(u'\t')) {
        hideSlashCommandPopup();
        return;
    }
    if (m_commandPopupShown && query == m_lastCommandQuery) {
        positionSlashCommandPopup();
        return;
    }
    m_lastCommandQuery = query;

    m_filteredCommands.clear();
    for (const PromptCommand &command : std::as_const(m_slashCommands)) {
        if (query.isEmpty()
            || command.name.contains(query, Qt::CaseInsensitive)
            || command.description.contains(query, Qt::CaseInsensitive)) {
            m_filteredCommands.append(command);
        }
    }
    if (m_filteredCommands.isEmpty()) {
        hideSlashCommandPopup();
        return;
    }

    rebuildSlashCommandRows();
    selectSlashCommand(0);
    positionSlashCommandPopup();
    m_commandPopup->show();
    m_commandPopup->raise();
    m_commandPopupShown = true;
    if (m_completer && m_completer->popup()->isVisible()) {
        m_completer->popup()->hide();
    }
}

QString PromptEdit::slashQuery() const
{
    const QString fullText = toPlainText();
    const int cursorPosition = textCursor().position();
    if (!fullText.startsWith(u'/') || cursorPosition <= 1) {
        return {};
    }
    return fullText.mid(1, cursorPosition - 1);
}

void PromptEdit::rebuildSlashCommandRows()
{
    for (QLabel *row : std::as_const(m_commandRows)) {
        delete row;
    }
    m_commandRows.clear();
    while (QLayoutItem *item = m_commandLayout->takeAt(0)) {
        delete item;
    }

    constexpr int maxRows = 6;
    const int count = qMin(maxRows, m_filteredCommands.size());
    for (int i = 0; i < count; ++i) {
        const PromptCommand &command = m_filteredCommands.at(i);
        auto *row = new QLabel(m_commandPopup);
        row->setObjectName(u"promptCommandRow"_s);
        row->setTextFormat(Qt::RichText);
        row->setWordWrap(true);
        row->setMargin(0);
        row->setContentsMargins(9, 5, 9, 5);
        row->setCursor(Qt::PointingHandCursor);
        QString description = command.description.toHtmlEscaped();
        if (description.isEmpty()) {
            description = i18n("No description").toHtmlEscaped();
        }
        row->setText(QStringLiteral("<span style=\"color:%1; font-family:monospace; font-size:12px;"
                                    " font-weight:600;\">%2</span><br>"
                                    "<span style=\"color:%3; font-size:11px;\">%4</span>")
                         .arg(ChatTheme::accent(), command.name.toHtmlEscaped(),
                              ChatTheme::textMuted(), description));
        row->installEventFilter(this);
        m_commandLayout->addWidget(row);
        m_commandRows.append(row);
    }
}

void PromptEdit::selectSlashCommand(int index)
{
    if (m_commandRows.isEmpty()) {
        m_slashIndex = -1;
        return;
    }
    m_slashIndex = std::clamp(index, 0, static_cast<int>(m_commandRows.size()) - 1);
    const int rowCount = static_cast<int>(m_commandRows.size());
    for (int i = 0; i < rowCount; ++i) {
        const bool selected = i == m_slashIndex;
        m_commandRows.at(i)->setStyleSheet(
            QStringLiteral("QLabel { background-color: %1; border-radius: 5px; }")
                .arg(selected ? ChatTheme::hoverBg() : QStringLiteral("transparent")));
    }
}

void PromptEdit::applySlashCommand(int index)
{
    if (index < 0 || index >= m_filteredCommands.size()) {
        return;
    }
    const QTextCursor current = textCursor();
    QTextCursor replace = current;
    replace.setPosition(0);
    replace.setPosition(current.position(), QTextCursor::KeepAnchor);
    replace.insertText(m_filteredCommands.at(index).name + u' ');
    setTextCursor(replace);
    hideSlashCommandPopup();
    setFocus();
}

void PromptEdit::hideSlashCommandPopup()
{
    if (m_commandPopup) {
        m_commandPopup->hide();
    }
    m_slashIndex = -1;
    m_commandPopupShown = false;
    m_lastCommandQuery.clear();
}

void PromptEdit::positionSlashCommandPopup()
{
    if (!m_commandPopup) {
        return;
    }
    const int width = qBound(280, viewport()->width() - 8, 520);
    m_commandPopup->setFixedWidth(width);
    m_commandPopup->adjustSize();
    const int maxHeight = 300;
    const int height = qMin(maxHeight, m_commandPopup->sizeHint().height());
    m_commandPopup->resize(width, height);

    const QRect caret = cursorRect();
    const QPoint anchor = viewport()->mapToGlobal(QPoint(caret.left(), caret.top()));
    QScreen *screen = QGuiApplication::screenAt(anchor);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect available = screen ? screen->availableGeometry() : QRect(anchor, QSize(800, 600));
    int x = qBound(available.left(), anchor.x(), available.right() - width);
    int y = anchor.y() - height - 8;
    if (y < available.top()) {
        // Not enough room above (a tall composer at the top of the screen):
        // fall back below the caret rather than drawing off-screen.
        y = anchor.y() + caret.height() + 8;
    }
    m_commandPopup->move(x, y);
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

void PromptEdit::focusOutEvent(QFocusEvent *event)
{
    // The command popup does not take focus (WindowDoesNotAcceptFocus), so a
    // focus-out means the user moved to another widget and the suggestions
    // should go away.
    hideSlashCommandPopup();
    QPlainTextEdit::focusOutEvent(event);
}

bool PromptEdit::eventFilter(QObject *watched, QEvent *event)
{
    const int index = m_commandRows.indexOf(qobject_cast<QLabel *>(watched));
    if (index >= 0) {
        if (event->type() == QEvent::MouseButtonRelease) {
            applySlashCommand(index);
            return true;
        }
        if (event->type() == QEvent::Enter) {
            selectSlashCommand(index);
            return false;
        }
    }
    return QPlainTextEdit::eventFilter(watched, event);
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
    if (m_commandPopup && m_commandPopupShown) {
        switch (event->key()) {
        case Qt::Key_Escape:
            hideSlashCommandPopup();
            event->accept();
            return;
        case Qt::Key_Up:
            selectSlashCommand(m_slashIndex - 1);
            event->accept();
            return;
        case Qt::Key_Down:
            selectSlashCommand(m_slashIndex + 1);
            event->accept();
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
            if (m_slashIndex >= 0) {
                applySlashCommand(m_slashIndex);
                event->accept();
                return;
            }
            break;
        default:
            break;
        }
    }

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
