/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "transcriptvirtualizer.h"

#include <QEvent>
#include <QScrollBar>
#include <QSpacerItem>
#include <QTimer>

#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

TranscriptVirtualizer::TranscriptVirtualizer(QScrollArea *scrollArea, QVBoxLayout *layout,
                                             int materializeThreshold, QObject *parent)
    : QObject(parent)
    , m_scrollArea(scrollArea)
    , m_layout(layout)
    , m_materializeThreshold(qMax(1, materializeThreshold))
{
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(0);
    connect(m_refreshTimer, &QTimer::timeout, this, &TranscriptVirtualizer::refresh);
    if (m_scrollArea) {
        if (auto *scrollBar = m_scrollArea->verticalScrollBar()) {
            connect(scrollBar, &QScrollBar::valueChanged, this, &TranscriptVirtualizer::scheduleRefresh);
            connect(scrollBar, &QScrollBar::rangeChanged, this, &TranscriptVirtualizer::scheduleRefresh);
        }
        if (auto *viewport = m_scrollArea->viewport()) {
            viewport->installEventFilter(this);
        }
    }
    if (m_layout && trailingStretchIndex() < 0) {
        m_layout->addStretch();
    }
}

TranscriptVirtualizer::~TranscriptVirtualizer()
{
    clear();
}

void TranscriptVirtualizer::setEntries(QList<Entry> entries)
{
    clear();
    m_active = entries.size() > m_materializeThreshold;
    if (!m_active) {
        m_entries.clear();
        return;
    }

    const int firstIndex = appendIndex();
    m_entries.reserve(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        EntryState state;
        state.definition = std::move(entries[i]);
        state.definition.estimatedHeight = qMax(1, state.definition.estimatedHeight);
        state.row = new QWidget(m_layout->parentWidget());
        state.row->setObjectName(u"transcriptVirtualRow"_s);
        state.row->setProperty("transcriptEntryKey", state.definition.key);
        state.row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        state.rowLayout = new QVBoxLayout(state.row);
        state.rowLayout->setContentsMargins(0, 0, 0, 0);
        state.rowLayout->setSpacing(m_layout ? qMax(0, m_layout->spacing()) : 0);
        state.placeholder = new QSpacerItem(0, state.definition.estimatedHeight,
                                            QSizePolicy::Minimum, QSizePolicy::Fixed);
        state.rowLayout->addSpacerItem(state.placeholder);
        m_layout->insertWidget(firstIndex + i, state.row);
        m_entries.append(std::move(state));
    }
    m_toolAliasEntries.clear();
    m_toolAliasWidgetKeys.clear();
    for (int i = 0; i < m_entries.size(); ++i) {
        for (int j = 0; j < m_entries.at(i).definition.toolKeys.size(); ++j) {
            const QString alias = toolAlias(i, j);
            m_toolAliasEntries.insert(alias, i);
            m_toolAliasWidgetKeys.insert(alias, m_entries.at(i).definition.toolKeys.at(j));
        }
    }
    m_layout->activate();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).definition.pinned) {
            materialize(i);
        }
    }
    refreshTail();
}

void TranscriptVirtualizer::clear()
{
    if (m_refreshTimer) {
        m_refreshTimer->stop();
    }
    for (int i = 0; i < m_entries.size(); ++i) {
        evict(i, true);
        delete m_entries.at(i).row.data();
    }
    m_entries.clear();
    m_toolAliasEntries.clear();
    m_toolAliasWidgetKeys.clear();
    m_currentEntry = -1;
    m_requiredEntry = -1;
    m_active = false;
    m_materializing = false;
    if (m_layout) {
        m_layout->invalidate();
    }
}

int TranscriptVirtualizer::materializedEntryCount() const
{
    int count = 0;
    for (const EntryState &entry : m_entries) {
        if (entry.materialized) {
            ++count;
        }
    }
    return count;
}

int TranscriptVirtualizer::appendIndex() const
{
    return trailingStretchIndex();
}

