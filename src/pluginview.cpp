/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "pluginview.h"
#include "chatwidget.h"
#include "mcp.h"
#include "chattheme.h"
#include "configpage.h"
#include "plugin.h"
#include "sessionstore.h"
#include "workspace.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KSyntaxHighlighting/Theme>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/View>
#include <KXMLGUIFactory>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QIcon>
#include <QKeySequence>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

    KateAiView::KateAiView(KateAiPlugin *plugin, KTextEditor::MainWindow *mainWindow)
    : QObject(plugin)
    , m_plugin(plugin)
    , m_mainWindow(mainWindow)
    {
        // Initialize the plugin with its component name and UI resource file
        setComponentName(u"kateai"_s, i18n("Kate AI"));
        setXMLFile(u"ui.rc"_s);

        // Create the tool view panel on the right side of the main window
        if (m_mainWindow) {
            m_toolView = m_mainWindow->createToolView(plugin,
                                                      u"kateai"_s,
                                                      KTextEditor::MainWindow::Right,
                                                      QIcon::fromTheme(u"help-hint"_s),
                                                      i18n("Kate AI"));
        }

        // Install the configured theme before any widget reads palette colours.
        ChatTheme::loadTheme(plugin->settings());

        // The tool view is the KateAI window: paint it from the window-chrome
        // colours, which follow the editor theme in inherit mode. The chat
        // widget paints its own content on top of it.
        if (m_toolView) {
            m_toolView->setObjectName(u"kateaiToolView"_s);
            m_toolView->setAttribute(Qt::WA_StyledBackground, true);
        }
        applyWindowChrome();

        // Create the chat widget that will contain the AI interface
        if (m_toolView) {
            m_chat = new ChatWidget(m_toolView);
        }

        // Ensure the tool view has a layout and add our chat widget to it
        // Configure the chat widget with plugin settings and set up document bridging
        if (m_toolView && m_chat) {
            if (!m_toolView->layout()) {
                auto *layout = new QVBoxLayout(m_toolView);
                layout->setContentsMargins(0, 0, 0, 0);
            }
            m_toolView->layout()->addWidget(m_chat);
        }

        // Configure the chat widget with plugin settings and set up document bridging
        if (m_chat) {
            m_chat->setSettings(plugin->settings());
            if (m_chat->agent()) {
                m_chat->agent()->setDocumentBridge(&m_bridge);
                // Every chat needs an ID before its first turn. Otherwise the
                // turn-finished autosave is skipped, and switching to another
                // conversation can save the new messages under an empty ID.
                // Load first: SessionStore may repair a stale active pointer by
                // falling back to the newest persisted transcript.
                const auto sessionData = SessionStore::load();
                QString activeId = SessionStore::getActiveConversationId();
                if (activeId.isEmpty()) {
                    activeId = SessionStore::createNewConversation();
                }
                m_chat->setCurrentConversationId(activeId);

                // Restore session data
                if (!sessionData.messages.isEmpty()) {
                    m_chat->agent()->restoreSession(sessionData);
                    m_chat->rebuildTranscript();
                }
                // The active conversation may be new and not yet listed, but
                // history should still reflect any previously saved threads.
                m_chat->updateHistoryButton();
            }
        }
        refreshWorkspace();

        if (plugin && m_mainWindow) {
            connect(plugin, &KateAiPlugin::settingsChanged, this, [this](const Settings &settings) {
                ChatTheme::loadTheme(settings);
                applyWindowChrome();
                if (m_chat) {
                    m_chat->setSettings(settings);
                }
                refreshWorkspace();
            });
        }
        // Inheriting the Kate editor theme has to follow the editor live: the
        // window repaints itself when its colour scheme changes, the chat UI
        // inside it does not without this.
        //
        // Two paths can carry the change, and both need handling:
        //  - Editor::configChanged fires when the editor's own configuration
        //    changes, including a manual scheme switch in Kate's settings.
        //  - QEvent::ApplicationPaletteChange (caught via eventFilter on qApp)
        //    fires when KDE switches the system theme (dark <-> light); Kate
        //    follows it and swaps the editor scheme, but configChanged does
        //    not reliably fire for that path.
        //
        // Both can arrive before Editor::theme() reflects the new scheme, so
        // the check is deferred to the next event-loop turn with
        // QTimer::singleShot(0). The comparison uses the theme's background
        // colour rather than only the name: KDE can swap the scheme under the
        // same name, and a name-only check would miss the change.
        // Track the application palette's window colour too: a system theme
        // switch can fire ApplicationPaletteChange without touching the editor
        // scheme, and the tool view must repaint regardless.
        m_lastWindowBg = QGuiApplication::palette().color(QPalette::Window).name();
        if (auto *editor = KTextEditor::Editor::instance()) {
            const auto theme = editor->theme();
            m_lastEditorThemeName = theme.name();
            const QRgb bgRgb = theme.editorColor(KSyntaxHighlighting::Theme::EditorColorRole::BackgroundColor);
            const QRgb styleBgRgb = theme.backgroundColor(KSyntaxHighlighting::Theme::TextStyle::Normal);
            const QColor bg = bgRgb != 0 ? QColor::fromRgb(bgRgb)
                             : styleBgRgb != 0 ? QColor::fromRgb(styleBgRgb)
                             : QColor();
            m_lastEditorThemeBg = bg.isValid() ? bg.name() : QString();
            connect(editor, &KTextEditor::Editor::configChanged, this, [this]() {
                recheckEditorTheme();
            });
        }
        // KDE system theme switch (dark <-> light): Kate follows it and swaps
        // the editor scheme, but configChanged does not reliably fire for this
        // path. ApplicationPaletteChange does.
        qApp->installEventFilter(this);
        if (m_chat) {
            connect(m_chat, &ChatWidget::settingsChanged, plugin, &KateAiPlugin::setSettings);
            connect(m_chat, &ChatWidget::configureRequested, this, &KateAiView::showConfiguration);
            connect(m_chat, &ChatWidget::openFileAtLineRequested, this, &KateAiView::openFileAtLine);
            connect(m_chat, &ChatWidget::aboutToSubmit, this, [this]() {
                refreshWorkspace();
            });
        }
        if (m_mainWindow) {
            connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, [this](KTextEditor::View *) {
                refreshWorkspace();
            });
        }

        auto *ac = actionCollection();
        QPointer<QAction> toggle, fresh, ask, fix, refactor, tests, configure;

        if (ac) {
            toggle = ac->addAction(u"kateai_toggle"_s);
            if (toggle) {
                toggle->setText(i18n("Show Kate AI"));
                toggle->setIcon(QIcon::fromTheme(u"help-hint"_s));
                KActionCollection::setDefaultShortcut(toggle, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A));
                connect(toggle, &QAction::triggered, this, &KateAiView::showPanel);
            }

            fresh = ac->addAction(u"kateai_new_chat"_s);
            if (fresh) {
                fresh->setText(i18n("Kate AI: New Chat"));
                KActionCollection::setDefaultShortcut(fresh, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N));
                connect(fresh, &QAction::triggered, this, &KateAiView::newChat);
            }

            ask = ac->addAction(u"kateai_ask_selection"_s);
            if (ask) {
                ask->setText(i18n("Ask Kate AI About This"));
                connect(ask, &QAction::triggered, this, &KateAiView::askSelection);
            }

            fix = ac->addAction(u"kateai_fix_selection"_s);
            if (fix) {
                fix->setText(i18n("Fix Selection"));
                connect(fix, &QAction::triggered, this, [this]() {
                    askSelectionWithInstruction(i18n("Find and fix problems in this code. Explain the change, then apply it."));
                });
            }

            refactor = ac->addAction(u"kateai_refactor_selection"_s);
            if (refactor) {
                refactor->setText(i18n("Refactor Selection"));
                connect(refactor, &QAction::triggered, this, [this]() {
                    askSelectionWithInstruction(i18n("Refactor this code for clarity and maintainability. Explain the change, then apply it."));
                });
            }

            tests = ac->addAction(u"kateai_test_selection"_s);
            if (tests) {
                tests->setText(i18n("Add Tests for Selection"));
                connect(tests, &QAction::triggered, this, [this]() {
                    askSelectionWithInstruction(i18n("Add or improve focused tests for this code. Explain the proposed coverage first."));
                });
            }

            configure = ac->addAction(u"kateai_configure"_s);
            if (configure) {
                configure->setText(i18n("Configure Kate AI…"));
                configure->setIcon(QIcon::fromTheme(u"settings-configure"_s));
                connect(configure, &QAction::triggered, this, &KateAiView::showConfiguration);
            }
        }

        // KTextEditor does not merge plugin XML clients into a view whose context
        // menu was supplied by another plugin. Add this action at show time so it
        // is consistently available in every editor tab.
        if (m_mainWindow) {
            for (auto *view : m_mainWindow->views()) {
                addEditorContextActions(view, {ask, fix, refactor, tests});
            }
            connect(m_mainWindow, &KTextEditor::MainWindow::viewCreated, this, [this, ask, fix, refactor, tests](KTextEditor::View *view) {
                addEditorContextActions(view, {ask, fix, refactor, tests});
            });
        }

        if (m_mainWindow && m_mainWindow->guiFactory()) {
            m_mainWindow->guiFactory()->addClient(this);
        }
    }

    KateAiView::~KateAiView()
    {
        // Session is saved in ChatWidget destructor before AgentLoop is destroyed
        // m_toolView is owned by the main window, do not delete it here

        if (m_mainWindow && m_mainWindow->guiFactory()) {
            m_mainWindow->guiFactory()->removeClient(this);
        }
    }

    bool KateAiView::eventFilter(QObject *watched, QEvent *event)
    {
        // QEvent::ApplicationPaletteChange is how Qt 6 delivers a system
        // palette switch (QGuiApplication::paletteChanged is deprecated).
        // KDE dark <-> light lands here; Kate follows by swapping the editor
        // scheme, which the chat panel must mirror when inheriting.
        if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
            recheckEditorTheme();
        }
        return QObject::eventFilter(watched, event);
    }

    void KateAiView::applyWindowChrome()
    {
        // The tool view is the KateAI window: it carries the window-chrome
        // colours, which follow the editor theme in inherit mode. The chat
        // widget paints its own content on top, so this is what keeps the
        // window itself themed wherever it shows through.
        if (!m_toolView) {
            return;
        }
        m_toolView->setStyleSheet(ChatTheme::windowChrome());
    }

    void KateAiView::recheckEditorTheme()
    {
        // configChanged and ApplicationPaletteChange can both fire before
        // Editor::theme() reflects the new scheme. Defer to the next
        // event-loop turn so the theme is settled before we read it.
        QTimer::singleShot(0, this, [this]() {
            if (!m_plugin) {
                return;
            }
            const Settings settings = m_plugin->settings();
            if (settings.themeSource != u"editor"_s || !m_chat) {
                return;
            }
            // In inherit mode both the chat content and the window chrome
            // follow the editor colour scheme, so a scheme change repaints
            // both. The window palette is still tracked: a system theme
            // switch can fire ApplicationPaletteChange without touching the
            // editor scheme, and the tool view must repaint regardless.
            bool changed = QGuiApplication::palette().color(QPalette::Window).name() != m_lastWindowBg;
            m_lastWindowBg = QGuiApplication::palette().color(QPalette::Window).name();
            if (auto *editor = KTextEditor::Editor::instance()) {
                const auto theme = editor->theme();
                if (theme.isValid()) {
                    // Derive the background the same way paletteForEditorTheme does,
                    // so the comparison matches what loadTheme will actually use.
                    const QRgb bgRgb = theme.editorColor(KSyntaxHighlighting::Theme::EditorColorRole::BackgroundColor);
                    const QRgb styleBgRgb = theme.backgroundColor(KSyntaxHighlighting::Theme::TextStyle::Normal);
                    const QColor bg = bgRgb != 0 ? QColor::fromRgb(bgRgb)
                                     : styleBgRgb != 0 ? QColor::fromRgb(styleBgRgb)
                                     : QColor();
                    const QString themeBg = bg.isValid() ? bg.name() : QString();
                    // Compare both the name and the background colour: KDE can swap
                    // the scheme under the same name, and a name-only check would
                    // miss the change.
                    if (theme.name() != m_lastEditorThemeName || themeBg != m_lastEditorThemeBg) {
                        changed = true;
                    }
                    m_lastEditorThemeName = theme.name();
                    m_lastEditorThemeBg = themeBg;
                }
            }
            if (!changed) {
                return;
            }
            // loadTheme installs the new palette into the globals first, so
            // both the tool view and the chat panel read the updated colours.
            ChatTheme::loadTheme(settings);
            applyWindowChrome();
            m_chat->applyTheme();
        });
    }

    void KateAiView::openFileAtLine(const QString &path, int line)
    {
        if (!m_mainWindow || path.isEmpty()) {
            return;
        }
        // openUrl reuses the view already showing the document and opens a new
        // one otherwise, which is what "show me" should mean either way.
        m_mainWindow->openUrl(QUrl::fromLocalFile(path));
        KTextEditor::View *view = m_mainWindow->activeView();
        if (view && line > 0) {
            // Cursors are 0-based; the suggestion line is 1-based.
            view->setCursorPosition(KTextEditor::Cursor(qMax(0, line - 1), 0));
            view->setFocus();
        }
    }

    void KateAiView::showPanel()
    {
        if (m_toolView && m_mainWindow) {
            m_mainWindow->showToolView(m_toolView);
            if (m_chat) {
                m_chat->focusPrompt();
            }
        }
    }

    void KateAiView::newChat()
    {
        showPanel();
        if (m_chat) {
            m_chat->newChat();
        }
    }

    void KateAiView::askSelection()
    {
        if (m_mainWindow) {
            askSelectionWithInstruction(i18n("Explain this code."));
        }
    }

    void KateAiView::askSelectionWithInstruction(const QString &instruction)
    {
        // Show the AI panel and get the currently active editor view
        showPanel();
        if (!m_mainWindow) {
            return;
        }
        auto *view = m_mainWindow->activeView();
        if (!view) {
            return;
        }

        // Extract the selected text or the current line if nothing is selected
        QString snippet = view->selectionText();
        if (snippet.isEmpty()) {
            snippet = view->document()->line(view->cursorPosition().line());
        }

        // Get the file path or document name for context
        const QString path = view->document()->url().toLocalFile();

        // Create a comprehensive prompt with instruction, file info, and code snippet
        const QString prompt = i18n("%1\n\nFile: %2\n\nCode:\n%3",
                                    instruction,
                                    path.isEmpty() ? view->document()->documentName() : path,
                                    snippet);

        // Send the prompt to the AI for analysis
        if (m_chat) {
            m_chat->ask(prompt);
        }
    }

    void KateAiView::showConfiguration()
    {
        if (!m_mainWindow) {
            return;
        }
        // If configuration dialog already exists, bring it to the front
        if (m_configDialog) {
            m_configDialog->raise();
            m_configDialog->activateWindow();
            return;
        }

        // Create a new configuration dialog as a child of the main window
        auto *dialog = new QDialog(m_mainWindow->window());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(i18n("Configure Kate AI"));
        dialog->resize(760, 620);

        // Set up the dialog layout with configuration page and buttons
        auto *layout = new QVBoxLayout(dialog);
        auto *page = new KateAiConfigPage(dialog, m_plugin);
        // MCP servers are stored per workspace, so tell the page which one.
        page->setWorkspace(detectWorkspace(m_mainWindow));
        layout->addWidget(page);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, dialog);
        layout->addWidget(buttons);

        // Connect dialog buttons to their respective actions
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, page, &KateAiConfigPage::apply);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        connect(dialog, &QObject::destroyed, this, [this]() {
            m_configDialog = nullptr;
        });

        // Store reference to the dialog and show it
        m_configDialog = dialog;
        dialog->show();
    }

    void KateAiView::addEditorContextActions(KTextEditor::View *view, const QList<QAction *> &actions)
    {
        if (!view) {
            return;
        }
        connect(view, &KTextEditor::View::contextMenuAboutToShow, this, [actions](KTextEditor::View *, QMenu *menu) {
            if (!menu) {
                return;
            }
            // The actions live in a submenu, so checking menu->actions().contains(action)
            // always fails. Detect an existing Kate AI submenu instead (by objectName
            // or title) to avoid stacking duplicates on every contextMenuAboutToShow.
            static const QString menuObjectName = QStringLiteral("kateai_context_menu");
            const QString menuTitle = i18n("Kate AI");
            for (QAction *a : menu->actions()) {
                if (QMenu *sub = a->menu()) {
                    if (sub->objectName() == menuObjectName || a->text() == menuTitle) {
                        return;
                    }
                }
            }
            auto *aiMenu = menu->addMenu(menuTitle);
            aiMenu->setObjectName(menuObjectName);
            for (QAction *action : actions) {
                aiMenu->addAction(action);
            }
        });
    }

    QString KateAiView::editorContext() const
    {
        QStringList lines;
        auto *view = m_mainWindow->activeView();
        if (view && m_mainWindow) {
            const QString path = view->document()->url().toLocalFile();
            lines.append(u"Active file: %1"_s.arg(path.isEmpty() ? view->document()->documentName() : path));
            lines.append(u"Cursor: line %1"_s.arg(view->cursorPosition().line() + 1));
            if (view->selection()) {
                lines.append(u"Selection:\n%1"_s.arg(view->selectionText().left(4000)));
            }
        }
        QStringList openFiles;
        for (auto *v : m_mainWindow->views()) {
            const QString path = v->document()->url().toLocalFile();
            openFiles.append(path.isEmpty() ? v->document()->documentName() : path);
        }
        if (!openFiles.isEmpty()) {
            lines.append(u"Open files:\n- %1"_s.arg(openFiles.join(u"\n- "_s)));
        }
        return lines.join(u'\n');
    }

    void KateAiView::confirmWorkspaceMcpServers(const QString &workspace)
    {
        if (workspace.isEmpty() || !m_chat || !m_chat->agent()) {
            return;
        }
        McpManager *manager = m_chat->agent()->mcpManager();
        if (!manager) {
            return;
        }
        // The answer is remembered for the session: refreshWorkspace() runs on
        // every editor change, and the question would pop up again and again.
        const auto known = m_mcpTrust.constFind(workspace);
        if (known != m_mcpTrust.constEnd()) {
            manager->setLoadWorkspaceConfig(known.value());
            return;
        }

        const QList<McpServerConfig> servers = McpConfigStore::load(workspace);
        if (servers.isEmpty()) {
            m_mcpTrust.insert(workspace, true);
            manager->setLoadWorkspaceConfig(true);
            return;
        }

        // A project can ship a server list, and a server is a program that
        // runs with the user's privileges. The list is only used after the
        // user has seen it and agreed to it.
        QStringList names;
        for (const McpServerConfig &server : servers) {
            names.append(server.name);
        }
        const QString message = i18n(
            "The project %1 defines %n MCP server(s): %2.\n\n"
            "Starting a server runs a program on your machine. Only continue if you trust this project.",
            names.size(),
            names.join(u", "_s));
        QMessageBox box(QMessageBox::Question, i18n("MCP servers in this project"), message,
                        QMessageBox::Yes | QMessageBox::No, qobject_cast<QWidget *>(m_mainWindow));
        box.setDefaultButton(QMessageBox::No);
        box.setDetailedText(McpConfigStore::configPath(workspace));
        const bool trusted = box.exec() == QMessageBox::Yes;
        m_mcpTrust.insert(workspace, trusted);
        manager->setLoadWorkspaceConfig(trusted);
        if (!trusted) {
            // Nothing of the file is started, and the servers of the previous
            // workspace are stopped as well.
            manager->setServers({});
            manager->disconnectAll();
        }
    }

    void KateAiView::refreshWorkspace()
    {
        if (!m_mainWindow || !m_chat || !m_chat->agent()) {
            return;
        }
        const QString workspace = detectWorkspace(m_mainWindow);
        confirmWorkspaceMcpServers(workspace);
        m_chat->agent()->setWorkspace(workspace);
        m_chat->agent()->setDocumentBridge(&m_bridge);
        m_chat->agent()->setEditorContext(editorContext());
        updateCompletions();
    }

    void KateAiView::updateCompletions()
    {
        if (!m_chat || !m_mainWindow) {
            return;
        }
        QStringList words = {u"active"_s, u"selection"_s, u"workspace"_s};
        const QString workspace = detectWorkspace(m_mainWindow);

        for (auto *view : m_mainWindow->views()) {
            if (!view || !view->document()) {
                continue;
            }
            const QString fullPath = view->document()->url().toLocalFile();
            const QString docName = view->document()->documentName();
            if (!docName.isEmpty() && !words.contains(docName)) {
                words.append(docName);
            }
            if (!fullPath.isEmpty() && !workspace.isEmpty() && fullPath.startsWith(workspace)) {
                QString relPath = fullPath.mid(workspace.length());
                if (relPath.startsWith(u'/')) {
                    relPath = relPath.mid(1);
                }
                if (!relPath.isEmpty() && !words.contains(relPath)) {
                    words.append(relPath);
                }
            }
        }
        m_chat->setCompletionWords(words);
    }

} // namespace KateAi
