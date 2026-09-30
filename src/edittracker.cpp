/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "edittracker.h"

#include <KLocalizedString>
#include <KTextEditor/Application>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>

#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

QString keepAllStyle()
{
    return u"QPushButton {"
           u"  background-color: #16a34a;"
           u"  color: #ffffff;"
           u"  border: none;"
           u"  border-radius: 16px;"
           u"  padding: 0 14px;"
           u"  font-weight: 600;"
           u"  font-size: 12px;"
           u"}"
           u"QPushButton:hover { background-color: #22c55e; }"
           u"QPushButton:pressed { background-color: #15803d; }"
           u"QPushButton:disabled { background-color: #1f3d2a; color: #6b7280; }"_s;
}

QString rejectAllStyle()
{
    return u"QPushButton {"
           u"  background-color: transparent;"
           u"  color: #f87171;"
           u"  border: 1px solid #7f1d1d;"
           u"  border-radius: 16px;"
           u"  padding: 0 14px;"
           u"  font-weight: 600;"
           u"  font-size: 12px;"
           u"}"
           u"QPushButton:hover { background-color: #3f1212; border-color: #ef4444; color: #fecaca; }"
           u"QPushButton:pressed { background-color: #7f1d1d; }"
           u"QPushButton:disabled { color: #6b7280; border-color: #333; }"_s;
}

QString keepStyle()
{
    return u"QPushButton {"
           u"  background-color: #166534;"
           u"  color: #bbf7d0;"
           u"  border: none;"
           u"  border-radius: 12px;"
           u"  padding: 0 10px;"
           u"  font-weight: 600;"
           u"  font-size: 11px;"
           u"}"
           u"QPushButton:hover { background-color: #16a34a; color: #ffffff; }"
           u"QPushButton:pressed { background-color: #15803d; }"_s;
}

QString rejectStyle()
{
    return u"QPushButton {"
           u"  background-color: transparent;"
           u"  color: #fca5a5;"
           u"  border: 1px solid #7f1d1d;"
           u"  border-radius: 12px;"
           u"  padding: 0 10px;"
           u"  font-weight: 600;"
           u"  font-size: 11px;"
           u"}"
           u"QPushButton:hover { background-color: #3f1212; color: #fecaca; border-color: #ef4444; }"
           u"QPushButton:pressed { background-color: #7f1d1d; }"_s;
}

QString reviewStyle()
{
    return u"QPushButton {"
           u"  background-color: #27272a;"
           u"  color: #e4e4e7;"
           u"  border: 1px solid #3f3f46;"
           u"  border-radius: 12px;"
           u"  padding: 0 10px;"
           u"  font-weight: 600;"
           u"  font-size: 11px;"
           u"}"
           u"QPushButton:hover { background-color: #3f3f46; color: #ffffff; border-color: #52525b; }"
           u"QPushButton:pressed { background-color: #18181b; }"_s;
}

void diffStats(const QString &diff, int *added, int *removed)
{
    int plus = 0;
    int minus = 0;
    const QStringList lines = diff.split(u'\n');
    for (const QString &line : lines) {
        if (line.startsWith(u"+++"_s) || line.startsWith(u"---"_s) || line.startsWith(u"@@"_s)) {
            continue;
        }
        if (line.startsWith(u'+')) {
            ++plus;
        } else if (line.startsWith(u'-')) {
            ++minus;
        }
    }
    if (added) {
        *added = plus;
    }
    if (removed) {
        *removed = minus;
    }
}

QString diffToHtml(const QString &diff)
{
    QString html = u"<body>"_s;
    const QStringList lines = diff.split(u'\n');
    if (lines.isEmpty()) {
        return u"<body><p> </p></body>"_s;
    }
    for (const QString &line : lines) {
        const QString escaped = line.isEmpty() ? u"&nbsp;"_s : line.toHtmlEscaped();
        if (line.startsWith(u"---"_s) || line.startsWith(u"+++"_s) || line.startsWith(u"@@"_s)) {
            html += u"<p class='hunk'>%1</p>"_s.arg(escaped);
        } else if (line.startsWith(u'+')) {
            html += u"<p class='added'>%1</p>"_s.arg(escaped);
        } else if (line.startsWith(u'-')) {
            html += u"<p class='removed'>%1</p>"_s.arg(escaped);
        } else {
            html += u"<p>%1</p>"_s.arg(escaped);
        }
    }
    html += u"</body>"_s;
    return html;
}

QString reviewFilePath(const QString &path)
{
    const QString tmpRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + u"/kateai-review"_s;
    QDir().mkpath(tmpRoot);
    const QString fileName = QFileInfo(path).fileName();
    const QString unique = QString::number(qHash(path), 16);
    return tmpRoot + u'/' + unique + u'-' + fileName + u".diff"_s;
}