void TranscriptVirtualizer::appendToCurrent(QWidget *widget)
{
    if (!widget || !m_layout) {
        return;
    }
    if (m_currentEntry < 0 || m_currentEntry >= m_entries.size()) {
        m_layout->insertWidget(appendIndex(), widget);
        return;
    }
    EntryState &entry = m_entries[m_currentEntry];
    if (!entry.rowLayout) {
        return;
    }
    if (!entry.widgets.contains(widget)) {
        entry.widgets.append(widget);
    }
    entry.rowLayout->addWidget(widget);
    if (entry.row) {
        entry.row->show();
    }
}

void TranscriptVirtualizer::forgetWidget(QWidget *widget)
{
    for (EntryState &entry : m_entries) {
        entry.widgets.removeAll(QPointer<QWidget>(widget));
    }
}

QWidget *TranscriptVirtualizer::lastWidget() const
{
    if (!m_layout) {
        return nullptr;
    }
    const int end = appendIndex();
    for (int i = end - 1; i >= 0; --i) {
        QLayoutItem *item = m_layout->itemAt(i);
        QWidget *widget = item ? item->widget() : nullptr;
        if (!widget) {
            continue;
        }
        // Virtual rows are direct children of the outer layout. Returning the
        // row (rather than a nested card) keeps retireIntentWidget's
        // outer-layout anchor lookup valid.
        return widget;
    }
    return nullptr;
}

QStringList TranscriptVirtualizer::toolAliases() const
{
    QStringList aliases;
    for (int i = 0; i < m_entries.size(); ++i) {
        for (int j = 0; j < m_entries.at(i).definition.toolKeys.size(); ++j) {
            aliases.append(toolAlias(i, j));
        }
    }
    return aliases;
}

QString TranscriptVirtualizer::toolWidgetKey(const QString &alias) const
{
    return m_toolAliasWidgetKeys.value(alias);
}

bool TranscriptVirtualizer::containsWidget(const QWidget *widget) const
{
    for (const EntryState &entry : m_entries) {
        for (const QPointer<QWidget> &candidate : entry.widgets) {
            if (candidate == widget) {
                return true;
            }
        }
    }
    return false;
}

QWidget *TranscriptVirtualizer::ensureAliasVisible(const QString &alias)
{
    int entryIndex = m_toolAliasEntries.value(alias, -1);
    if (entryIndex < 0) {
        for (int i = 0; i < m_entries.size(); ++i) {
            const EntryState &candidate = m_entries.at(i);
            if (candidate.definition.key == alias || candidate.definition.aliases.contains(alias)) {
                entryIndex = i;
                break;
            }
        }
    }
    if (entryIndex < 0 || entryIndex >= m_entries.size()) {
        return nullptr;
    }
    EntryState &entry = m_entries[entryIndex];
    const bool wasMaterialized = entry.materialized;
    m_requiredEntry = entryIndex;
    materialize(entryIndex);
    if (!wasMaterialized && entry.materialized) {
        Q_EMIT viewportUpdated();
    }
    QPointer<QWidget> target = entry.row;
    QPointer<QWidget> firstWidget;
    const QString toolKey = toolWidgetKey(alias);
    for (const QPointer<QWidget> &widget : entry.widgets) {
        if (widget && !firstWidget) {
            firstWidget = widget;
        }
        if (widget && (widget->objectName() == alias
                       || (!toolKey.isEmpty() && widget->objectName() == u"ToolCallWidget_"_s + toolKey))) {
            target = widget;
            firstWidget = widget;
            break;
        }
    }
    if (target == entry.row && firstWidget) {
        target = firstWidget;
    }
    if (target) {
        if (auto *rowLayout = target->parentWidget() ? target->parentWidget()->layout() : nullptr) {
            rowLayout->activate();
        }
        if (m_layout) {
            m_layout->activate();
        }
    }
    if (m_scrollArea && target) {
        m_scrollArea->ensureWidgetVisible(target, 24, 40);
        // The first ensure can run before QScrollArea has consumed the row's
        // new geometry. Repeat next turn with the updated range, but guard the
        // pointer so a clear/eviction cannot leave a dangling capture.
        const QPointer<QWidget> guarded = target;
        QTimer::singleShot(0, this, [this, guarded] {
            if (m_scrollArea && guarded) {
                m_scrollArea->ensureWidgetVisible(guarded, 24, 40);
            }
        });
    }
    return target;
}

