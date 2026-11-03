/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "edittracker.h"

#include "chattheme.h"
#include "editranges.h"

#include <KLocalizedString>

#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QStandardPaths>
#include <QWidget>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// The tracker is deliberately compact: it is a review control, not the main
// transcript. Keep every size in one place so the bar and the rows stay at
// part of the chat's usual type scale. The scale was raised by 15% so the
// small type stays legible next to the transcript.
constexpr qreal kTrackerFontScale = 0.69;

QString trackerFontSize(int pixels)
{
    return QString::number(pixels * kTrackerFontScale) + u"px"_s;
}

QString keepAllStyle()
{
    return QStringLiteral(
               "QPushButton {"
               "  background-color: %1;"
               "  color: %2;"
               "  border: none;"
               "  border-radius: 16px;"
               "  padding: 0 4px;"
               "  font-weight: 600;"
               "  font-size: %7;"
               "}"
               "QPushButton:hover { background-color: %3; }"
               "QPushButton:pressed { background-color: %4; }"
               "QPushButton:disabled { background-color: %5; color: %6; }")
        .arg(ChatTheme::success(), ChatTheme::textOn(ChatTheme::success()), ChatTheme::accent(), ChatTheme::danger(),
             ChatTheme::tinted(ChatTheme::success(), 0.15), ChatTheme::textMuted(), trackerFontSize(12));
}

QString rejectAllStyle()
{
    return QStringLiteral(
               "QPushButton {"
               "  background-color: transparent;"
               "  color: %1;"
               "  border: 1px solid %1;"
               "  border-radius: 16px;"
               "  padding: 0 4px;"
               "  font-weight: 600;"
               "  font-size: %5;"
               "}"
               "QPushButton:hover { background-color: %2; border-color: %1; color: %6; }"
               "QPushButton:pressed { background-color: %1; }"
               "QPushButton:disabled { color: %3; border-color: %4; }")
        .arg(ChatTheme::danger(), ChatTheme::tinted(ChatTheme::danger(), 0.15), ChatTheme::textMuted(),
             ChatTheme::border(), trackerFontSize(12), ChatTheme::textOn(ChatTheme::danger()));
}

QString keepStyle()
{
    return QStringLiteral(
               "QPushButton {"
               "  background-color: transparent;"
               "  color: %1;"
               "  border: 1px solid %1;"
               "  border-radius: 12px;"
               "  padding: 0 4px;"
               "  font-weight: 600;"
               "  font-size: %4;"
               "}"
               "QPushButton:hover { background-color: %1; color: %2; }"
               "QPushButton:pressed { background-color: %3; }")
        .arg(ChatTheme::success(), ChatTheme::textOn(ChatTheme::success()), ChatTheme::danger(), trackerFontSize(11));
}

QString rejectStyle()
{
    return QStringLiteral(
               "QPushButton {"
               "  background-color: transparent;"
               "  color: %1;"
               "  border: 1px solid %1;"
               "  border-radius: 12px;"
               "  padding: 0 4px;"
               "  font-weight: 600;"
               "  font-size: %2;"
               "}"
               "QPushButton:hover { background-color: %1; color: %3; }"
               "QPushButton:pressed { background-color: %1; }")
        .arg(ChatTheme::danger(), trackerFontSize(11), ChatTheme::textOn(ChatTheme::danger()));
}

QString reviewStyle()
{
    return QStringLiteral(
               "QPushButton {"
               "  background-color: %1;"
               "  color: %2;"
               "  border: 1px solid %3;"
               "  border-radius: 12px;"
               "  padding: 0 4px;"
               "  font-weight: 600;"
               "  font-size: %6;"
               "}"
               "QPushButton:hover { background-color: %4; color: %2; border-color: %5; }"
               "QPushButton:pressed { background-color: %1; }")
        .arg(ChatTheme::hoverBg(), ChatTheme::textPrimary(), ChatTheme::border(), ChatTheme::surfaceBg(),
             ChatTheme::borderStrong(), trackerFontSize(11));
}

QString undoStyle()
{
    return QStringLiteral(
               "QPushButton {"
               "  background-color: transparent;"
               "  color: %1;"
               "  border: 1px solid %2;"
               "  border-radius: 16px;"
               "  padding: 0 4px;"
               "  font-weight: 600;"
               "  font-size: %4;"
               "}"
               "QPushButton:hover { background-color: %3; color: %1; }"
               "QPushButton:disabled { color: %5; border-color: %2; }")
        .arg(ChatTheme::textMuted(), ChatTheme::border(), ChatTheme::hoverBg(), trackerFontSize(12),
             ChatTheme::textMuted());
}

