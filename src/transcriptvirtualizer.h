/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

namespace KateAi
{

/**
 * Windowed rendering for a long transcript.
 *
 * Every entry owns a lightweight row and a height estimate, so the complete
 * conversation remains scrollable. Only entries near the viewport materialize
 * their expensive widgets. An entry's materializer appends through
 * appendToCurrent(), which lets existing card builders keep their insertion
 * code. Pinned entries stay materialized even when they are off screen.
 */
class TranscriptVirtualizer : public QObject
{
    Q_OBJECT

public:
    struct Entry {
        QString key;
        // Additional exact aliases for ensureAliasVisible(). Tool navigation
        // aliases are derived separately from toolKeys.
        QStringList aliases;
        // Tool occurrence keys within this entry. The viewport derives a
        // unique navigation alias from the entry key plus this position, so a
        // live call reusing a provider id cannot collapse historical targets.
        QStringList toolKeys;
        int estimatedHeight = 120;
        bool pinned = false;
        std::function<void()> materialize;
    };

    TranscriptVirtualizer(QScrollArea *scrollArea, QVBoxLayout *layout,
                          int materializeThreshold = 256, QObject *parent = nullptr);
    ~TranscriptVirtualizer() override;

    void setEntries(QList<Entry> entries);
    void clear();
    bool isActive() const { return m_active; }
    bool isMaterializing() const { return m_materializing; }
    int entryCount() const { return m_entries.size(); }
    int materializedEntryCount() const;
    int appendIndex() const;
    void appendToCurrent(QWidget *widget);
    void forgetWidget(QWidget *widget);
    QWidget *lastWidget() const;
    QStringList toolAliases() const;
    QString toolWidgetKey(const QString &alias) const;
    bool containsWidget(const QWidget *widget) const;
    QWidget *ensureAliasVisible(const QString &alias);

Q_SIGNALS:
    void entryMaterialized(const QString &key);
    // Emitted once after a viewport refresh or navigation batch finishes
    // materializing entries, so listeners can coalesce collapse/reflow work.
    void viewportUpdated();
    // Emitted before an entry widget is destroyed. Owners use this to remove
    // their raw-pointer indexes without deleting the widget a second time.
    void entryWillEvict(const QString &key, QWidget *widget);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct EntryState {
        Entry definition;
        QPointer<QWidget> row;
        QVBoxLayout *rowLayout = nullptr;
        QSpacerItem *placeholder = nullptr;
        QList<QPointer<QWidget>> widgets;
        bool materialized = false;
    };

    void scheduleRefresh();
    void refresh();
    void refreshTail();
    void materialize(int index);
    void evict(int index, bool force = false);
    QString toolAlias(int entryIndex, int toolIndex) const;
    int entryOffset(int index) const;
    int firstVisibleEntry(int y) const;
    int lastVisibleEntry(int y) const;
    int trailingStretchIndex() const;
    int estimatedTotalHeight() const;
    int viewportHeight() const;

    QPointer<QScrollArea> m_scrollArea;
    QPointer<QVBoxLayout> m_layout;
    QList<EntryState> m_entries;
    QHash<QString, int> m_toolAliasEntries;
    QHash<QString, QString> m_toolAliasWidgetKeys;
    QTimer *m_refreshTimer = nullptr;
    int m_materializeThreshold = 256;
    int m_currentEntry = -1;
    // The last explicitly requested navigation target. Keep it mounted until
    // another target is chosen so estimate/layout feedback cannot evict the
    // card the user just asked to see.
    int m_requiredEntry = -1;
    bool m_active = false;
    bool m_materializing = false;
    bool m_refreshing = false;
};

} // namespace KateAi