bool TranscriptVirtualizer::eventFilter(QObject *watched, QEvent *event)
{
    if (m_scrollArea && watched == m_scrollArea->viewport()
        && event->type() == QEvent::Resize) {
        scheduleRefresh();
    }
    return QObject::eventFilter(watched, event);
}

void TranscriptVirtualizer::scheduleRefresh()
{
    if (m_active && !m_refreshing && m_refreshTimer && !m_refreshTimer->isActive()) {
        m_refreshTimer->start();
    }
}

void TranscriptVirtualizer::refresh()
{
    if (!m_active || m_refreshing || m_entries.isEmpty() || !m_scrollArea) {
        return;
    }
    m_refreshing = true;
    QScrollBar *scrollBar = m_scrollArea->verticalScrollBar();
    const int value = scrollBar ? scrollBar->value() : 0;
    const int maximum = scrollBar ? scrollBar->maximum() : 0;
    const bool initialTail = maximum <= 0 && value <= 0;
    const int anchor = initialTail ? m_entries.size() - 1 : firstVisibleEntry(value);
    const int offsetInAnchor = initialTail ? 0 : value - entryOffset(anchor);
    const int height = viewportHeight();
    const int overscan = qMax(240, height);
    const int first = firstVisibleEntry(qMax(0, value - overscan));
    int last = lastVisibleEntry(value + height + overscan);
    if (initialTail) {
        last = lastVisibleEntry(estimatedTotalHeight());
    }
    last = qBound(first, last, m_entries.size() - 1);

    // Mount the new window before releasing the old one. That keeps a card
    // under the user's pointer alive until its replacement has real geometry.
    for (int i = first; i <= last; ++i) {
        materialize(i);
    }
    const bool keepRequired = m_requiredEntry >= 0 && m_requiredEntry < m_entries.size();
    if (keepRequired && (m_requiredEntry < first || m_requiredEntry > last)) {
        materialize(m_requiredEntry);
    }
    for (int i = 0; i < m_entries.size(); ++i) {
        if ((i < first || i > last) && (!keepRequired || i != m_requiredEntry)) {
            evict(i);
        }
    }
    if (m_layout) {
        m_layout->activate();
        for (EntryState &entry : m_entries) {
            if (entry.materialized && entry.row) {
                const int measured = entry.row->sizeHint().height();
                if (measured > 0) {
                    entry.definition.estimatedHeight = measured;
                }
            }
        }
        m_layout->invalidate();
    }
    if (scrollBar && !initialTail) {
        // Mounting replaces an estimate with the card's real height. Restore
        // the same logical row and pixel offset so that replacement cannot move
        // the text under the user's viewport.
        scrollBar->setValue(entryOffset(anchor) + offsetInAnchor);
    }
    m_refreshing = false;
    Q_EMIT viewportUpdated();
}

void TranscriptVirtualizer::refreshTail()
{
    if (!m_active || m_entries.isEmpty()) {
        return;
    }
    const int first = firstVisibleEntry(qMax(0, estimatedTotalHeight() - viewportHeight() * 2));
    for (int i = 0; i < m_entries.size(); ++i) {
        if (i >= first) {
            materialize(i);
        }
    }
    Q_EMIT viewportUpdated();
    if (m_scrollArea) {
        QTimer::singleShot(0, this, [this] {
            if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
                m_scrollArea->verticalScrollBar()->setValue(m_scrollArea->verticalScrollBar()->maximum());
            }
        });
    }
}

void TranscriptVirtualizer::materialize(int index)
{
    if (!m_active || index < 0 || index >= m_entries.size()) {
        return;
    }
    EntryState &entry = m_entries[index];
    if (entry.materialized) {
        return;
    }
    if (!entry.row || !entry.rowLayout) {
        return;
    }
    entry.materialized = true;
    if (entry.placeholder) {
        entry.rowLayout->removeItem(entry.placeholder);
        delete entry.placeholder;
        entry.placeholder = nullptr;
    }
    m_currentEntry = index;
    m_materializing = true;
    if (entry.definition.materialize) {
        entry.definition.materialize();
    }
    m_materializing = false;
    m_currentEntry = -1;
    if (entry.widgets.isEmpty()) {
        // A running sub-agent may live in the intent dock instead of the
        // transcript. Its logical row reserves no visible height until the
        // card retires into the thread.
        entry.row->hide();
    } else {
        entry.row->show();
    }
    Q_EMIT entryMaterialized(entry.definition.key);
    if (entry.rowLayout) {
        entry.rowLayout->invalidate();
    }
}