int countDiffLines(const QString &diff, QChar marker)
{
    int count = 0;
    const QStringList lines = diff.split(u'\n');
    for (const QString &line : lines) {
        // Only true hunk headers and file headers are metadata. "@decorator"
        // is a content line, and "++++foo"/"----foo" are content lines, so
        // only "@@" hunks and "--- "/"+++ " (or bare "---"/"+++") are skipped.
        if (line.startsWith(u"@@")) {
            continue;
        }
        if (line == u"---"_s || line.startsWith(u"--- "_s) || line == u"+++"_s || line.startsWith(u"+++ "_s)) {
            continue;
        }
        if (line.startsWith(marker)) {
            ++count;
        }
    }
    return count;
}

} // namespace

EditTracker::EditTracker(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"EditTracker"_s);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_barWidget = new QWidget(this);
    m_barWidget->setObjectName(u"EditTrackerBar"_s);
    m_barWidget->setFixedHeight(44);
    auto *barLayout = new QHBoxLayout(m_barWidget);
    barLayout->setContentsMargins(12, 6, 12, 6);
    barLayout->setSpacing(8);

    m_countLabel = new QLabel(m_barWidget);
    barLayout->addWidget(m_countLabel);
    barLayout->addStretch();

    m_acceptAllBtn = new QPushButton(i18n("Keep All"), m_barWidget);
    m_acceptAllBtn->setCursor(Qt::PointingHandCursor);
    m_acceptAllBtn->setFixedHeight(30);
    m_acceptAllBtn->setToolTip(i18n("Accept every pending change"));
    connect(m_acceptAllBtn, &QPushButton::clicked, this, &EditTracker::acceptAll);
    barLayout->addWidget(m_acceptAllBtn);

    m_rejectAllBtn = new QPushButton(i18n("Reject All"), m_barWidget);
    m_rejectAllBtn->setCursor(Qt::PointingHandCursor);
    m_rejectAllBtn->setFixedHeight(30);
    m_rejectAllBtn->setToolTip(i18n("Revert every pending change to its pre-edit content"));
    connect(m_rejectAllBtn, &QPushButton::clicked, this, &EditTracker::rejectAll);
    barLayout->addWidget(m_rejectAllBtn);

    m_toggleListBtn = new QPushButton(i18n("Show files"), m_barWidget);
    m_toggleListBtn->setCursor(Qt::PointingHandCursor);
    m_toggleListBtn->setFixedHeight(30);
    m_toggleListBtn->setToolTip(i18n("Show or hide the pending file list"));
    connect(m_toggleListBtn, &QPushButton::clicked, this, [this]() {
        setFileListVisible(!m_fileListVisible);
    });
    barLayout->addWidget(m_toggleListBtn);

    root->addWidget(m_barWidget);

    m_listContainer = new QWidget(this);
    m_listContainer->setObjectName(u"EditTrackerList"_s);
    auto *listLayout = new QVBoxLayout(m_listContainer);
    listLayout->setContentsMargins(10, 8, 10, 10);
    listLayout->setSpacing(0);

    m_scrollArea = new QScrollArea(m_listContainer);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *scrollContent = new QWidget(m_scrollArea);
    scrollContent->setObjectName(u"EditTrackerScrollContent"_s);
    m_scrollArea->setWidget(scrollContent);
    listLayout->addWidget(m_scrollArea);

    m_editListLayout = new QVBoxLayout(scrollContent);
    m_editListLayout->setContentsMargins(0, 0, 2, 0);
    m_editListLayout->setSpacing(6);
    m_editListLayout->setAlignment(Qt::AlignTop);

    root->addWidget(m_listContainer);

    // Ctrl+Z undoes the last keep/reject. Scoped to the tracker rather than the
    // window so it cannot shadow the editor's own undo while the panel has focus.
    auto *undoShortcut = new QShortcut(QKeySequence(QKeySequence::Undo), this);
    undoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undoShortcut, &QShortcut::activated, this, &EditTracker::undoLast);

    applyStyle();
    m_barWidget->hide();
    m_listContainer->hide();
}

void EditTracker::applyStyle()
{
    m_countLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: %2; font-weight: 600; }").arg(ChatTheme::textPrimary(), trackerFontSize(12)));
    m_acceptAllBtn->setStyleSheet(keepAllStyle());
    m_rejectAllBtn->setStyleSheet(rejectAllStyle());
    m_toggleListBtn->setStyleSheet(undoStyle());
    m_barWidget->setStyleSheet(QStringLiteral(
                                    "#EditTrackerBar { background-color: %1; border-top: 1px solid %2; }")
                                    .arg(ChatTheme::panelBg(), ChatTheme::border()));
    m_scrollArea->setStyleSheet(ChatTheme::scrollArea());
    m_listContainer->setStyleSheet(QStringLiteral("#EditTrackerList { background-color: %1; }").arg(ChatTheme::panelBg()));
    setStyleSheet(QStringLiteral(
                      "#EditTracker { background-color: %1; border-top: 1px solid %2; }")
                      .arg(ChatTheme::panelBg(), ChatTheme::border()));
    // Rows bake their colours in at creation, so repaint them too.
    rebuildEditList();
}

