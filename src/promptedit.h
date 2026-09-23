/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QPlainTextEdit>
#include <QStringList>

class QCompleter;
class QStringListModel;

namespace KateAi
{

class PromptEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit PromptEdit(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    void setCompletionWords(const QStringList &words);
    void addHistory(const QString &text);

public Q_SLOTS:
    void autoGrow();

Q_SIGNALS:
    void submitRequested();
    void escapePressed();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;

private Q_SLOTS:
    void insertCompletion(const QString &completion);

private:
    QString wordUnderCursor() const;
    int atSymbolPosition() const;
    void resetHistoryNavigation();

    QCompleter *m_completer = nullptr;
    QStringListModel *m_completionModel = nullptr;

    QStringList m_history;
    int m_historyIndex = -1;
    QString m_draft;
    bool m_navigatingHistory = false;
};

} // namespace KateAi
