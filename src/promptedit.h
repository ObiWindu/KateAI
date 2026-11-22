/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QPlainTextEdit>
#include <QList>
#include <QPointer>
#include <QStringList>

class QCompleter;
class QFrame;
class QLabel;
class QPushButton;
class QStringListModel;
class QVBoxLayout;

namespace KateAi
{

// One slash command advertised by the active agent (or a future built-in
// source). The name always includes the leading slash.
struct PromptCommand {
    QString name;
    QString description;
};

class PromptEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    enum class VoiceVisualState {
        Unavailable,
        Idle,
        Recording,
        Processing,
    };

    explicit PromptEdit(QWidget *parent = nullptr);
    ~PromptEdit() override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    void setCompletionWords(const QStringList &words);
    void addHistory(const QString &text);
    // Slash-command suggestions shown above the cursor when the input starts
    // with "/". An empty list restores the plain placeholder.
    void setSlashCommands(const QList<PromptCommand> &commands);
    // Whether the placeholder should mention "/" even before the agent has
    // advertised any commands. An ACP agent is already selected when the
    // editor starts, but its command list only arrives with the live session,
    // so the hint has to lead the list rather than wait for it.
    void setSlashCommandHintEnabled(bool enabled);
    // Widget the slash-command popup should overlay. A panel (the chat) passes
    // itself so the list can cover the transcript, not only the composer. When
    // unset the popup follows the composer's top-level window.
    void setCommandPopupHost(QWidget *host);

    // Microphone button embedded inside the text input area.
    QPushButton *voiceButton() const
    {
        return m_voiceButton;
    }
    void setVoiceAvailability(bool available, const QString &detail = QString(), const QString &shortcutHint = QString());
    void setVoiceVisualState(VoiceVisualState state, const QString &toolTip = QString());
    void applyTheme();

public Q_SLOTS:
    void autoGrow();

Q_SIGNALS:
    void submitRequested();
    void escapePressed();
    void voiceRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void insertCompletion(const QString &completion);
    void updateSlashCommandPopup();

private:
    QString wordUnderCursor() const;
    int atSymbolPosition() const;
    // Heights of the composer, in lines of the current font. Kept as one pair
    // of constants so the empty height and the growth cap move together.
    int minHeight() const;
    int maxHeight() const;
    void positionVoiceButton();
    void updateVoiceButtonAppearance();

    void rebuildSlashCommandRows();
    void selectSlashCommand(int index);
    void applySlashCommand(int index);
    void hideSlashCommandPopup();
    void ensureCommandPopupParent();
    void positionSlashCommandPopup();
    void updateCommandPlaceholder();
    QString slashQuery() const;

    QCompleter *m_completer = nullptr;
    QStringListModel *m_completionModel = nullptr;
    QPushButton *m_voiceButton = nullptr;
    VoiceVisualState m_voiceState = VoiceVisualState::Unavailable;
    bool m_voiceAvailable = false;
    QString m_voiceDetail;
    QString m_voiceShortcutHint;
    QString m_voiceToolTip;

    QFrame *m_commandPopup = nullptr;
    QVBoxLayout *m_commandLayout = nullptr;
    // Overlay host chosen by the panel, if any; null means "the window".
    QPointer<QWidget> m_commandPopupHost;
    QList<QLabel *> m_commandRows;
    QList<PromptCommand> m_slashCommands;
    QList<PromptCommand> m_filteredCommands;
    int m_slashIndex = -1;
    bool m_commandPopupShown = false;
    bool m_slashHintEnabled = false;
    QString m_lastCommandQuery;

    QStringList m_history;
    int m_historyIndex = -1;
    QString m_draft;
};

} // namespace KateAi