int EditTracker::addedLines(const QString &diff)
{
    return countDiffLines(diff, u'+');
}

int EditTracker::removedLines(const QString &diff)
{
    return countDiffLines(diff, u'-');
}

void EditTracker::addEdit(const QString &path, const QString &toolName, const QString &diff,
                          const QString &oldContent, const QString &newContent, bool createdFile)
{
    if (path.isEmpty()) {
        return;
    }
    const QString normPath = QDir::cleanPath(path);

    EditEntry entry;
    entry.path = normPath;
    entry.toolName = toolName;
    entry.diff = diff;
    entry.oldContent = oldContent;
    entry.newContent = newContent;
    entry.createdFile = createdFile;

    Group *group = nullptr;
    for (auto &candidate : m_groups) {
        // Normalised compare: "a/../b" and "b" are the same file and must
        // share one group.
        if (QDir::cleanPath(candidate.path) == normPath) {
            group = &candidate;
            break;
        }
    }
    if (!group) {
        Group fresh;
        fresh.path = normPath;
        // The baseline a rejection restores: the file as it looked before the
        // first pending edit, which for a brand-new file is nothing at all.
        fresh.lastKnownContent = oldContent;
        fresh.createdFile = createdFile;
        m_groups.append(fresh);
        group = &m_groups.last();
    }

    // createdFile is sticky. At toolStarted the flag is derived from whether the
    // file could be read, so a second edit to a file the agent created itself
    // arrives with createdFile == false: the file exists by then. Folding that in
    // with && would downgrade the group, and rejecting it would restore the
    // group's baseline -- a null QString for a created file, since it was never
    // read -- leaving a zero-byte file behind instead of deleting it. A file that
    // already existed reports false for every entry, so || keeps it false.
    group->createdFile = group->createdFile || createdFile;
    group->pending.append(entry);
    refreshTotals(*group);

    // A new edit invalidates the state earlier decisions were made against,
    // so undo must not reach back past it.
    m_undoStack.clear();

    updateUI();
    Q_EMIT editsChanged(true);
}

void EditTracker::updateEdit(const QString &path, const QString &toolName, const QString &diff,
                             const QString &oldContent, const QString &newContent)
{
    if (path.isEmpty()) {
        return;
    }
    const QString normPath = QDir::cleanPath(path);
    Group *group = nullptr;
    for (auto &candidate : m_groups) {
        if (QDir::cleanPath(candidate.path) == normPath) {
            group = &candidate;
            break;
        }
    }
    if (!group) {
        // Nothing tracked yet for this path: track it from scratch. A null
        // oldContent means "unknown", not "empty file": only an explicit
        // create flag marks a new file, so pass false here.
        addEdit(normPath, toolName, diff, oldContent, newContent, false);
        return;
    }
    // Null (absent) vs empty (present-but-empty) matters: an empty string is a
    // real value for content fields, so only a null update is skipped.
    if (!diff.isNull()) {
        // Replace the diff of the last pending entry, leaving the baseline
        // untouched. A diff that arrives after the snapshot still belongs to
        // the edit that was already queued.
        // Find the entry by path: entries are per-file, so the last entry of
        // this group is the one.
        if (!group->pending.isEmpty()) {
            group->pending.last().diff = diff;
            group->pending.last().toolName = toolName;
            if (!oldContent.isNull()) {
                group->pending.last().oldContent = oldContent;
            }
            if (!newContent.isNull()) {
                group->pending.last().newContent = newContent;
            }
        }
    } else {
        // No new diff: only the tool name may have changed.
        if (!group->pending.isEmpty()) {
            group->pending.last().toolName = toolName;
        }
    }
    updateUI();
    Q_EMIT editsChanged(true);
}

void EditTracker::clear()
{
    m_groups.clear();
    m_undoStack.clear();
    m_settledRanges.clear();
    m_fileListVisible = false;
    updateUI();
    m_barWidget->hide();
    m_listContainer->hide();
    Q_EMIT editsChanged(false);
}

bool EditTracker::hasPendingEdits() const
{
    for (const auto &group : m_groups) {
        if (!group.pending.isEmpty()) {
            return true;
        }
    }
    return false;
}

int EditTracker::pendingEditCount() const
{
    int count = 0;
    for (const auto &group : m_groups) {
        count += group.pending.size();
    }
    return count;
}

QStringList EditTracker::pendingPaths() const
{
    QStringList paths;
    for (const auto &group : m_groups) {
        if (!group.pending.isEmpty()) {
            paths.append(group.path);
        }
    }
    // Arrival order: rows are user-facing and must read in the order the
    // agent made them (QHash guaranteed nothing).
    return paths;
    return paths;
}

