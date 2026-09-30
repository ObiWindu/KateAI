/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QWidget>
#include <QHash>
#include <QString>
#include <QList>

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

    // Apply or revert all pending edits
    void acceptAll();
    void rejectAll();

    // Apply or revert a single edit
    void acceptEdit(const QString &path);
    void rejectEdit(const QString &path);

Q_SIGNALS:
    void editAccepted(const QString &path, const QString &toolName, const QString &newContent);
    void editRejected(const QString &path, const QString &toolName, const QString &oldContent);
    void editsChanged(bool hasEdits);

private:
    void updateUI();
    void rebuildEditList();
    void setExpanded(bool expanded);

    QHash<QString, EditEntry> m_edits; // key = path
    bool m_expanded = false;

    // UI elements - compact bar
    QWidget *m_barWidget = nullptr;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_countLabel = nullptr;
    QPushButton *m_acceptAllBtn = nullptr;
    QPushButton *m_rejectAllBtn = nullptr;
    QPushButton *m_expandBtn = nullptr;

    // UI elements - expanded list
    QWidget *m_listContainer = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QVBoxLayout *m_editListLayout = nullptr;
};

} // namespace KateAi