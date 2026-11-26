/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "kateecosystembridge.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KTextEditor/MainWindow>
#include <KXMLGUIClient>

#include <QAbstractItemModel>
#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

constexpr auto kBuildPluginId = "katebuildplugin";
constexpr auto kKonsolePluginId = "katekonsoleplugin";
constexpr auto kProjectPluginId = "kateprojectplugin";
constexpr auto kBacktracePluginId = "katebacktracebrowserplugin";

// Minimal diff syntax highlighter for native Kate review tabs.
class DiffSyntaxHighlighter : public QSyntaxHighlighter
{
public:
    explicit DiffSyntaxHighlighter(QTextDocument *parent)
        : QSyntaxHighlighter(parent)
    {
        m_addFmt.setForeground(QColor(u"#2ea043"_s));
        m_delFmt.setForeground(QColor(u"#f85149"_s));
        m_hunkFmt.setForeground(QColor(u"#58a6ff"_s));
        m_hunkFmt.setFontWeight(QFont::Bold);
        m_headerFmt.setForeground(QColor(u"#d29922"_s));
        m_headerFmt.setFontWeight(QFont::Bold);
    }

protected:
    void highlightBlock(const QString &text) override
    {
        if (text.startsWith(u"+++ "_s) || text.startsWith(u"--- "_s) || text.startsWith(u"=== "_s)
            || text.startsWith(u"diff "_s)) {
            setFormat(0, text.size(), m_headerFmt);
        } else if (text.startsWith(u"@@"_s)) {
            setFormat(0, text.size(), m_hunkFmt);
        } else if (text.startsWith(u'+')) {
            setFormat(0, text.size(), m_addFmt);
        } else if (text.startsWith(u'-')) {
            setFormat(0, text.size(), m_delFmt);
        }
    }

private:
    QTextCharFormat m_addFmt;
    QTextCharFormat m_delFmt;
    QTextCharFormat m_hunkFmt;
    QTextCharFormat m_headerFmt;
};

} // namespace

KateEcosystemBridge::KateEcosystemBridge(QObject *parent)
    : EcosystemProvider(parent)
{
}

KateEcosystemBridge::~KateEcosystemBridge() = default;

void KateEcosystemBridge::setMainWindow(KTextEditor::MainWindow *mainWindow)
{
    if (m_mainWindow == mainWindow) {
        return;
    }
    m_mainWindow = mainWindow;
    m_hookedProjectView = nullptr;
    if (m_mainWindow) {
        connect(m_mainWindow, &KTextEditor::MainWindow::pluginViewCreated, this,
                [this](const QString &name, QObject *pluginView) {
                    onPluginViewCreated(name, pluginView);
                },
                Qt::UniqueConnection);
        hookProjectTreeMenu();
    }
}

void KateEcosystemBridge::onPluginViewCreated(const QString &name, QObject *pluginView)
{
    Q_UNUSED(pluginView);
    if (name == QLatin1String(kProjectPluginId)) {
        hookProjectTreeMenu();
    }
}

void KateEcosystemBridge::hookProjectTreeMenu()
{
    if (!m_mainWindow) {
        return;
    }
    QObject *projectView = m_mainWindow->pluginView(QLatin1String(kProjectPluginId));
    if (!projectView || projectView == m_hookedProjectView) {
        return;
    }
    m_hookedProjectView = projectView;
    // KateProjectPluginView emits projectTreeContextMenuAboutToShow(QMenu*, QString, QString, int)
    // right before displaying the Project tree right-click menu.
    if (projectView->metaObject()->indexOfSignal("projectTreeContextMenuAboutToShow(QMenu*,QString,QString,int)") >= 0) {
        connect(projectView, SIGNAL(projectTreeContextMenuAboutToShow(QMenu *, QString, QString, int)),
                this, SLOT(onProjectTreeContextMenu(QMenu *, QString, QString, int)),
                Qt::UniqueConnection);
    }
}