QList<EditEntry> EditTracker::pendingEdits() const
{
    QList<EditEntry> result;
    for (const auto &group : m_groups) {
        result.append(group.pending);
    }
    return result;
}

QList<KTextEditor::Range> EditTracker::pendingEditRanges(const QString &path) const
{
    // Per-file, current-version only: the pending entries of one file are
    // successive versions of the same file, so their diffs do not share one
    // coordinate space and must not be concatenated before parsing. Only the
    // latest entry describes the file as it is now.
    const QString normPath = QDir::cleanPath(path);
    for (const auto &group : m_groups) {
        if (QDir::cleanPath(group.path) == normPath) {
            if (group.pending.isEmpty()) {
                return {};
            }
            return addedLineRangesFromDiff(group.pending.last().diff);
        }
    }
    return {};
}

QList<KTextEditor::Range> EditTracker::settledEditRanges(const QString &path) const
{
    return m_settledRanges.value(path);
}

QStringList EditTracker::rangedPaths() const
{
    QStringList paths;
    for (const auto &group : m_groups) {
        if (!group.pending.isEmpty()) {
            paths.append(group.path);
        }
    }
    for (auto it = m_settledRanges.constBegin(); it != m_settledRanges.constEnd(); ++it) {
        if (!paths.contains(it.key())) {
            paths.append(it.key());
        }
    }
    return paths;
}

void EditTracker::refreshTotals(Group &group) const
{
    int added = 0;
    int removed = 0;
    for (const EditEntry &entry : group.pending) {
        added += addedLines(entry.diff);
        removed += removedLines(entry.diff);
    }
    group.added = added;
    group.removed = removed;
}

EditTracker::Group EditTracker::makeGroup(const QString &path) const
{
    Group result;
    result.path = path;
    for (const auto &group : m_groups) {
        if (group.path == path) {
            return group;
        }
    }
    return result;
}

void EditTracker::pruneGroups()
{
    m_groups.erase(std::remove_if(m_groups.begin(), m_groups.end(), [](const Group &group) {
                       return group.pending.isEmpty();
                   }),
                   m_groups.end());
}

void EditTracker::acceptAll()
{
    // Batch: settle every path, then rebuild the list once. Settling one by
    // one rebuilds the whole list per file (O(n²) row churn).
    m_inBatch = true;
    const QStringList paths = pendingPaths();
    for (const QString &path : paths) {
        acceptEdit(path);
    }
    m_inBatch = false;
    // Not clear(): settle() already emptied and pruned every group, and
    // clear() would wipe the undo stack these decisions were pushed on. A
    // bulk decision is the action with the largest blast radius, so it is
    // exactly the one that must stay undoable.
    pruneGroups();
    updateUI();
    Q_EMIT editsChanged(hasPendingEdits());
}

void EditTracker::rejectAll()
{
    m_inBatch = true;
    const QStringList paths = pendingPaths();
    for (const QString &path : paths) {
        rejectEdit(path);
    }
    m_inBatch = false;
    pruneGroups();
    updateUI();
    Q_EMIT editsChanged(hasPendingEdits());
}

bool EditTracker::canUndo() const
{
    return !m_undoStack.isEmpty();
}

void EditTracker::undoLast()
{
    if (m_undoStack.isEmpty()) {
        return;
    }
    const Decision decision = m_undoStack.takeLast();
    if (decision.path.isEmpty()) {
        return;
    }

    // Undoing retracts the settled hint ranges of that decision round, so the
    // lines go back to showing as pending (through the restored group) instead
    // of both.
    if (!decision.settledRanges.isEmpty()) {
        QList<KTextEditor::Range> &ranges = m_settledRanges[decision.path];
        for (const KTextEditor::Range &range : decision.settledRanges) {
            ranges.removeAll(range);
        }
        if (ranges.isEmpty()) {
            m_settledRanges.remove(decision.path);
        }
    }

    // Put the edits back exactly as they were, so the user can decide again
    // rather than having to reconstruct what happened.
    Group restored;
    restored.path = decision.path;
    restored.pending = decision.pending;
    restored.createdFile = decision.createdFile;
    restored.lastKnownContent = decision.previousContent;
    refreshTotals(restored);

    bool replaced = false;
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
        if (QDir::cleanPath(it->path) == QDir::cleanPath(decision.path)) {
            *it = restored;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        // Back where the group stood when it was settled (clamped to the
        // current list): undo must not shuffle the review order by appending
        // a middle file at the end.
        const int at = qBound(0, decision.groupIndex, static_cast<int>(m_groups.size()));
        m_groups.insert(at, restored);
    }

    // Undoing an acceptance has to undo the file too. Restoring the pre-edit
    // content for a file the agent created means deleting it again, otherwise
    // "undo" would leave behind exactly the file the user said to undo.
    if (decision.kind == Decision::Accepted) {
        if (decision.createdFile) {
            Q_EMIT fileCreatedThenRejected(decision.path);
        } else if (!decision.previousContent.isNull()) {
            Q_EMIT editRejected(decision.path, decision.toolName, decision.previousContent);
        } else {
            // Null means the baseline was never read: the file diverged from
            // anything the tracker knows. Warn and ask the caller to refetch
            // the baseline instead of writing back an empty file.
            qWarning("KateAI: undo of %s has no known baseline; requesting refetch.",
                     qUtf8Printable(decision.path));
            Q_EMIT statusMessage(i18n("No known pre-edit content for %1; cannot undo safely",
                                      QFileInfo(decision.path).fileName()),
                                 true);
        }
    } else {
        // Undoing a rejection has to put the edit back: the file on disk holds
        // the reverted content, and the tracker now shows the edit as pending
        // again. Without this the two disagree.
        const EditEntry *last = decision.pending.isEmpty() ? nullptr : &decision.pending.last();
        if (decision.createdFile) {
            // The file was created by the agent and then deleted by the
            // rejection. Putting it back means writing the content again.
            if (last && !last->newContent.isNull()) {
                Q_EMIT editAccepted(decision.path, decision.toolName, last->newContent);
            }
        } else if (last && !last->newContent.isNull()) {
            Q_EMIT editAccepted(decision.path, decision.toolName, last->newContent);
        }
    }

    updateUI();
    Q_EMIT editsChanged(true);
    Q_EMIT statusMessage(i18n("Undid the last decision for %1", QFileInfo(decision.path).fileName()), false);
}

