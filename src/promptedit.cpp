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
#include <QHelpEvent>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>
#include <QToolTip>
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

constexpr int kVoiceBtnSize = 26;
constexpr int kVoiceBtnMarginRight = 6;
constexpr int kVoiceBtnMarginBottom = 6;

// Renders a crisp HiDPI microphone icon tinted to the current theme colour so
// the button stays legible across dark, light, and editor-inherited themes.
QIcon makeMicrophoneIcon(const QColor &color, bool slashed, bool stopSquare)
{
    constexpr int logical = 16;
    constexpr int scale = 2;
    QPixmap pm(logical * scale, logical * scale);
    pm.setDevicePixelRatio(scale);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (stopSquare) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(4.0, 4.0, 8.0, 8.0), 1.8, 1.8);
        return QIcon(pm);
    }

    // Microphone capsule
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(5.75, 1.75, 4.5, 7.5), 2.25, 2.25);

    // U-shaped cradle and stand
    QPen pen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(3.75, 3.75, 8.5, 7.25), 180 * 16, 180 * 16);
    p.drawLine(QPointF(8.0, 11.0), QPointF(8.0, 13.6));
    p.drawLine(QPointF(5.5, 13.6), QPointF(10.5, 13.6));

    if (slashed) {
        QPen slashPen(color, 1.5, Qt::SolidLine, Qt::RoundCap);
        p.setPen(slashPen);
        p.drawLine(QPointF(2.5, 13.5), QPointF(13.5, 2.5));
    }

    return QIcon(pm);
}

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

    // Reserve space on the right inside the input box so typed text wraps cleanly
    // before the embedded voice button.
    setViewportMargins(0, 0, kVoiceBtnSize + kVoiceBtnMarginRight + 4, 0);

    m_voiceButton = new QPushButton(this);
    m_voiceButton->setObjectName(u"voicePromptButton"_s);
    m_voiceButton->setFixedSize(kVoiceBtnSize, kVoiceBtnSize);
    m_voiceButton->setIconSize(QSize(15, 15));
    m_voiceButton->setFocusPolicy(Qt::NoFocus);
    m_voiceButton->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    m_voiceButton->setAccessibleName(i18n("Voice Prompt"));
    connect(m_voiceButton, &QPushButton::clicked, this, &PromptEdit::voiceRequested);

    if (viewport()) {
        viewport()->installEventFilter(this);
    }
    if (verticalScrollBar()) {
        connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) {
            positionVoiceButton();
        });
    }

    m_completionModel = new QStringListModel(this);
    m_completer = new QCompleter(m_completionModel, this);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);

    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated),
            this, &PromptEdit::insertCompletion);

    connect(this, &QPlainTextEdit::textChanged, this, &PromptEdit::autoGrow);

    // Slash-command suggestions live in their own popup because the command
    // needs two lines (name and description) and must sit above the cursor,
    // which QCompleter's popup cannot do.
    //
    // The popup is an ordinary child widget, not a Qt::Tool window. Wayland
    // compositors place toplevel windows themselves, so a native tool window
    // ignored positionSlashCommandPopup() and showed up in the middle of the
    // screen; mapping it could also bounce keyboard focus for a frame, which
    // fired focusOutEvent() and hid it at once. A child widget is positioned in
    // host coordinates, sits right above the caret, and never touches the focus
    // chain. The host is the top-level window by default; the chat panel
    // overrides it with setCommandPopupHost() so the list covers the transcript
    // instead of being drawn behind it.
    m_commandPopup = new QFrame(window());
    m_commandPopup->setObjectName(u"promptCommandPopup"_s);
    m_commandPopup->setFocusPolicy(Qt::NoFocus);
    m_commandPopup->setStyleSheet(
        QStringLiteral("QFrame#promptCommandPopup { background-color: %1; border: 1px solid %2;"
                       " border-radius: 8px; }")
            .arg(ChatTheme::surfaceBg(), ChatTheme::border()));
    m_commandLayout = new QVBoxLayout(m_commandPopup);
    m_commandLayout->setContentsMargins(5, 5, 5, 5);
    m_commandLayout->setSpacing(2);
    m_commandPopup->hide();

    connect(this, &QPlainTextEdit::textChanged, this, &PromptEdit::updateSlashCommandPopup);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this,
            &PromptEdit::updateSlashCommandPopup);

    setVoiceAvailability(false, i18n("No microphone is available or usable."));
    applyTheme();
    autoGrow();
    positionVoiceButton();
}

