/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

class QDialog;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QWidget;

namespace KateAi
{

struct EditEntry
{
    QString path;
    QString toolName; // "write_file" or "edit_file"
    QString diff;     // Unified diff showing the changes
    QString oldContent;
    QString newContent;
    bool accepted = false;
    bool rejected = false;
};

class EditTracker : public QWidget
{
    Q_OBJECT

public:
    explicit EditTracker(QWidget *parent = nullptr);

    void addEdit(const QString &path, const QString &toolName, const QString &diff,
                 const QString &oldContent = QString(), const QString &newContent = QString());
    void clear();
    bool hasPendingEdits() const;
    QList<EditEntry> pendingEdits() const;

    void acceptAll();
    void rejectAll();

    void acceptEdit(const QString &path);
    void rejectEdit(const QString &path);

Q_SIGNALS:
    void editAccepted(const QString &path, const QString &toolName, const QString &newContent);
    void editRejected(const QString &path, const QString &toolName, const QString &oldContent);
    void editsChanged(bool hasEdits);

private:
    void updateUI();
    void rebuildEditList();
    void openReview(const QString &path);
    void closeReview(const QString &path);
    void closeAllReviews();
    void finishEdit(const QString &path);

    QHash<QString, EditEntry> m_edits; // key = path
    QHash<QString, QPointer<QDialog>> m_reviewDialogs;
    QHash<QString, QString> m_reviewFiles;

    QWidget *m_barWidget = nullptr;
    QLabel *m_countLabel = nullptr;
    QPushButton *m_acceptAllBtn = nullptr;
    QPushButton *m_rejectAllBtn = nullptr;

    QWidget *m_listContainer = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QVBoxLayout *m_editListLayout = nullptr;
};

} // namespace KateAi