void EditTracker::updateUI()
{
    // Deferred while a bulk settle runs; the caller rebuilds once at the end.
    if (m_inBatch) {
        return;
    }
    const int pendingCount = pendingEditCount();

    if (pendingCount > 0) {
        const int fileCount = pendingPaths().size();
        // Both numbers, because they answer different questions: "how many
        // files do I still have to look at" and "how much did the agent do".
        m_countLabel->setText(i18n("%1 pending in %2", pendingCount, i18np("1 file", "%1 files", fileCount)));
        m_acceptAllBtn->setEnabled(true);
        m_rejectAllBtn->setEnabled(true);
        m_barWidget->show();
        setFileListVisible(m_fileListVisible);
    } else {
        m_countLabel->clear();
        m_acceptAllBtn->setEnabled(false);
        m_rejectAllBtn->setEnabled(false);
        m_barWidget->hide();
        setFileListVisible(false);
    }

    rebuildEditList();
}

void EditTracker::setFileListVisible(bool visible)
{
    m_fileListVisible = visible && hasPendingEdits();
    m_listContainer->setVisible(m_fileListVisible);
    if (m_toggleListBtn) {
        m_toggleListBtn->setText(m_fileListVisible ? i18n("Hide files") : i18n("Show files"));
    }
}

QWidget *EditTracker::createRow(const Group &group)
{
    auto *row = new QWidget(m_scrollArea->widget());
    row->setObjectName(u"EditFileRow"_s);
    row->setStyleSheet(QStringLiteral(
                           "#EditFileRow { background-color: %1; border: 1px solid %2; border-radius: 10px; }")
                           .arg(ChatTheme::surfaceBg(), ChatTheme::border()));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 6, 8, 6);
    layout->setSpacing(8);

    auto *nameLabel = new QLabel(QFileInfo(group.path).fileName(), row);
    nameLabel->setStyleSheet(QStringLiteral(
                                  "QLabel { color: %1; font-size: %2; font-weight: 600; background: transparent; border: none; }")
                                  .arg(ChatTheme::textPrimary(), trackerFontSize(12)));
    // The bare filename is ambiguous the moment a turn touches two files with the
    // same name; the full path is what makes the row actionable.
    nameLabel->setToolTip(group.path);
    nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    nameLabel->setMinimumWidth(40);
    layout->addWidget(nameLabel, 1);

    if (group.pending.size() > 1) {
        auto *countLabel = new QLabel(i18np("1 edit", "%1 edits", group.pending.size()), row);
        countLabel->setStyleSheet(QStringLiteral(
                                      "QLabel { color: %1; font-size: %2; background: transparent; border: none; }")
                                      .arg(ChatTheme::textMuted(), trackerFontSize(11)));
        countLabel->setToolTip(i18np("The agent edited this file once in this turn",
                                     "The agent edited this file %1 times in this turn",
                                     group.pending.size()));
        layout->addWidget(countLabel);
    }

    if (group.createdFile) {
        auto *newLabel = new QLabel(i18n("new"), row);
        newLabel->setStyleSheet(QStringLiteral(
                                   "QLabel { color: %1; font-size: %2; font-weight: 700; background: transparent; border: none; }")
                                   .arg(ChatTheme::accent(), trackerFontSize(10)));
        newLabel->setToolTip(i18n("This file did not exist before the agent created it. Rejecting deletes it."));
        layout->addWidget(newLabel);
    }

    auto *addedLabel = new QLabel(row);
    addedLabel->setStyleSheet(QStringLiteral(
                                  "QLabel { color: %1; font-size: %2; font-weight: 700; font-family: monospace; background: transparent; border: none; }")
                                  .arg(ChatTheme::success(), trackerFontSize(12)));
    addedLabel->setText(group.added > 0 ? QStringLiteral("+%1").arg(group.added) : QString());
    addedLabel->setVisible(group.added > 0);
    layout->addWidget(addedLabel);

    auto *removedLabel = new QLabel(row);
    removedLabel->setStyleSheet(QStringLiteral(
                                    "QLabel { color: %1; font-size: %2; font-weight: 700; font-family: monospace; background: transparent; border: none; }")
                                    .arg(ChatTheme::danger(), trackerFontSize(12)));
    removedLabel->setText(group.removed > 0 ? QStringLiteral("-%1").arg(group.removed) : QString());
    removedLabel->setVisible(group.removed > 0);
    layout->addWidget(removedLabel);

    auto *reviewBtn = new QPushButton(i18n("Review"), row);
    reviewBtn->setCursor(Qt::PointingHandCursor);
    reviewBtn->setFixedHeight(24);
    reviewBtn->setStyleSheet(reviewStyle());
    reviewBtn->setToolTip(i18n("Preview this file's diff in the editor"));
    const QString path = group.path;
    connect(reviewBtn, &QPushButton::clicked, this, [this, path]() {
        previewDiff(path);
    });
    layout->addWidget(reviewBtn);

    auto *keepBtn = new QPushButton(i18n("Keep"), row);
    keepBtn->setCursor(Qt::PointingHandCursor);
    keepBtn->setFixedHeight(24);
    keepBtn->setStyleSheet(keepStyle());
    connect(keepBtn, &QPushButton::clicked, this, [this, path]() {
        acceptEdit(path);
    });
    layout->addWidget(keepBtn);

    auto *rejectBtn = new QPushButton(i18n("Reject"), row);
    rejectBtn->setCursor(Qt::PointingHandCursor);
    rejectBtn->setFixedHeight(24);
    rejectBtn->setStyleSheet(rejectStyle());
    rejectBtn->setToolTip(group.createdFile ? i18n("Delete this file") : i18n("Restore the pre-edit content"));
    connect(rejectBtn, &QPushButton::clicked, this, [this, path]() {
        rejectEdit(path);
    });
    layout->addWidget(rejectBtn);

    return row;
}