void KateEcosystemBridge::onProjectTreeContextMenu(QMenu *menu, const QString &path, const QString &baseDir, int itemType)
{
    Q_UNUSED(itemType);
    if (!menu || path.isEmpty()) {
        return;
    }
    static const QString menuObjName = u"kateai_project_tree_menu"_s;
    for (QAction *a : menu->actions()) {
        if (a->menu() && a->menu()->objectName() == menuObjName) {
            return;
        }
    }
    menu->addSeparator();
    auto *aiMenu = menu->addMenu(QIcon::fromTheme(u"help-hint"_s), i18n("Kate AI"));
    aiMenu->setObjectName(menuObjName);

    const bool isDir = QFileInfo(path).isDir();
    QAction *askAct = aiMenu->addAction(isDir ? i18n("Ask Kate AI About This Folder")
                                              : i18n("Ask Kate AI About This File"));
    connect(askAct, &QAction::triggered, this, [this, path, baseDir]() {
        Q_EMIT projectTreeActionRequested(u"ask"_s, path, baseDir);
    });

    QAction *summarizeAct = aiMenu->addAction(isDir ? i18n("Summarize Module / Folder")
                                                    : i18n("Summarize File"));
    connect(summarizeAct, &QAction::triggered, this, [this, path, baseDir]() {
        Q_EMIT projectTreeActionRequested(u"summarize"_s, path, baseDir);
    });

    if (!isDir) {
        QAction *testsAct = aiMenu->addAction(i18n("Add Tests for This File"));
        connect(testsAct, &QAction::triggered, this, [this, path, baseDir]() {
            Q_EMIT projectTreeActionRequested(u"tests"_s, path, baseDir);
        });
    }
}

QObject *KateEcosystemBridge::buildPluginView() const
{
    return m_mainWindow ? m_mainWindow->pluginView(QLatin1String(kBuildPluginId)) : nullptr;
}

QObject *KateEcosystemBridge::konsolePluginView() const
{
    return m_mainWindow ? m_mainWindow->pluginView(QLatin1String(kKonsolePluginId)) : nullptr;
}

QAction *KateEcosystemBridge::findPluginAction(QObject *pluginView, const QString &actionName) const
{
    if (!pluginView) {
        return nullptr;
    }
    auto *client = dynamic_cast<KXMLGUIClient *>(pluginView);
    if (!client || !client->actionCollection()) {
        return nullptr;
    }
    return client->actionCollection()->action(actionName);
}

bool KateEcosystemBridge::isBuildPluginAvailable() const
{
    return buildPluginView() != nullptr;
}

QList<BuildTargetInfo> KateEcosystemBridge::buildTargets(const QString &workspace) const
{
    QList<BuildTargetInfo> out;

    // 1. Inspect katebuildplugin's target tree model if loaded.
    if (m_mainWindow && m_mainWindow->window() && buildPluginView()) {
        const QList<QWidget *> widgets = m_mainWindow->window()->findChildren<QWidget *>();
        for (QWidget *w : widgets) {
            if (!w || QByteArrayView(w->metaObject()->className()) != "TargetsUi") {
                continue;
            }
            if (auto *tv = w->findChild<QTreeView *>()) {
                if (const QAbstractItemModel *model = tv->model()) {
                    const int setCount = model->rowCount();
                    for (int s = 0; s < setCount; ++s) {
                        const QModelIndex setIdx = model->index(s, 0);
                        const QString setName = model->data(setIdx, Qt::DisplayRole).toString().trimmed();
                        const QString buildDir = model->columnCount() > 1
                            ? model->data(model->index(s, 1), Qt::DisplayRole).toString().trimmed()
                            : QString();
                        const int targetCount = model->rowCount(setIdx);
                        for (int t = 0; t < targetCount; ++t) {
                            const QString tName = model->data(model->index(t, 0, setIdx), Qt::DisplayRole).toString().trimmed();
                            const QString cmd = model->columnCount(setIdx) > 1
                                ? model->data(model->index(t, 1, setIdx), Qt::DisplayRole).toString().trimmed()
                                : QString();
                            if (!tName.isEmpty()) {
                                BuildTargetInfo info;
                                info.name = setName.isEmpty() ? tName : u"%1 / %2"_s.arg(setName, tName);
                                info.type = u"kate-build"_s;
                                info.buildDir = buildDir;
                                info.command = cmd;
                                info.isDefault = out.isEmpty();
                                out.append(std::move(info));
                            }
                        }
                    }
                }
            }
            break;
        }
    }

    // 2. Merge workspace filesystem discovery (CMake File API, Cargo, npm, Go, pytest).
    const QList<BuildTargetInfo> fsTargets = discoverWorkspaceBuildTargets(workspace);
    for (const BuildTargetInfo &t : fsTargets) {
        out.append(t);
    }
    return out;
}