void PromptEdit::applyTheme()
{
    if (m_completer && m_completer->popup()) {
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
    updateVoiceButtonAppearance();
}

void PromptEdit::setVoiceAvailability(bool available, const QString &detail, const QString &shortcutHint)
{
    m_voiceAvailable = available;
    m_voiceDetail = detail;
    if (!shortcutHint.isEmpty()) {
        m_voiceShortcutHint = shortcutHint;
    }

    if (!available) {
        m_voiceState = VoiceVisualState::Unavailable;
        m_voiceToolTip = detail.trimmed().isEmpty()
            ? i18n("No microphone is available or usable.")
            : detail.trimmed();
    } else if (m_voiceState == VoiceVisualState::Unavailable) {
        m_voiceState = VoiceVisualState::Idle;
        m_voiceToolTip.clear();
    }
    updateVoiceButtonAppearance();
}

void PromptEdit::setVoiceVisualState(VoiceVisualState state, const QString &toolTip)
{
    m_voiceState = (!m_voiceAvailable && state == VoiceVisualState::Idle)
        ? VoiceVisualState::Unavailable
        : state;
    if (!toolTip.isEmpty() || state == VoiceVisualState::Idle) {
        m_voiceToolTip = toolTip;
    }
    updateVoiceButtonAppearance();
}

void PromptEdit::updateVoiceButtonAppearance()
{
    if (!m_voiceButton) {
        return;
    }

    const bool enabled = m_voiceAvailable
        && (m_voiceState != VoiceVisualState::Unavailable);
    m_voiceButton->setEnabled(enabled);
    m_voiceButton->setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);

    QString tip = m_voiceToolTip;
    if (!enabled) {
        if (tip.isEmpty()) {
            tip = m_voiceDetail.trimmed().isEmpty()
                ? i18n("No microphone is available or usable.")
                : m_voiceDetail.trimmed();
        }
    } else if (tip.isEmpty()) {
        const QString shortcut = m_voiceShortcutHint.isEmpty() ? u"Ctrl+Alt+V"_s : m_voiceShortcutHint;
        if (!m_voiceDetail.trimmed().isEmpty()) {
            tip = i18n("Voice prompt (%1) — %2", shortcut, m_voiceDetail.trimmed());
        } else {
            tip = i18n("Voice prompt (%1) — click to start speaking", shortcut);
        }
    }
    m_voiceButton->setToolTip(tip);
    m_voiceButton->setAccessibleDescription(tip);

    const QString radius = u"border-radius: 13px;"_s;
    switch (m_voiceState) {
    case VoiceVisualState::Recording: {
        const QColor fg(ChatTheme::textOn(ChatTheme::danger()));
        m_voiceButton->setIcon(makeMicrophoneIcon(fg, false, true));
        m_voiceButton->setStyleSheet(
            QStringLiteral(
                "QPushButton { background-color: %1; border: none; %3 }"
                "QPushButton:hover { background-color: %2; }")
                .arg(ChatTheme::danger(), ChatTheme::tinted(ChatTheme::danger(), 0.85), radius));
        break;
    }
    case VoiceVisualState::Processing: {
        const QColor fg(ChatTheme::textOn(ChatTheme::accent()));
        m_voiceButton->setIcon(makeMicrophoneIcon(fg, false, false));
        m_voiceButton->setStyleSheet(
            QStringLiteral(
                "QPushButton { background-color: %1; border: none; %3 }"
                "QPushButton:hover { background-color: %2; }")
                .arg(ChatTheme::accent(), ChatTheme::accentHover(), radius));
        break;
    }
    case VoiceVisualState::Idle: {
        const QColor fg(ChatTheme::textMuted());
        m_voiceButton->setIcon(makeMicrophoneIcon(fg, false, false));
        m_voiceButton->setStyleSheet(
            QStringLiteral(
                "QPushButton { background-color: transparent; border: 1px solid transparent; %3 }"
                "QPushButton:hover { background-color: %1; border-color: %2; }"
                "QPushButton:pressed { background-color: %2; }")
                .arg(ChatTheme::hoverBg(), ChatTheme::border(), radius));
        break;
    }
    case VoiceVisualState::Unavailable:
    default: {
        const QColor fg(ChatTheme::mixColors(ChatTheme::textMuted(), ChatTheme::surfaceBg(), 45));
        m_voiceButton->setIcon(makeMicrophoneIcon(fg, true, false));
        m_voiceButton->setStyleSheet(
            QStringLiteral(
                "QPushButton { background-color: transparent; border: 1px solid %1; %2 }"
                "QPushButton:disabled { background-color: transparent; border: 1px solid %1; %2 }")
                .arg(ChatTheme::tinted(ChatTheme::border(), 0.55), radius));
        break;
    }
    }
}

