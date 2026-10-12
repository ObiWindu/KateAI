/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "edittracker.h"

#include "chattheme.h"
#include "codehighlight.h"

#include <KLocalizedString>

#include <QClipboard>
#include <QColor>
#include <QDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// The tracker is deliberately compact: it is a review control, not the main
// transcript. Keep every size in one place so the bar, rows and review dialog
// stay at half the chat's usual type scale.
constexpr qreal kTrackerFontScale = 0.5;

QString trackerFontSize(int pixels)
{
    return QString::number(pixels * kTrackerFontScale) + u"px"_s;
}

QString keepAllStyle()
{
    // Derived from the theme's success colour at varying alpha rather than a
    // second hard-coded green, so retheming the panel does not leave this one
    // button behind. The previous literal #16a34a was a third green.
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
        .arg(ChatTheme::success(), QColor(0x08, 0x12, 0x0e).name(), ChatTheme::accent(), ChatTheme::danger(),
             QColor(0x1f, 0x3d, 0x2a).name(), ChatTheme::textMuted(), trackerFontSize(12));
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
               "QPushButton:hover { background-color: %2; border-color: %1; color: #fecaca; }"
               "QPushButton:pressed { background-color: %1; }"
               "QPushButton:disabled { color: %3; border-color: %4; }")
        .arg(ChatTheme::danger(), QColor(0x3f, 0x12, 0x12).name(), ChatTheme::textMuted(), ChatTheme::border(), trackerFontSize(12));
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
        .arg(ChatTheme::success(), QColor(0x08, 0x12, 0x0e).name(), ChatTheme::danger(), trackerFontSize(11));
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
               "QPushButton:hover { background-color: %1; color: #fecaca; }"
               "QPushButton:pressed { background-color: %1; }")
        .arg(ChatTheme::danger(), trackerFontSize(11));
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
               "QPushButton:hover { background-color: %4; color: #ffffff; border-color: %5; }"
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
               "QPushButton:hover { background-color: %3; color: #ffffff; }"
               "QPushButton:disabled { color: #6b7280; border-color: #333333; }")
        .arg(ChatTheme::textMuted(), ChatTheme::border(), ChatTheme::hoverBg(), trackerFontSize(12));
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

QString diffToHtml(const QString &diff)
{
    if (diff.isEmpty()) {
        return QStringLiteral("<body><p>&nbsp;</p></body>");
    }
    // Use Kate's syntax highlighting for diffs to get proper token colours.
    const QString highlighted = CodeHighlight::htmlBodyForCode(diff, u"diff"_s);
    if (!highlighted.isEmpty()) {
        return u"<body>"_s + highlighted + u"</body>"_s;
    }
    // Fallback to the simple line-by-line rendering if highlighting is unavailable.
    QString html = QStringLiteral("<body>");
    const QStringList lines = diff.split(u'\n');
    for (const QString &line : lines) {
        const QString escaped = line.isEmpty() ? QStringLiteral("&nbsp;") : line.toHtmlEscaped();
        if (line.startsWith(u"---"_s) || line.startsWith(u"+++"_s) || line.startsWith(u'@')) {
            html += QStringLiteral("<p class='hunk'>%1</p>").arg(escaped);
        } else if (line.startsWith(u'+')) {
            html += QStringLiteral("<p class='added'>%1</p>").arg(escaped);
        } else if (line.startsWith(u'-')) {
            html += QStringLiteral("<p class='removed'>%1</p>").arg(escaped);
        } else {
            html += QStringLiteral("<p>%1</p>").arg(escaped);
        }
    }
    html += QStringLiteral("</body>");
    return html;
}