EcosystemActionResult KateEcosystemBridge::triggerBuild(const QString &targetName)
{
    QObject *view = buildPluginView();
    if (!view) {
        return {false, u"Kate's Build plugin (katebuildplugin) is not loaded. Use bash to run your build command."_s};
    }

    // Try matching a specific target in TargetsUi if targetName was provided.
    if (!targetName.trimmed().isEmpty() && m_mainWindow && m_mainWindow->window()) {
        for (QWidget *w : m_mainWindow->window()->findChildren<QWidget *>()) {
            if (!w || QByteArrayView(w->metaObject()->className()) != "TargetsUi") {
                continue;
            }
            if (auto *tv = w->findChild<QTreeView *>()) {
                if (const QAbstractItemModel *model = tv->model()) {
                    const QString needle = targetName.trimmed();
                    for (int s = 0; s < model->rowCount(); ++s) {
                        const QModelIndex setIdx = model->index(s, 0);
                        for (int t = 0; t < model->rowCount(setIdx); ++t) {
                            const QModelIndex tIdx = model->index(t, 0, setIdx);
                            const QString name = model->data(tIdx, Qt::DisplayRole).toString().trimmed();
                            if (name.compare(needle, Qt::CaseInsensitive) == 0
                                || name.contains(needle, Qt::CaseInsensitive)) {
                                tv->setCurrentIndex(tIdx);
                                if (QAction *selectedAct = findPluginAction(view, u"build_selected_target"_s)) {
                                    selectedAct->trigger();
                                    return {true, u"Triggered Kate Build for target '%1'. Check lsp_diagnostics once compilation finishes."_s.arg(name)};
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
    }

    // Fallback to default target action, then selected target action.
    for (const QString &actName : {u"build_default_target"_s, u"build_selected_target"_s, u"quick_compile"_s}) {
        if (QAction *act = findPluginAction(view, actName)) {
            if (act->isEnabled()) {
                act->trigger();
                return {true, u"Triggered Kate Build (%1). Check lsp_diagnostics for structured compiler errors."_s.arg(actName)};
            }
        }
    }
    return {false, u"No enabled build action found in Kate's Build plugin. Configure a build target or run build via bash."_s};
}

bool KateEcosystemBridge::isTerminalAvailable() const
{
    return konsolePluginView() != nullptr;
}

EcosystemActionResult KateEcosystemBridge::runInTerminal(const QString &command, const QString &workingDir)
{
    QObject *view = konsolePluginView();
    if (!view) {
        return {false, u"Kate's Terminal plugin (katekonsoleplugin) is not loaded."_s};
    }
    const QString cmd = command.trimmed();
    if (cmd.isEmpty()) {
        return {false, u"Command cannot be empty."_s};
    }

    // Locate the KateConsole widget child to ensure the toolview is visible and
    // invoke cd() / sendInput().
    QObject *consoleObj = nullptr;
    for (QObject *child : view->findChildren<QObject *>()) {
        if (child && QByteArrayView(child->metaObject()->className()) == "KateConsole") {
            consoleObj = child;
            break;
        }
    }
    if (!consoleObj && m_mainWindow && m_mainWindow->window()) {
        for (QWidget *w : m_mainWindow->window()->findChildren<QWidget *>()) {
            if (w && QByteArrayView(w->metaObject()->className()) == "KateConsole") {
                consoleObj = w;
                break;
            }
        }
    }
    if (!consoleObj) {
        return {false, u"Could not locate Kate's embedded Konsole widget."_s};
    }

    if (QAction *showAct = findPluginAction(view, u"katekonsole_tools_toggle_focus"_s)) {
        // Only focus if console isn't visible yet.
        if (auto *w = qobject_cast<QWidget *>(consoleObj); w && !w->isVisible()) {
            showAct->trigger();
        }
    }

    if (!workingDir.trimmed().isEmpty()) {
        QMetaObject::invokeMethod(consoleObj, "cd", Qt::DirectConnection, Q_ARG(QString, workingDir.trimmed()));
    }

    const QString withNewline = cmd.endsWith(u'\n') ? cmd : (cmd + u'\n');
    const bool sent = QMetaObject::invokeMethod(consoleObj, "sendInput", Qt::DirectConnection,
                                                Q_ARG(QString, withNewline));
    if (!sent) {
        return {false, u"Failed to send command to Kate's embedded terminal."_s};
    }
    return {true, u"Sent command to Kate's embedded terminal: %1"_s.arg(cmd)};
}

QString KateEcosystemBridge::terminalContext() const
{
    if (!m_mainWindow || !m_mainWindow->window()) {
        return {};
    }
    for (QWidget *w : m_mainWindow->window()->findChildren<QWidget *>()) {
        if (!w || QByteArrayView(w->metaObject()->className()) != "KateConsole") {
            continue;
        }
        // Check if any Konsole TerminalDisplay child exposes selectedText() via Q_INVOKABLE.
        for (QObject *child : w->findChildren<QObject *>()) {
            if (!child) {
                continue;
            }
            if (child->metaObject()->indexOfMethod("selectedText()") >= 0) {
                QString sel;
                if (QMetaObject::invokeMethod(child, "selectedText", Qt::DirectConnection,
                                              Q_RETURN_ARG(QString, sel))) {
                    if (!sel.trimmed().isEmpty()) {
                        return sel.trimmed();
                    }
                }
            }
        }
        break;
    }
    return {};
}

QString KateEcosystemBridge::activeBacktraceText() const
{
    if (!m_mainWindow || !m_mainWindow->window()) {
        return {};
    }
    if (!m_mainWindow->pluginView(QLatin1String(kBacktracePluginId))) {
        return {};
    }
    for (QWidget *w : m_mainWindow->window()->findChildren<QWidget *>()) {
        if (!w) {
            continue;
        }
        const QByteArray cls(w->metaObject()->className());
        if (!cls.contains("Backtrace") && !cls.contains("BtBrowser")) {
            continue;
        }
        if (auto *tv = w->findChild<QTreeView *>()) {
            if (const QAbstractItemModel *model = tv->model()) {
                QStringList lines;
                for (int r = 0; r < model->rowCount() && r < 80; ++r) {
                    QStringList cols;
                    for (int c = 0; c < model->columnCount(); ++c) {
                        const QString cell = model->data(model->index(r, c), Qt::DisplayRole).toString().trimmed();
                        if (!cell.isEmpty()) {
                            cols.append(cell);
                        }
                    }
                    if (!cols.isEmpty()) {
                        lines.append(cols.join(u" | "_s));
                    }
                }
                if (!lines.isEmpty()) {
                    return lines.join(u'\n');
                }
            }
        }
    }
    return {};
}

QWidget *KateEcosystemBridge::openNativeTab(const QString &title,
                                            const QString &subtitle,
                                            const QString &content,
                                            bool isDiff,
                                            const std::function<void()> &onAcceptAll,
                                            const std::function<void()> &onRejectAll)
{
    if (!m_mainWindow) {
        return nullptr;
    }
    auto *container = new QWidget();
    container->setAttribute(Qt::WA_DeleteOnClose);
    container->setWindowTitle(title);
    container->setWindowIcon(QIcon::fromTheme(isDiff ? u"vcs-diff"_s : u"help-hint"_s));

    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *viewer = new QPlainTextEdit(container);
    viewer->setReadOnly(true);
    viewer->setLineWrapMode(QPlainTextEdit::NoWrap);
    viewer->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    viewer->setPlainText(content);
    if (isDiff) {
        new DiffSyntaxHighlighter(viewer->document());
    }

    auto closeTab = [self = QPointer<KateEcosystemBridge>(this), containerWeak = QPointer<QWidget>(container)]() {
        if (!containerWeak) {
            return;
        }
        if (self && self->m_mainWindow) {
            self->m_mainWindow->removeWidget(containerWeak);
        }
        // KTextEditor::MainWindow::removeWidget() detaches without deleting.
        containerWeak->deleteLater();
    };

    auto *header = new QHBoxLayout();
    auto *titleLbl = new QLabel(u"<b>%1</b>"_s.arg(title.toHtmlEscaped()), container);
    header->addWidget(titleLbl);
    if (!subtitle.isEmpty()) {
        auto *subLbl = new QLabel(subtitle, container);
        subLbl->setStyleSheet(u"color: gray;"_s);
        header->addWidget(subLbl);
    }
    header->addStretch(1);

    auto *copyBtn = new QPushButton(QIcon::fromTheme(u"edit-copy"_s), i18n("Copy"), container);
    connect(copyBtn, &QPushButton::clicked, container, [viewerWeak = QPointer<QPlainTextEdit>(viewer)]() {
        if (!viewerWeak) {
            return;
        }
        if (QClipboard *cb = QGuiApplication::clipboard()) {
            cb->setText(viewerWeak->toPlainText());
        }
    });
    header->addWidget(copyBtn);

    if (onAcceptAll) {
        auto *acceptBtn = new QPushButton(QIcon::fromTheme(u"dialog-ok-apply"_s), i18n("Accept All"), container);
        connect(acceptBtn, &QPushButton::clicked, container, [closeTab, onAcceptAll]() {
            onAcceptAll();
            closeTab();
        });
        header->addWidget(acceptBtn);
    }
    if (onRejectAll) {
        auto *rejectBtn = new QPushButton(QIcon::fromTheme(u"dialog-cancel"_s), i18n("Reject All"), container);
        connect(rejectBtn, &QPushButton::clicked, container, [closeTab, onRejectAll]() {
            onRejectAll();
            closeTab();
        });
        header->addWidget(rejectBtn);
    }

    auto *closeBtn = new QPushButton(QIcon::fromTheme(u"tab-close"_s), i18n("Close"), container);
    connect(closeBtn, &QPushButton::clicked, container, closeTab);
    header->addWidget(closeBtn);
    layout->addLayout(header);
    layout->addWidget(viewer, 1);

    if (!m_mainWindow->addWidget(container)) {
        delete container;
        return nullptr;
    }
    return container;
}

} // namespace KateAi