void TranscriptVirtualizer::evict(int index, bool force)
{
    if (index < 0 || index >= m_entries.size()) {
        return;
    }
    EntryState &entry = m_entries[index];
    if (!entry.materialized || (!force && entry.definition.pinned)) {
        return;
    }
    if (entry.row) {
        const int measured = entry.row->sizeHint().height();
        if (measured > 0) {
            entry.definition.estimatedHeight = measured;
        }
    }
    for (const QPointer<QWidget> &widget : entry.widgets) {
        if (!widget) {
            continue;
        }
        Q_EMIT entryWillEvict(entry.definition.key, widget);
        if (entry.rowLayout) {
            entry.rowLayout->removeWidget(widget);
        }
        widget->hide();
        widget->setParent(nullptr);
        widget->deleteLater();
    }
    entry.widgets.clear();
    entry.materialized = false;
    if (entry.row && entry.rowLayout) {
        // Keep the lightweight row visible so its spacer contributes to the
        // scrollbar. Only the expensive child widgets are evicted.
        entry.placeholder = new QSpacerItem(0, qMax(1, entry.definition.estimatedHeight),
                                            QSizePolicy::Minimum, QSizePolicy::Fixed);
        entry.rowLayout->addSpacerItem(entry.placeholder);
        entry.rowLayout->invalidate();
    }
}

QString TranscriptVirtualizer::toolAlias(int entryIndex, int toolIndex) const
{
    return u"tool:%1#%2"_s.arg(m_entries.at(entryIndex).definition.key).arg(toolIndex);
}

int TranscriptVirtualizer::entryOffset(int index) const
{
    const int spacing = m_layout ? qMax(0, m_layout->spacing()) : 0;
    int offset = m_layout ? qMax(0, m_layout->contentsMargins().top()) : 0;
    for (int i = 0; i < qBound(0, index, m_entries.size()); ++i) {
        offset += qMax(1, m_entries.at(i).definition.estimatedHeight) + spacing;
    }
    return offset;
}

int TranscriptVirtualizer::firstVisibleEntry(int y) const
{
    if (m_entries.isEmpty()) {
        return 0;
    }
    const int spacing = m_layout ? qMax(0, m_layout->spacing()) : 0;
    int offset = entryOffset(0);
    for (int i = 0; i < m_entries.size(); ++i) {
        const int height = qMax(1, m_entries.at(i).definition.estimatedHeight);
        if (offset + height >= y) {
            return i;
        }
        offset += height + spacing;
    }
    return m_entries.size() - 1;
}

int TranscriptVirtualizer::lastVisibleEntry(int y) const
{
    if (m_entries.isEmpty()) {
        return 0;
    }
    const int spacing = m_layout ? qMax(0, m_layout->spacing()) : 0;
    int offset = entryOffset(0);
    int result = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (offset > y) {
            break;
        }
        result = i;
        offset += qMax(1, m_entries.at(i).definition.estimatedHeight) + spacing;
    }
    return result;
}

int TranscriptVirtualizer::trailingStretchIndex() const
{
    if (!m_layout) {
        return 0;
    }
    for (int i = m_layout->count() - 1; i >= 0; --i) {
        if (QLayoutItem *item = m_layout->itemAt(i); item && item->spacerItem()) {
            return i;
        }
    }
    return m_layout->count();
}

int TranscriptVirtualizer::estimatedTotalHeight() const
{
    const int spacing = m_layout ? qMax(0, m_layout->spacing()) : 0;
    int total = m_layout ? m_layout->contentsMargins().top() + m_layout->contentsMargins().bottom() : 0;
    for (const EntryState &entry : m_entries) {
        total += qMax(1, entry.definition.estimatedHeight) + spacing;
    }
    return total;
}

int TranscriptVirtualizer::viewportHeight() const
{
    if (!m_scrollArea || !m_scrollArea->viewport()) {
        return 600;
    }
    return qMax(200, m_scrollArea->viewport()->height());
}

} // namespace KateAi