void PromptEdit::positionVoiceButton()
{
    if (!m_voiceButton) {
        return;
    }
    const int sbWidth = (verticalScrollBar() && verticalScrollBar()->isVisible())
        ? verticalScrollBar()->width()
        : 0;
    const int x = std::max(2, width() - sbWidth - m_voiceButton->width() - kVoiceBtnMarginRight);
    const int y = std::max(2, height() - m_voiceButton->height() - kVoiceBtnMarginBottom);
    m_voiceButton->move(x, y);
    m_voiceButton->raise();
}

PromptEdit::~PromptEdit()
{
    // The popup lives in the hosting window rather than in the composer, so
    // the window would otherwise outlive this editor holding a hidden frame.
    delete m_commandPopup;
    m_commandPopup = nullptr;
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
    // button. An ACP agent may already be selected before its command list is
    // known (the list arrives with the live session), so the hint can also be
    // enabled independently through setSlashCommandHintEnabled().
    updateCommandPlaceholder();
    // The list changed, so the popup has to be rebuilt even when the query is
    // still the same. hide() also clears the "already shown for this query"
    // shortcut that the other state leaves behind.
    hideSlashCommandPopup();
    // An empty list falls back to the hint row when "/" is already on screen.
    if (!m_slashCommands.isEmpty() || m_slashHintEnabled) {
        updateSlashCommandPopup();
    }
}

void PromptEdit::setSlashCommandHintEnabled(bool enabled)
{
    if (m_slashHintEnabled == enabled) {
        return;
    }
    m_slashHintEnabled = enabled;
    updateCommandPlaceholder();
    if (enabled) {
        // The hint may fill in for a list that is still empty; if "/" is
        // already on screen, show it now instead of waiting for the next key.
        updateSlashCommandPopup();
    } else if (m_slashCommands.isEmpty()) {
        hideSlashCommandPopup();
    }
}

void PromptEdit::updateCommandPlaceholder()
{
    setPlaceholderText(m_slashCommands.isEmpty() && !m_slashHintEnabled
                           ? i18n("Ask anything, @ to mention…")
                           : i18n("Ask anything, @ to mention, / for commands…"));
}

