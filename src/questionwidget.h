/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;

namespace KateAi
{

/**
 * The card the `question` tool puts in front of the user.
 *
 * A question is the one tool whose result is the user's own words, so the card
 * is a prompt rather than a log entry: the question, the likely answers as
 * one-tap chips, and a line to type anything else in. Until it is answered the
 * agent turn is stopped, which is why ChatWidget keeps the card in the intent
 * dock above the input instead of letting it scroll away into the transcript.
 * Once answered it becomes a record and is retired into the transcript like any
 * other finished card.
 */
class QuestionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit QuestionWidget(const QString &questionCallId, QWidget *parent = nullptr);
    ~QuestionWidget() override;

    QString questionCallId() const
    {
        return m_questionCallId;
    }

    // Fills the card. An empty option list, or allowFreeform == false, decides
    // whether the user must pick a chip or may type instead.
    void setQuestion(const QString &questionText, const QStringList &options, bool allowFreeform);
    // Turns the card into a record of the answer, with nothing left to press.
    void markAnswered(const QString &answer);

    void reflowNow();

Q_SIGNALS:
    void answered(const QString &questionCallId, const QString &answer);

private:
    void applyStyle();
    void updateStatus();

    QString m_questionCallId;
    bool m_answered = false;

    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QTextBrowser *m_question = nullptr;
    QWidget *m_optionsRow = nullptr;
    QLineEdit *m_freeform = nullptr;
    QPushButton *m_submit = nullptr;
};

} // namespace KateAi