QString diffStylesheet()
{
    return QStringLiteral(
               "body { color: %1; font-family: monospace; font-size: %2; margin: 0; padding: 0; }"
               ".removed { color: #fca5a5; background-color: #3f1212; }"
               ".added { color: #86efac; background-color: #14532d; }"
               ".hunk { color: #71717a; }"
               "p { margin: 0; padding: 1px 6px; white-space: pre-wrap; }")
        .arg(ChatTheme::textPrimary(), trackerFontSize(12));
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
    m_countLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: %2; font-weight: 600; }").arg(ChatTheme::textPrimary(), trackerFontSize(12)));
    barLayout->addWidget(m_countLabel);
    barLayout->addStretch();

    m_acceptAllBtn = new QPushButton(i18n("Keep All"), m_barWidget);
    m_acceptAllBtn->setCursor(Qt::PointingHandCursor);
    m_acceptAllBtn->setFixedHeight(30);
    m_acceptAllBtn->setStyleSheet(keepAllStyle());
    m_acceptAllBtn->setToolTip(i18n("Accept every pending change"));
    connect(m_acceptAllBtn, &QPushButton::clicked, this, &EditTracker::acceptAll);
    barLayout->addWidget(m_acceptAllBtn);

    m_rejectAllBtn = new QPushButton(i18n("Reject All"), m_barWidget);
    m_rejectAllBtn->setCursor(Qt::PointingHandCursor);
    m_rejectAllBtn->setFixedHeight(30);
    m_rejectAllBtn->setStyleSheet(rejectAllStyle());
    m_rejectAllBtn->setToolTip(i18n("Revert every pending change to its pre-edit content"));
    connect(m_rejectAllBtn, &QPushButton::clicked, this, &EditTracker::rejectAll);
    barLayout->addWidget(m_rejectAllBtn);

    m_toggleListBtn = new QPushButton(i18n("Show files"), m_barWidget);
    m_toggleListBtn->setCursor(Qt::PointingHandCursor);
    m_toggleListBtn->setFixedHeight(30);
    m_toggleListBtn->setStyleSheet(undoStyle());
    m_toggleListBtn->setToolTip(i18n("Show or hide the pending file list"));
    connect(m_toggleListBtn, &QPushButton::clicked, this, [this]() {
        setFileListVisible(!m_fileListVisible);
    });
    barLayout->addWidget(m_toggleListBtn);

    m_barWidget->setStyleSheet(QStringLiteral(
                                    "#EditTrackerBar { background-color: %1; border-top: 1px solid %2; }")
                                    .arg(ChatTheme::panelBg(), ChatTheme::border()));

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
    m_scrollArea->setStyleSheet(ChatTheme::scrollArea());

    auto *scrollContent = new QWidget(m_scrollArea);
    scrollContent->setObjectName(u"EditTrackerScrollContent"_s);
    m_scrollArea->setWidget(scrollContent);
    listLayout->addWidget(m_scrollArea);

    m_editListLayout = new QVBoxLayout(scrollContent);
    m_editListLayout->setContentsMargins(0, 0, 2, 0);
    m_editListLayout->setSpacing(6);
    m_editListLayout->setAlignment(Qt::AlignTop);

    m_listContainer->setStyleSheet(QStringLiteral("#EditTrackerList { background-color: %1; }").arg(ChatTheme::panelBg()));

    root->addWidget(m_listContainer);

    setStyleSheet(QStringLiteral(
                      "#EditTracker { background-color: %1; border-top: 1px solid %2; }")
                      .arg(ChatTheme::panelBg(), ChatTheme::border()));

    // Ctrl+Z undoes the last keep/reject. Scoped to the tracker rather than the
    // window so it cannot shadow the editor's own undo while the panel has focus.
    auto *undoShortcut = new QShortcut(QKeySequence(QKeySequence::Undo), this);
    undoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undoShortcut, &QShortcut::activated, this, &EditTracker::undoLast);

    m_barWidget->hide();
    m_listContainer->hide();
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
    closeReview(path);

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
    closeAllReviews();
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
    }

    closeReview(decision.path);
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
        m_countLabel->setText(i18n("%1 pending in %2", pendingCount, i18np("1 file", "%n files", fileCount)));
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
        auto *countLabel = new QLabel(i18np("1 edit", "%n edits", group.pending.size()), row);
        countLabel->setStyleSheet(QStringLiteral(
                                      "QLabel { color: %1; font-size: %2; background: transparent; border: none; }")
                                      .arg(ChatTheme::textMuted(), trackerFontSize(11)));
        countLabel->setToolTip(i18np("The agent edited this file once in this turn",
                                     "The agent edited this file %n times in this turn",
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
    const QString path = group.path;
    connect(reviewBtn, &QPushButton::clicked, this, [this, path]() {
        openReview(path);
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

void EditTracker::openReview(const QString &path)
{
    const Group group = makeGroup(path);
    if (group.pending.isEmpty()) {
        return;
    }
    showDiffDialog(path, group);
}

void EditTracker::showDiffDialog(const QString &path, const Group &group)
{
    // One dialog per path. Opening a second one for a file that is already being
    // reviewed used to stack a duplicate window behind the first, each with its
    // own Keep/Reject buttons deciding the same thing.
    if (auto existing = m_reviewDialogs.value(path)) {
        existing->raise();
        existing->activateWindow();
        return;
    }

    auto *dialog = new QDialog(window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(i18n("Review %1", QFileInfo(path).fileName()));
    dialog->setModal(false);
    dialog->resize(760, 560);
    dialog->setStyleSheet(
        QStringLiteral("QDialog { background-color: %1; } QLabel { background: transparent; }").arg(ChatTheme::panelBg()));

    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto *title = new QLabel(QFileInfo(path).fileName(), dialog);
    title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: %2; font-weight: 700; }")
                             .arg(ChatTheme::textPrimary(), trackerFontSize(16)));
    root->addWidget(title);

    auto *subtitle = new QLabel(path, dialog);
    subtitle->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: %2; font-family: monospace; }")
                                .arg(ChatTheme::textMuted(), trackerFontSize(11)));
    subtitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);

    // One combined diff for the whole file rather than a tab per edit: the
    // question the review answers is "what does this file look like now versus
    // before the turn", which is a single comparison.
    QString combined;
    for (const EditEntry &entry : group.pending) {
        if (!combined.isEmpty()) {
            combined += u'\n';
        }
        combined += entry.diff;
    }

    auto *diffBrowser = new QTextBrowser(dialog);
    diffBrowser->setReadOnly(true);
    diffBrowser->setOpenExternalLinks(false);
    diffBrowser->setFrameShape(QFrame::NoFrame);
    diffBrowser->setStyleSheet(QStringLiteral(
                                   "QTextBrowser { background-color: %1; color: %2; border: 1px solid %3; border-radius: 10px; padding: 8px; font-family: monospace; font-size: %4; }")
                                   .arg(ChatTheme::codeBlockBg(), ChatTheme::textPrimary(), ChatTheme::border(), trackerFontSize(12)));
    diffBrowser->document()->setDefaultStyleSheet(diffStylesheet());
    diffBrowser->setHtml(diffToHtml(combined));
    root->addWidget(diffBrowser, 1);

    auto *btnLayout = new QHBoxLayout;
    btnLayout->setContentsMargins(0, 4, 0, 0);
    btnLayout->setSpacing(8);

    auto *copyBtn = new QPushButton(i18n("Copy Diff"), dialog);
    copyBtn->setCursor(Qt::PointingHandCursor);
    copyBtn->setFixedHeight(32);
    copyBtn->setStyleSheet(reviewStyle());
    connect(copyBtn, &QPushButton::clicked, this, [combined]() {
        QGuiApplication::clipboard()->setText(combined);
    });
    btnLayout->addWidget(copyBtn);

    btnLayout->addStretch();

    auto *rejectBtn = new QPushButton(group.createdFile ? i18n("Delete File") : i18n("Reject"), dialog);
    rejectBtn->setCursor(Qt::PointingHandCursor);
    rejectBtn->setFixedHeight(32);
    rejectBtn->setStyleSheet(rejectStyle());
    connect(rejectBtn, &QPushButton::clicked, this, [this, path, dialog]() {
        dialog->close();
        rejectEdit(path);
    });
    btnLayout->addWidget(rejectBtn);

    auto *keepBtn = new QPushButton(i18n("Keep"), dialog);
    keepBtn->setCursor(Qt::PointingHandCursor);
    keepBtn->setFixedHeight(32);
    keepBtn->setStyleSheet(keepStyle());
    keepBtn->setDefault(true);
    connect(keepBtn, &QPushButton::clicked, this, [this, path, dialog]() {
        dialog->close();
        acceptEdit(path);
    });
    btnLayout->addWidget(keepBtn);

    root->addLayout(btnLayout);

    // Escape closes without deciding, so a review opened by accident cannot
    // silently accept or reject the change it was showing.
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), dialog);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, dialog, &QDialog::close);

    m_reviewDialogs[path] = dialog;
    connect(dialog, &QDialog::destroyed, this, [this, path]() {
        m_reviewDialogs.remove(path);
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void EditTracker::closeReview(const QString &path)
{
    // Take the pointer out of the map first: closing triggers destroyed(), whose
    // handler removes the entry, so removing it here too would be redundant, but
    // leaving it in place until the deferred delete ran would let a re-open
    // during that window find a dangling dialog.
    if (auto dialog = m_reviewDialogs.take(path)) {
        dialog->close();
    }
}

void EditTracker::closeAllReviews()
{
    const QList<QString> paths = m_reviewDialogs.keys();
    for (const QString &path : paths) {
        closeReview(path);
    }
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
    if (accepted) {
        // The file is already in this state, so accepting only settles the queue.
        group.lastKnownContent = settledTo;
    }
    m_undoStack.append(decision);
    closeReview(path);
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