void PromptEdit::updateSlashCommandPopup()
{
    // Before the agent's live session advertises its list (or for an agent
    // that advertises none), the "/" gesture still has to explain itself.
    const bool hintOnly = m_slashCommands.isEmpty();
    if (hintOnly && !m_slashHintEnabled) {
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

    // The composer may have been reparented since the popup was created (the
    // chat panel is built before it is docked into Kate's window), so keep the
    // overlay a child of whatever window currently hosts the composer.
    ensureCommandPopupParent();

    const QString query = slashQuery();
    if (query.contains(u' ') || query.contains(u'\t')) {
        hideSlashCommandPopup();
        return;
    }
    if (m_commandPopupShown && query == m_lastCommandQuery) {
        positionSlashCommandPopup();
        if (!m_commandPopup->isVisible()) {
            m_commandPopup->show();
            m_commandPopup->raise();
        }
        return;
    }
    m_lastCommandQuery = query;

    m_filteredCommands.clear();
    if (!hintOnly) {
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
    }

    rebuildSlashCommandRows();
    selectSlashCommand(hintOnly ? -1 : 0);
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
    // Every row is a direct child of the popup, including the hint row that is
    // deliberately absent from m_commandRows (it is not selectable). Removing
    // the layout items alone would leave that one on screen.
    const QList<QLabel *> rows = m_commandPopup->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly);
    for (QLabel *row : rows) {
        delete row;
    }
    m_commandRows.clear();
    while (QLayoutItem *item = m_commandLayout->takeAt(0)) {
        delete item;
    }

    constexpr int maxRows = 6;
    const int count = qMin(maxRows, m_filteredCommands.size());
    if (count == 0 && m_slashCommands.isEmpty() && m_slashHintEnabled) {
        // Nothing advertised yet. Say so rather than mapping an empty frame:
        // the user pressed "/" expecting an answer, and a blank popup looks
        // like the gesture is broken.
        auto *row = new QLabel(m_commandPopup);
        row->setObjectName(u"promptCommandHintRow"_s);
        row->setTextFormat(Qt::RichText);
        row->setWordWrap(true);
        row->setMargin(0);
        row->setContentsMargins(9, 6, 9, 6);
        row->setText(QStringLiteral("<span style=\"color:%1; font-size:11px;\">%2</span>")
                         .arg(ChatTheme::textMuted(),
                              i18n("The agent advertises its commands when a session starts.").toHtmlEscaped()));
        m_commandLayout->addWidget(row);
        // Same reason as the command rows below: the layout only measures
        // visible children.
        row->show();
        return;
    }
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
        // A newly created child of an already visible popup stays hidden until
        // something shows it. The layout's size hint skips hidden rows, so the
        // popup would be measured as an empty frame (a 12px strip at the bottom
        // of the panel) every time filtering replaced the list. Showing the row
        // here makes the measurement below see the real height; the initial
        // show() used to do this implicitly, which is why only the filtered
        // list collapsed.
        row->show();
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

void PromptEdit::ensureCommandPopupParent()
{
    if (!m_commandPopup) {
        return;
    }
    // A panel host keeps the overlay inside the panel so it paints above the
    // panel's own children (the transcript) rather than becoming a sibling of
    // the docked panel in the main window.
    QWidget *host = m_commandPopupHost ? m_commandPopupHost.data() : window();
    // A standalone composer (tests, previews) is its own window; there the
    // popup simply stays a child of the editor.
    if (!host || host == this) {
        return;
    }
    if (m_commandPopup->parentWidget() != host) {
        m_commandPopup->setParent(host);
        m_commandPopup->hide();
    }
}

void PromptEdit::setCommandPopupHost(QWidget *host)
{
    if (m_commandPopupHost == host) {
        return;
    }
    m_commandPopupHost = host;
    if (m_commandPopup) {
        // The list was positioned for the old host; start over on the new one.
        hideSlashCommandPopup();
        if (host && host != this && m_commandPopup->parentWidget() != host) {
            m_commandPopup->setParent(host);
        }
    }
}

void PromptEdit::positionSlashCommandPopup()
{
    if (!m_commandPopup) {
        return;
    }
    ensureCommandPopupParent();
    QWidget *host = m_commandPopup->parentWidget();
    if (!host) {
        return;
    }

    const int width = qBound(280, viewport()->width() - 8, 520);
    m_commandPopup->setFixedWidth(width);
    m_commandPopup->adjustSize();
    const int maxHeight = 300;
    const int height = qMin(maxHeight, m_commandPopup->sizeHint().height());
    m_commandPopup->resize(width, height);

    // Map the caret into the host so the list lands right above the line being
    // typed on, wherever the composer sits inside that host. The popup is a
    // child widget, so these coordinates really are honoured (unlike a Wayland
    // toplevel, whose position the compositor decides).
    const QRect caret = cursorRect();
    const QPoint anchor = viewport()->mapTo(host, QPoint(caret.left(), caret.top()));
    const int x = qBound(0, anchor.x(), qMax(0, host->width() - width));
    int y = anchor.y() - height - 8;
    if (y < 0) {
        // Not enough room above (a tall composer at the top of the panel):
        // fall back below the caret rather than drawing off-screen.
        y = anchor.y() + caret.height() + 8;
    }
    y = qBound(0, y, qMax(0, host->height() - height));
    m_commandPopup->move(x, y);
    // The host may have gained children since the popup was shown (a new
    // transcript card, the jump-to-latest button); keep the overlay on top.
    m_commandPopup->raise();
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
    positionVoiceButton();
}

QSize PromptEdit::sizeHint() const
{
    return {QPlainTextEdit::sizeHint().width(), minHeight()};
}

QSize PromptEdit::minimumSizeHint() const
{
    return {QPlainTextEdit::minimumSizeHint().width(), minHeight()};
}

void PromptEdit::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    positionVoiceButton();
    // Growing the composer (a new line, a wrapped prompt) moves the caret, so
    // the open suggestion list has to follow it.
    if (m_commandPopupShown) {
        positionSlashCommandPopup();
    }
}

