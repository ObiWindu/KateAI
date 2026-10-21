/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QTextBrowser;

namespace KateAi
{

// A transcript card carrying one suggestion from the agent, with Accept and
// Dismiss buttons. Suggestions are advice rather than changes: the agent posts
// one instead of editing something the user never asked it to touch, and the
// user decides whether to act on it. The card is posted while its tool runs --
// the advice itself is already in hand by then -- so it carries a live state
// until the call reports back, unlike a record replayed from history.
class SuggestionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SuggestionWidget(const QString &suggestionId, QWidget *parent = nullptr);
    ~SuggestionWidget() override;

    QString suggestionId() const
    {
        return m_suggestionId;
    }

    // Fills the card. An empty path means the suggestion is not anchored to a
    // file, so Accept only records the decision.
    void setSuggestion(const QString &text, const QString &path, int line);
    // Marks the card as in flight: the tool carrying the suggestion has not
    // reported back yet, so the advice is on screen but still live. Either
    // setSuggestion or setError settles it again.
    void setRunning();
    // Called when the suggest tool failed, so the card can report why instead
    // of showing a suggestion that never arrived.
    void setError(const QString &message);
    // Rebuilt from an earlier turn: the decision buttons are not offered again,
    // because acting on it would jump to code the turn has since moved past.
    void markDecided();

    void reflowNow();
    // Re-applies every theme-derived stylesheet, so a theme change repaints a
    // card that is live in the transcript.
    void applyStyle();

Q_SIGNALS:
    void accepted(const QString &suggestionId, const QString &path, int line);
    void dismissed(const QString &suggestionId);

private:
    void updateHeader();

    QString m_suggestionId;
    QString m_path;
    int m_line = 0;
    bool m_failed = false;
    bool m_decided = false;
    // True while the tool that carries the suggestion is still running: the
    // advice is already on screen, but it has not been confirmed.
    bool m_running = false;

    QLabel *m_badge = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_anchor = nullptr;
    QLabel *m_status = nullptr;
    QTextBrowser *m_body = nullptr;
    QPushButton *m_accept = nullptr;
    QPushButton *m_dismiss = nullptr;
};

} // namespace KateAi
