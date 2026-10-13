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
// user decides whether to act on it. The card therefore has no live state to
// drive -- unlike SubtaskWidget it is created already posted.
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
    // Called when the suggest tool failed, so the card can report why instead
    // of showing a suggestion that never arrived.
    void setError(const QString &message);
    // Rebuilt from an earlier turn: the decision buttons are not offered again,
    // because acting on it would jump to code the turn has since moved past.
    void markDecided();

    void reflowNow();

Q_SIGNALS:
    void accepted(const QString &suggestionId, const QString &path, int line);
    void dismissed(const QString &suggestionId);

private:
    void applyStyle();
    void updateHeader();

    QString m_suggestionId;
    QString m_path;
    int m_line = 0;
    bool m_failed = false;
    bool m_decided = false;

    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_anchor = nullptr;
    QLabel *m_status = nullptr;
    QTextBrowser *m_body = nullptr;
    QPushButton *m_accept = nullptr;
    QPushButton *m_dismiss = nullptr;
};

} // namespace KateAi