bool PromptEdit::event(QEvent *event)
{
    // Disabled child buttons do not receive QEvent::ToolTip directly in Qt;
    // the event bubbles to the parent widget. Show the disabled mic button's
    // informational message when hovering over its bounds.
    if (event->type() == QEvent::ToolTip && m_voiceButton && m_voiceButton->isVisible()) {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        if (m_voiceButton->geometry().contains(helpEvent->pos())) {
            const QString tip = m_voiceButton->toolTip();
            if (!tip.isEmpty()) {
                QToolTip::showText(helpEvent->globalPos(), tip, this, m_voiceButton->geometry());
                event->accept();
                return true;
            }
        }
    }
    return QPlainTextEdit::event(event);
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
    if (watched == viewport() && event->type() == QEvent::ToolTip && m_voiceButton && m_voiceButton->isVisible()) {
        auto *helpEvent = static_cast<QHelpEvent *>(event);
        const QPoint localPos = viewport()->mapTo(this, helpEvent->pos());
        if (m_voiceButton->geometry().contains(localPos)) {
            const QString tip = m_voiceButton->toolTip();
            if (!tip.isEmpty()) {
                QToolTip::showText(helpEvent->globalPos(), tip, this, m_voiceButton->geometry());
                event->accept();
                return true;
            }
        }
    }
    return QPlainTextEdit::eventFilter(watched, event);
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
    // The popup is a plain child widget and never takes focus, so a focus-out
    // means the user moved to another widget and the suggestions should go
    // away.
    hideSlashCommandPopup();
    QPlainTextEdit::focusOutEvent(event);
}

void PromptEdit::hideEvent(QHideEvent *event)
{
    // The popup is a child of the window, not of the composer, so hiding the
    // composer (panel closed, mode switched) would leave it floating. It must
    // follow the composer out.
    hideSlashCommandPopup();
    QPlainTextEdit::hideEvent(event);
}

void PromptEdit::moveEvent(QMoveEvent *event)
{
    QPlainTextEdit::moveEvent(event);
    // The composer itself moves when banners above it come and go; the popup
    // is positioned in window coordinates, so it would otherwise stay put.
    if (m_commandPopupShown) {
        positionSlashCommandPopup();
    }
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
        case Qt::Key_Down:
            // The hint row is not selectable. Leave the arrow keys to history
            // navigation when there is no command list to move through.
            if (!m_commandRows.isEmpty()) {
                selectSlashCommand(m_slashIndex + (event->key() == Qt::Key_Up ? -1 : 1));
                event->accept();
                return;
            }
            break;
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