void EditTracker::appendRow(const Group &group)
{
    m_editListLayout->addWidget(createRow(group));
}

void EditTracker::rebuildEditList()
{
    // Detach rows with deleteLater(): a synchronous delete inside a signal
    // sender's own slot can free the sender mid-emit.
    QLayoutItem *item = nullptr;
    while ((item = m_editListLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    int rows = 0;
    for (const auto &group : m_groups) {
        if (group.pending.isEmpty()) {
            continue;
        }
        appendRow(group);
        ++rows;
    }

    m_editListLayout->addStretch();

    // Size the viewport from the rows that actually got built rather than from a
    // guessed row height: a wrong constant either clipped the last row or raised
    // a scrollbar on a list meant to fit. Deferred: sizeHint() is only valid
    // after the layout pass, so measure after it, not synchronously here.
    QTimer::singleShot(0, this, [this, rows]() {
        if (!m_scrollArea || !m_editListLayout) {
            return;
        }
        m_scrollArea->widget()->adjustSize();
        const int visibleRows = std::min(rows, 6);
        int contentHeight = 0;
        for (int i = 0; i < visibleRows; ++i) {
            if (auto *layoutItem = m_editListLayout->itemAt(i)) {
                if (QWidget *row = layoutItem->widget()) {
                    contentHeight += row->sizeHint().height();
                }
            }
        }
        contentHeight += std::max(0, visibleRows - 1) * m_editListLayout->spacing();
        m_scrollArea->setFixedHeight(contentHeight > 0 ? contentHeight + 2 : 0);
    });
}

// The whole question a review answers is "what does this file look like now
// versus before the turn", which is one comparison no matter how many edits the
// agent stacked on top of each other.
QString EditTracker::combinedDiff(const Group &group) const
{
    QString combined;
    for (const EditEntry &entry : group.pending) {
        if (!combined.isEmpty()) {
            combined += u'\n';
        }
        combined += entry.diff;
    }
    return combined;
}

QString EditTracker::previewDiffPath(const QString &path) const
{
    // The preview is a file, so it lives outside the workspace: a .diff dropped
    // next to the code would be something the agent's own tools trip over on the
    // next turn. The name carries the file it came from, so several previews open
    // in the editor are each identifiable, and the hash keeps two files that
    // share a base name from overwriting each other's preview.
    const QByteArray digest = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    const QString base = QFileInfo(path).completeBaseName();
    const QString fileName = u"kateai-%1-%2.diff"_s.arg(base.isEmpty() ? u"diff"_s : base,
                                                          QString::fromUtf8(digest));
    const QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return QDir(temp.isEmpty() ? QDir::tempPath() : temp).absoluteFilePath(fileName);
}

void EditTracker::previewDiff(const QString &path)
{
    const Group group = makeGroup(path);
    if (group.pending.isEmpty()) {
        return;
    }

    // Written as a .diff so the editor colours the added and removed lines with
    // its own diff highlighting: the same reading experience as a diff anywhere
    // else in the IDE, instead of a second window with its own idea of what a
    // diff looks like. The path is stable per reviewed file, so previewing again
    // after another edit lands in the same tab rather than piling up.
    //
    // TempLocation is not always writable (a read-only bind of /tmp, or a TMPDIR
    // the process cannot create files in). The review still has to open, so the
    // same file name is tried in each candidate until one accepts the write.
    const QString previewPath = previewDiffPath(path);
    const QString fileName = QFileInfo(previewPath).fileName();
    const QString diff = combinedDiff(group);
    if (diff.trimmed().isEmpty()) {
        // Nothing to show: opening an empty preview tab answers no review
        // question and strands the user in a blank editor.
        Q_EMIT statusMessage(i18n("No diff to preview for %1", QFileInfo(path).fileName()), true);
        return;
    }
    const QByteArray bytes = diff.toUtf8();
    QStringList directories;
    const auto addDirectory = [&directories](const QString &directory) {
        if (!directory.isEmpty() && !directories.contains(directory)) {
            directories.append(directory);
        }
    };
    addDirectory(QFileInfo(previewPath).absolutePath());
    addDirectory(QDir::tempPath());
    // tempPath() follows TMPDIR even when that directory cannot take a file.
    // /tmp and /dev/shm are separate from it, and from a home directory that
    // is itself mounted read-only.
    addDirectory(QStringLiteral("/tmp"));
    addDirectory(QStringLiteral("/dev/shm"));
    addDirectory(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    addDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    addDirectory(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation));
    addDirectory(QDir::homePath() + u"/.cache/kateai"_s);
    // Never $HOME root or cwd: a preview containing diff secrets must not land
    // in the workspace where agent tools would trip over it.

    // Clean stale previews from earlier runs (same naming scheme, older than
    // a day) so they do not pile up. Every candidate dir is swept, not just
    // the first: previews written to a fallback dir would otherwise never be
    // reaped.
    {
        const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
        for (const QString &directory : directories) {
            const QDir staleDir(directory);
            const QFileInfoList stale = staleDir.entryInfoList({u"kateai-*.diff"_s}, QDir::Files);
            for (const QFileInfo &fi : stale) {
                if (fi.lastModified() < cutoff && !fi.isSymLink()) {
                    QFile::remove(fi.absoluteFilePath());
                }
            }
        }
    }

    for (const QString &directory : directories) {
        if (!QDir().mkpath(directory)) {
            continue;
        }
        const QString candidate = QDir(directory).absoluteFilePath(fileName);
        // Predictable name in a shared temp dir: planted symlinks are refused
        // outright rather than followed or overwritten. Note this is a
        // check-then-act sequence, not an atomic O_NOFOLLOW open: a hostile
        // writer with access to the same temp dir could swap the path between
        // the check and the create. NewOnly narrows the window (creation
        // fails instead of following an existing file), and owner-only perms
        // apply at create time, but a fully adversarial TMPDIR is out of
        // scope here.
        QFileInfo existing(candidate);
        if (existing.isSymLink()) {
            QFile::remove(candidate);
            if (QFileInfo(candidate).isSymLink()) {
                continue;
            }
        }
        const bool isNew = !QFile::exists(candidate);
        if (!isNew) {
            // A regular file from an earlier preview is replaced wholesale so
            // the write below never follows stale state.
            QFile::remove(candidate);
            if (QFileInfo(candidate).isSymLink() || QFile::exists(candidate)) {
                continue;
            }
        }
        QFile preview(candidate);
        QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text | QIODevice::NewOnly;
        if (!preview.open(mode)) {
            continue;
        }
        // Secrets in diffs: owner-only regardless of umask.
        preview.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const qint64 written = preview.write(bytes);
        preview.close();
        if (written != bytes.size()) {
            QFile::remove(candidate);
            continue;
        }
        Q_EMIT previewDiffRequested(candidate);
        return;
    }

    Q_EMIT statusMessage(i18n("Could not write the diff preview for %1", QFileInfo(path).fileName()), true);
}

void EditTracker::settle(const QString &path, bool accepted)
{
    // Keyed by clean path throughout: "a/./b" and "a/b" are one file and must
    // settle the same group and the same settled-ranges slot.
    const QString normPath = QDir::cleanPath(path);
    const int groupIndex = static_cast<int>(std::distance(
        m_groups.cbegin(),
        std::find_if(m_groups.cbegin(), m_groups.cend(), [&normPath](const Group &group) {
            return QDir::cleanPath(group.path) == normPath;
        })));
    if (groupIndex >= m_groups.size()) {
        return;
    }
    auto it = m_groups.begin() + groupIndex;
    if (it->pending.isEmpty()) {
        return;
    }

    Group &group = *it;

    // The settled ranges are recorded before the pending list is cleared:
    // the hover hint keeps pointing at the lines this round changed, with the
    // "turn finished" wording, until the queue itself is cleared.
    const QList<KTextEditor::Range> settledNow = addedLineRangesFromDiff(combinedDiff(group));

    // Everything still pending for this file is settled together, and the restore
    // target is the state before the FIRST of them. Restoring before the last one
    // instead would leave the file holding the intermediate result of an edit the
    // user just said no to.
    Decision decision;
    decision.kind = accepted ? Decision::Accepted : Decision::Rejected;
    decision.path = normPath;
    decision.groupIndex = groupIndex;
    // Join every contributing tool, not just the first: a group can stack
    // edits from several tools and the record must name them all.
    {
        QStringList tools;
        for (const EditEntry &entry : group.pending) {
            if (!entry.toolName.isEmpty() && !tools.contains(entry.toolName)) {
                tools.append(entry.toolName);
            }
        }
        decision.toolName = tools.join(u", "_s);
    }
    decision.pending = group.pending;
    decision.createdFile = group.createdFile;
    decision.previousContent = group.lastKnownContent;
    decision.settledRanges = settledNow;

    // Mutate our own state before emitting anything. The slots write files and
    // show messages, and a slot is free to call back into the tracker; settling
    // afterwards would leave it observing a queue that still claims the edit is
    // pending.
    const bool createdFile = group.createdFile;
    const QString toolName = decision.toolName;
    const QString restoreTo = group.lastKnownContent;
    const QString settledTo = group.pending.last().newContent;
    group.pending.clear();
    if (!settledNow.isEmpty()) {
        m_settledRanges[normPath].append(settledNow);
    }
    if (accepted && !settledTo.isNull()) {
        // The file is already in this state, so accepting only settles the queue.
        // A null content means the post-edit read failed, so the state of the
        // file is unknown: the previous baseline is kept, because a null
        // baseline would be restored as an empty file later.
        group.lastKnownContent = settledTo;
    }
    m_undoStack.append(decision);
    pruneGroups();
    updateUI();

    if (accepted) {
        Q_EMIT editAccepted(normPath, toolName, settledTo);
    } else if (createdFile) {
        Q_EMIT fileCreatedThenRejected(normPath);
    } else {
        Q_EMIT editRejected(normPath, toolName, restoreTo);
    }
    // Bulk settles (accept/reject all) emit once at the end; a per-file
    // notification here would fire N times for N files.
    if (!m_inBatch) {
        Q_EMIT editsChanged(hasPendingEdits());
    }
}

void EditTracker::renamePath(const QString &oldPath, const QString &newPath)
{
    // The tracker keys everything by path, so the entries have
    // to follow the file or a later accept/reject would settle the old,
    // possibly nonexistent path. Compared by clean path throughout.
    if (oldPath.isEmpty() || newPath.isEmpty() || QDir::cleanPath(oldPath) == QDir::cleanPath(newPath)) {
        return;
    }
    const QString cleanOld = QDir::cleanPath(oldPath);
    const QString cleanNew = QDir::cleanPath(newPath);
    bool changed = false;
    for (auto &group : m_groups) {
        if (QDir::cleanPath(group.path) == cleanOld) {
            group.path = newPath;
            for (auto &entry : group.pending) {
                entry.path = newPath;
            }
            changed = true;
        }
    }
    for (auto &decision : m_undoStack) {
        if (QDir::cleanPath(decision.path) == cleanOld) {
            decision.path = newPath;
            for (auto &entry : decision.pending) {
                entry.path = newPath;
            }
            changed = true;
        }
    }
    if (m_settledRanges.contains(cleanOld)) {
        m_settledRanges.insert(cleanNew, m_settledRanges.take(cleanOld));
        changed = true;
    } else if (m_settledRanges.contains(oldPath)) {
        // Legacy slot filed under the raw path before clean-path keying.
        m_settledRanges.insert(cleanNew, m_settledRanges.take(oldPath));
        changed = true;
    }
    if (changed) {
        updateUI();
    }
}

void EditTracker::acceptEdit(const QString &path)
{
    settle(path, true);
}

void EditTracker::rejectEdit(const QString &path)
{
    settle(path, false);
}

} // namespace KateAi