bool writeReviewFile(const QString &tmpPath, const QString &diff)
{
    QFile file(tmpPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << diff;
    if (!diff.endsWith(u'\n')) {
        out << u'\n';
    }
    return true;
}

KTextEditor::MainWindow *activeEditorWindow()
{
    auto *editor = KTextEditor::Editor::instance();
    if (!editor || !editor->application()) {
        return nullptr;
    }
    return editor->application()->activeMainWindow();
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
    m_countLabel->setStyleSheet(u"QLabel { color: #e4e4e7; font-size: 12px; font-weight: 600; letter-spacing: 0.2px; }"_s);
    barLayout->addWidget(m_countLabel);
    barLayout->addStretch();

    m_acceptAllBtn = new QPushButton(i18n("Keep All"), m_barWidget);
    m_acceptAllBtn->setCursor(Qt::PointingHandCursor);
    m_acceptAllBtn->setFixedHeight(30);
    m_acceptAllBtn->setStyleSheet(keepAllStyle());
    connect(m_acceptAllBtn, &QPushButton::clicked, this, &EditTracker::acceptAll);
    barLayout->addWidget(m_acceptAllBtn);

    m_rejectAllBtn = new QPushButton(i18n("Reject All"), m_barWidget);
    m_rejectAllBtn->setCursor(Qt::PointingHandCursor);
    m_rejectAllBtn->setFixedHeight(30);
    m_rejectAllBtn->setStyleSheet(rejectAllStyle());
    connect(m_rejectAllBtn, &QPushButton::clicked, this, &EditTracker::rejectAll);
    barLayout->addWidget(m_rejectAllBtn);

    m_barWidget->setStyleSheet(
        u"#EditTrackerBar {"
        u"  background-color: #18181b;"
        u"  border-top: 1px solid #27272a;"
        u"}"_s);

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
    m_scrollArea->setStyleSheet(
        u"QScrollArea { background: transparent; border: none; }"
        u"QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
        u"QScrollBar::handle:vertical { background: #3f3f46; border-radius: 4px; min-height: 24px; }"
        u"QScrollBar::handle:vertical:hover { background: #52525b; }"
        u"QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"_s);

    QWidget *scrollContent = new QWidget(m_scrollArea);
    scrollContent->setObjectName(u"EditTrackerScrollContent"_s);
    m_scrollArea->setWidget(scrollContent);
    listLayout->addWidget(m_scrollArea);

    m_editListLayout = new QVBoxLayout(scrollContent);
    m_editListLayout->setContentsMargins(0, 0, 2, 0);
    m_editListLayout->setSpacing(6);
    m_editListLayout->setAlignment(Qt::AlignTop);

    m_listContainer->setStyleSheet(
        u"#EditTrackerList { background-color: #141416; }"_s);

    root->addWidget(m_listContainer);

    setStyleSheet(
        u"#EditTracker {"
        u"  background-color: #141416;"
        u"  border-top: 1px solid #27272a;"
        u"}"_s);

    m_barWidget->hide();
    m_listContainer->hide();
}

void EditTracker::addEdit(const QString &path, const QString &toolName, const QString &diff,
                          const QString &oldContent, const QString &newContent)
{
    closeReview(path);

    EditEntry entry;
    entry.path = path;
    entry.toolName = toolName;
    entry.diff = diff;
    entry.oldContent = oldContent;
    entry.newContent = newContent;
    entry.accepted = false;
    entry.rejected = false;

    m_edits[path] = entry;
    updateUI();
    Q_EMIT editsChanged(true);
}

void EditTracker::clear()
{
    closeAllReviews();
    m_edits.clear();
    updateUI();
    m_barWidget->hide();
    m_listContainer->hide();
    Q_EMIT editsChanged(false);
}

bool EditTracker::hasPendingEdits() const
{
    for (const auto &entry : m_edits) {
        if (!entry.accepted && !entry.rejected) {
            return true;
        }
    }
    return false;
}

QList<EditEntry> EditTracker::pendingEdits() const
{
    QList<EditEntry> result;
    for (const auto &entry : m_edits) {
        if (!entry.accepted && !entry.rejected) {
            result.append(entry);
        }
    }
    return result;
}

void EditTracker::acceptAll()
{
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        if (!it->accepted && !it->rejected) {
            it->accepted = true;
            Q_EMIT editAccepted(it->path, it->toolName, it->newContent);
        }
    }
    clear();
}

void EditTracker::rejectAll()
{
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        if (!it->accepted && !it->rejected) {
            it->rejected = true;
            Q_EMIT editRejected(it->path, it->toolName, it->oldContent);
        }
    }
    clear();
}

