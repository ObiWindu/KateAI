/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QWidget>
#include <QHash>
#include <QString>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QScrollArea;

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

Q_SIGNALS:
    void editAccepted(const QString &path, const QString &toolName, const QString &newContent);
    void editRejected(const QString &path, const QString &toolName, const QString &oldContent);
    void editsChanged(bool hasEdits);

private:
    void rebuildUI();
    void createEditWidget(const EditEntry &entry);
    void updateGlobalButtons();

    QHash<QString, EditEntry> m_edits; // key = path
    QWidget *m_container = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QScrollArea *m_scrollArea = nullptr;

    // Global buttons
    QWidget *m_globalBar = nullptr;
    QPushButton *m_acceptAllBtn = nullptr;
    QPushButton *m_rejectAllBtn = nullptr;
    QLabel *m_countLabel = nullptr;
};

} // namespace KateAi