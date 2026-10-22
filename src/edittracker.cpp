/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "edittracker.h"

#include "chattheme.h"

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
// part of the chat's usual type scale.
constexpr qreal kTrackerFontScale = 0.6;

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
               "  padding: 0 14px;"
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
               "  padding: 0 14px;"
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
               "  padding: 0 10px;"
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
               "  padding: 0 10px;"
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
               "  padding: 0 10px;"
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
               "  padding: 0 12px;"
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
        // The ---/+++ file headers are metadata, not content changes.
        if (line.startsWith(u"---"_s) || line.startsWith(u"+++"_s) || line.startsWith(u'@')) {
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

    EditEntry entry;
    entry.path = path;
    entry.toolName = toolName;
    entry.diff = diff;
    entry.oldContent = oldContent;
    entry.newContent = newContent;
    entry.createdFile = createdFile;

    Group *group = nullptr;
    for (auto &candidate : m_groups) {
        if (candidate.path == path) {
            group = &candidate;
            break;
        }
    }
    if (!group) {
        Group fresh;
        fresh.path = path;
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

    // New edits invalidate the record of earlier decisions: they were made
    // against a state that no longer exists.
    m_undoStack.clear();

    updateUI();
    Q_EMIT editsChanged(true);
}

void EditTracker::clear()
{
    m_groups.clear();
    m_undoStack.clear();
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
    // Snapshot the paths first: settle() prunes m_groups as it goes, so
    // iterating the container while emitting would invalidate the iterator.
    const QStringList paths = pendingPaths();
    for (const QString &path : paths) {
        acceptEdit(path);
    }
    clear();
}

void EditTracker::rejectAll()
{
    const QStringList paths = pendingPaths();
    for (const QString &path : paths) {
        rejectEdit(path);
    }
    clear();
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
        if (it->path == decision.path) {
            *it = restored;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        m_groups.append(restored);
    }

    // Undoing an acceptance has to undo the file too. Restoring the pre-edit
    // content for a file the agent created means deleting it again, otherwise
    // "undo" would leave behind exactly the file the user said to undo.
    if (decision.kind == Decision::Accepted) {
        if (decision.createdFile) {
            Q_EMIT fileCreatedThenRejected(decision.path);
        } else if (!decision.previousContent.isNull()) {
            Q_EMIT editRejected(decision.path, decision.toolName, decision.previousContent);
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
    // Delete rather than deleteLater(): a turn that lands several edits at once
    // rebuilt the list once per edit, and the deferred deletions all queued up
    // behind each other, so ghost rows painted over the real ones until the
    // event loop finally ran.
    QLayoutItem *item = nullptr;
    while ((item = m_editListLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            delete item->widget();
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
    // a scrollbar on a list meant to fit.
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
    const QString fileName = u"kateai-%1-%2.diff"_s.arg(base.isEmpty() ? u"diff"_s : base, QString::fromUtf8(digest));
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
    const QByteArray bytes = combinedDiff(group).toUtf8();
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
    addDirectory(QDir::homePath());
    addDirectory(QDir::currentPath());

    for (const QString &directory : directories) {
        if (!QDir().mkpath(directory)) {
            continue;
        }
        const QString candidate = QDir(directory).absoluteFilePath(fileName);
        QFile preview(candidate);
        if (!preview.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            continue;
        }
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
    auto it = std::find_if(m_groups.begin(), m_groups.end(), [&path](const Group &group) {
        return group.path == path;
    });
    if (it == m_groups.end() || it->pending.isEmpty()) {
        return;
    }

    Group &group = *it;

    // Everything still pending for this file is settled together, and the restore
    // target is the state before the FIRST of them. Restoring before the last one
    // instead would leave the file holding the intermediate result of an edit the
    // user just said no to.
    Decision decision;
    decision.kind = accepted ? Decision::Accepted : Decision::Rejected;
    decision.path = path;
    decision.toolName = group.pending.first().toolName;
    decision.pending = group.pending;
    decision.createdFile = group.createdFile;
    decision.previousContent = group.lastKnownContent;

    // Mutate our own state before emitting anything. The slots write files and
    // show messages, and a slot is free to call back into the tracker; settling
    // afterwards would leave it observing a queue that still claims the edit is
    // pending.
    const bool createdFile = group.createdFile;
    const QString toolName = decision.toolName;
    const QString restoreTo = group.lastKnownContent;
    const QString settledTo = group.pending.last().newContent;
    group.pending.clear();
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
        Q_EMIT editAccepted(path, toolName, settledTo);
    } else if (createdFile) {
        Q_EMIT fileCreatedThenRejected(path);
    } else {
        Q_EMIT editRejected(path, toolName, restoreTo);
    }
    Q_EMIT editsChanged(hasPendingEdits());
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