void EditTracker::updateUI()
{
    int pendingCount = 0;
    for (const auto &entry : m_edits) {
        if (!entry.accepted && !entry.rejected) {
            ++pendingCount;
        }
    }

    if (pendingCount > 0) {
        m_countLabel->setText(i18np("1 file", "%n files", pendingCount));
        m_acceptAllBtn->setEnabled(true);
        m_rejectAllBtn->setEnabled(true);
        m_barWidget->show();
        m_listContainer->show();
    } else {
        m_countLabel->clear();
        m_acceptAllBtn->setEnabled(false);
        m_rejectAllBtn->setEnabled(false);
        m_barWidget->hide();
        m_listContainer->hide();
    }

    rebuildEditList();
}

void EditTracker::rebuildEditList()
{
    QLayoutItem *item;
    while ((item = m_editListLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    QStringList paths;
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        if (!it->accepted && !it->rejected) {
            paths.append(it.key());
        }
    }
    paths.sort(Qt::CaseInsensitive);

    for (const QString &path : paths) {
        const EditEntry &entry = m_edits[path];

        QWidget *editWidget = new QWidget(m_scrollArea->widget());
        editWidget->setObjectName(u"EditFileRow"_s);
        editWidget->setStyleSheet(
            u"#EditFileRow {"
            u"  background-color: #1c1c20;"
            u"  border: 1px solid #2a2a2e;"
            u"  border-radius: 10px;"
            u"}"_s);
        auto *editLayout = new QHBoxLayout(editWidget);
        editLayout->setContentsMargins(10, 6, 8, 6);
        editLayout->setSpacing(8);

        QLabel *nameLabel = new QLabel(QFileInfo(entry.path).fileName(), editWidget);
        nameLabel->setStyleSheet(
            u"QLabel { color: #f4f4f5; font-size: 12px; font-weight: 600; background: transparent; border: none; }"_s);
        nameLabel->setToolTip(entry.path);
        nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        nameLabel->setMinimumWidth(40);
        editLayout->addWidget(nameLabel, 1);

        int added = 0;
        int removed = 0;
        diffStats(entry.diff, &added, &removed);

        QLabel *addedLabel = new QLabel(editWidget);
        addedLabel->setStyleSheet(
            u"QLabel { color: #4ade80; font-size: 12px; font-weight: 700; font-family: monospace; background: transparent; border: none; }"_s);
        addedLabel->setText(added > 0 ? u"+%1"_s.arg(added) : QString());
        addedLabel->setVisible(added > 0);
        editLayout->addWidget(addedLabel);

        QLabel *removedLabel = new QLabel(editWidget);
        removedLabel->setStyleSheet(
            u"QLabel { color: #f87171; font-size: 12px; font-weight: 700; font-family: monospace; background: transparent; border: none; }"_s);
        removedLabel->setText(removed > 0 ? u"-%1"_s.arg(removed) : QString());
        removedLabel->setVisible(removed > 0);
        editLayout->addWidget(removedLabel);

        QPushButton *reviewBtn = new QPushButton(i18n("Review"), editWidget);
        reviewBtn->setCursor(Qt::PointingHandCursor);
        reviewBtn->setFixedHeight(24);
        reviewBtn->setStyleSheet(reviewStyle());
        connect(reviewBtn, &QPushButton::clicked, this, [this, path = entry.path]() {
            openReview(path);
        });
        editLayout->addWidget(reviewBtn);

        QPushButton *acceptBtn = new QPushButton(i18n("Keep"), editWidget);
        acceptBtn->setCursor(Qt::PointingHandCursor);
        acceptBtn->setFixedHeight(24);
        acceptBtn->setStyleSheet(keepStyle());
        connect(acceptBtn, &QPushButton::clicked, this, [this, path = entry.path]() {
            acceptEdit(path);
        });
        editLayout->addWidget(acceptBtn);

        QPushButton *rejectBtn = new QPushButton(i18n("Reject"), editWidget);
        rejectBtn->setCursor(Qt::PointingHandCursor);
        rejectBtn->setFixedHeight(24);
        rejectBtn->setStyleSheet(rejectStyle());
        connect(rejectBtn, &QPushButton::clicked, this, [this, path = entry.path]() {
            rejectEdit(path);
        });
        editLayout->addWidget(rejectBtn);

        m_editListLayout->addWidget(editWidget);
    }

    m_editListLayout->addStretch();

    const int rowCount = static_cast<int>(paths.size());
    const int visibleRows = qMin(rowCount, 6);
    const int listHeight = visibleRows > 0 ? (visibleRows * 40 + 4) : 0;
    m_scrollArea->setFixedHeight(listHeight);
}

void EditTracker::openReview(const QString &path)
{
    auto it = m_edits.find(path);
    if (it == m_edits.end() || it->accepted || it->rejected) {
        return;
    }

    const QString tmpPath = reviewFilePath(path);
    if (writeReviewFile(tmpPath, it->diff)) {
        m_reviewFiles[path] = tmpPath;
        if (auto *mainWindow = activeEditorWindow()) {
            mainWindow->openUrl(QUrl::fromLocalFile(tmpPath));
            return;
        }
    }

    if (auto existing = m_reviewDialogs.value(path)) {
        existing->raise();
        existing->activateWindow();
        return;
    }

    auto *dialog = new QDialog(window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(i18n("Review %1", QFileInfo(path).fileName()));
    dialog->setModal(false);
    dialog->resize(720, 520);
    dialog->setStyleSheet(
        u"QDialog { background-color: #121214; }"
        u"QLabel { background: transparent; }"_s);

    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    QLabel *title = new QLabel(QFileInfo(path).fileName(), dialog);
    title->setStyleSheet(u"QLabel { color: #f4f4f5; font-size: 16px; font-weight: 700; }"_s);
    root->addWidget(title);

    QLabel *subtitle = new QLabel(path, dialog);
    subtitle->setStyleSheet(u"QLabel { color: #a1a1aa; font-size: 11px; font-family: monospace; }"_s);
    subtitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);

    auto *diffBrowser = new QTextBrowser(dialog);
    diffBrowser->setReadOnly(true);
    diffBrowser->setOpenExternalLinks(false);
    diffBrowser->setFrameShape(QFrame::NoFrame);
    diffBrowser->setStyleSheet(
        u"QTextBrowser {"
        u"  background-color: #09090b;"
        u"  color: #d4d4d8;"
        u"  border: 1px solid #27272a;"
        u"  border-radius: 10px;"
        u"  padding: 8px;"
        u"  font-family: monospace;"
        u"  font-size: 12px;"
        u"}"_s);
    diffBrowser->document()->setDefaultStyleSheet(
        u"body { color: #d4d4d8; font-family: monospace; font-size: 12px; margin: 0; padding: 0; }"
        u".removed { color: #fca5a5; background-color: #3f1212; }"
        u".added { color: #86efac; background-color: #14532d; }"
        u".hunk { color: #71717a; }"
        u"p { margin: 0; padding: 1px 6px; white-space: pre-wrap; }"_s);
    diffBrowser->setHtml(diffToHtml(it->diff));
    root->addWidget(diffBrowser, 1);

    auto *btnLayout = new QHBoxLayout;
    btnLayout->setContentsMargins(0, 4, 0, 0);
    btnLayout->setSpacing(8);

    QPushButton *closeBtn = new QPushButton(i18n("Close"), dialog);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setFixedHeight(32);
    closeBtn->setStyleSheet(reviewStyle());
    connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::close);
    btnLayout->addWidget(closeBtn);
    btnLayout->addStretch();

    QPushButton *rejectBtn = new QPushButton(i18n("Reject"), dialog);
    rejectBtn->setCursor(Qt::PointingHandCursor);
    rejectBtn->setFixedHeight(32);
    rejectBtn->setStyleSheet(rejectStyle());
    connect(rejectBtn, &QPushButton::clicked, this, [this, path, dialog]() {
        dialog->close();
        rejectEdit(path);
    });
    btnLayout->addWidget(rejectBtn);

    QPushButton *keepBtn = new QPushButton(i18n("Keep"), dialog);
    keepBtn->setCursor(Qt::PointingHandCursor);
    keepBtn->setFixedHeight(32);
    keepBtn->setStyleSheet(keepAllStyle());
    connect(keepBtn, &QPushButton::clicked, this, [this, path, dialog]() {
        dialog->close();
        acceptEdit(path);
    });
    btnLayout->addWidget(keepBtn);

    root->addLayout(btnLayout);

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

void EditTracker::finishEdit(const QString &path)
{
    closeReview(path);
    if (!hasPendingEdits()) {
        clear();
        return;
    }
    updateUI();
}

void EditTracker::acceptEdit(const QString &path)
{
    auto it = m_edits.find(path);
    if (it != m_edits.end() && !it->accepted && !it->rejected) {
        it->accepted = true;
        Q_EMIT editAccepted(it->path, it->toolName, it->newContent);
        finishEdit(path);
    }
}

void EditTracker::rejectEdit(const QString &path)
{
    auto it = m_edits.find(path);
    if (it != m_edits.end() && !it->accepted && !it->rejected) {
        it->rejected = true;
        Q_EMIT editRejected(it->path, it->toolName, it->oldContent);
        finishEdit(path);
    }
}

} // namespace KateAi
