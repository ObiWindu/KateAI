/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "pluginview.h"
#include "chatwidget.h"
#include "edittracker.h"
#include "editorassist.h"
#include "mcp.h"
#include "chattheme.h"
#include "configpage.h"
#include "plugin.h"
#include "sessionstore.h"
#include "workspace.h"

#include <KActionCollection>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KSyntaxHighlighting/Theme>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/Message>
#include <KTextEditor/View>
#include <KXMLGUIFactory>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
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

        // The in-editor assist: AI completions at the cursor and hover hints
        // over agent-edited lines. One per window, attached to every editor
        // view of the window. Completions are opt-in through the settings.
        m_assist = new EditorAssist(this);
        m_assist->setSettings(plugin->settings());
        m_assist->setBusyCheck([this]() {
            return m_chat && m_chat->agent() && m_chat->agent()->isBusy();
        });

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
            // Edit-in-buffer mode follows the setting, both at startup and
            // on every later change through the config page.
            m_bridge.setEditInBuffer(plugin->settings().editInBuffer);
            if (m_chat->agent()) {
                m_chat->agent()->setDocumentBridge(&m_bridge);
                // Every chat needs an ID before its first turn. Otherwise the
                // turn-finished autosave is skipped, and switching to another
                // conversation can save the new messages under an empty ID.
                // The session config comes first: it knows which conversation
                // this window was showing when the session was saved, so each
                // window reopens its own chat instead of every window
                // claiming the same global active one. Load first:
                // SessionStore may repair a stale active pointer by falling
                // back to the newest persisted transcript.
                const QString claimedId = plugin->claimSessionConversation();
                const auto sessionData = claimedId.isEmpty()
                    ? SessionStore::load()
                    : SessionStore::loadConversation(claimedId);
                QString activeId = claimedId;
                if (activeId.isEmpty()) {
                    activeId = SessionStore::getActiveConversationId();
                    if (activeId.isEmpty()) {
                        activeId = SessionStore::createNewConversation();
                    }
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
        // The plugin remembers this view for the session config (its current
        // conversation, per window) and for the ":kateai" command.
        plugin->registerView(this);
        if (m_chat) {
            plugin->noteConversationChanged(this, m_chat->conversationId());
            connect(m_chat, &ChatWidget::conversationChanged, this, [this, plugin](const QString &conversationId) {
                plugin->noteConversationChanged(this, conversationId);
            });
        }
        refreshWorkspace();

        if (plugin && m_mainWindow) {
            connect(plugin, &KateAiPlugin::settingsChanged, this, [this](const Settings &settings) {
                ChatTheme::loadTheme(settings);
                applyWindowChrome();
                if (m_chat) {
                    m_chat->setSettings(settings);
                }
                if (m_assist) {
                    m_assist->setSettings(settings);
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
                // A new turn starts: edits the bridge left unsaved in earlier
                // turns are no longer "ours", and the agent must not build on
                // them until they were settled.
                m_bridge.resetBufferedEdits();
                // Completions stay off while the chat turn runs.
                if (m_assist) {
                    m_assist->setAgentBusy(true);
                }
                refreshWorkspace();
            });
            // The turn ends (finishes, fails or is stopped): completions and
            // their budget come back.
            if (m_chat->agent()) {
                connect(m_chat->agent(), &AgentLoop::turnFinished, this, [this]() {
                    if (m_assist) {
                        m_assist->setAgentBusy(false);
                    }
                });
            }
            // An agent edit of an open document shows where the file lives:
            // the in-editor banner points into the review queue.
            connect(m_chat, &ChatWidget::agentEditedFile, this, [this](const QString &path) {
                postEditBanner(path);
                syncEditRanges();
            });
            // When the queue settles, its banners have nothing left to point
            // at, so the next round may post fresh ones. The hover hints move
            // from "pending review" to "turn finished".
            connect(m_chat, &ChatWidget::pendingEditsChanged, this, [this](bool hasPending) {
                if (!hasPending) {
                    m_bannerPaths.clear();
                }
                syncEditRanges();
            });
        }
        // Buffer mode follows the setting on every later change too.
        connect(plugin, &KateAiPlugin::settingsChanged, this, [this](const Settings &settings) {
            m_bridge.setEditInBuffer(settings.editInBuffer);
        });
        if (m_mainWindow) {
            connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, [this](KTextEditor::View *) {
                refreshWorkspace();
            });
        }
        // Follow the document set: Save-As has to rename the review queue,
        // a close while the agent works needs a notice, and new documents
        // change the completion words.
        if (auto *editor = KTextEditor::Editor::instance()) {
            for (KTextEditor::Document *document : editor->documents()) {
                trackDocument(document);
            }
            connect(editor, &KTextEditor::Editor::documentCreated, this,
                    [this](KTextEditor::Editor *, KTextEditor::Document *document) {
                        trackDocument(document);
                    });
        }

        auto *ac = actionCollection();
        QPointer<QAction> toggle, fresh, ask, fix, refactor, tests, configure, stop, completeAction;

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

            // Stop has a shortcut of its own: the panel button is at the far
            // end of a scrolled transcript when a turn runs long.
            stop = ac->addAction(u"kateai_stop"_s);
            if (stop) {
                stop->setText(i18n("Stop Kate AI"));
                stop->setIcon(QIcon::fromTheme(u"process-stop"_s));
                connect(stop, &QAction::triggered, this, [this]() {
                    if (m_chat && m_chat->agent()) {
                        m_chat->agent()->abort();
                    }
                });
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

            // Manual completion trigger. The shortcut only reaches the panel's
            // window; the assist is per main window and asks its active view.
            completeAction = ac->addAction(u"kateai_complete"_s);
            if (completeAction) {
                completeAction->setText(i18n("Kate AI: Complete at Cursor"));
                KActionCollection::setDefaultShortcut(
                    completeAction, QKeySequence(Qt::ALT | Qt::Key_Backslash));
                connect(completeAction, &QAction::triggered, this, [this]() {
                    if (m_assist && m_mainWindow) {
                        m_assist->triggerCompletion(m_mainWindow->activeView());
                    }
                });
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

        // The assist follows the window's view set: existing views first, then
        // every view the editor creates later. KTextEditor has no
        // view-about-to-be-destroyed signal; the assist tracks the views with
        // QPointer and drops dead ones on its own.
        if (m_mainWindow && m_assist) {
            for (auto *view : m_mainWindow->views()) {
                m_assist->attachView(view);
            }
            connect(m_mainWindow, &KTextEditor::MainWindow::viewCreated, this,
                    [this](KTextEditor::View *view) {
                        if (m_assist) {
                            m_assist->attachView(view);
                        }
                    });
            // One-time notice the first time auto mode is about to fire: an
            // idle-triggered completion loop on a metered key is exactly the
            // cost surprise the user should hear about before it happens.
            connect(m_assist, &EditorAssist::autoModeFirstRun, this, [this]() {
                KConfigGroup g(KSharedConfig::openConfig(), u"KateAI"_s);
                if (g.readEntry(u"CompletionAutoNoticeShown"_s, false)) {
                    return;
                }
                g.writeEntry(u"CompletionAutoNoticeShown"_s, true);
                g.sync();
                QVariantMap message;
                message.insert(u"type"_s, u"Information"_s);
                message.insert(u"category"_s, i18n("Kate AI"));
                message.insert(u"categoryIcon"_s, QIcon::fromTheme(u"help-hint"_s));
                message.insert(u"text"_s, i18n("Automatic AI completions are on: Kate AI now requests short completions while you type, within the per-minute budget from its settings. Turn this off any time in the Kate AI configuration."));
                m_mainWindow->showMessage(message);
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
        if (m_plugin) {
            m_plugin->unregisterView(this);
        }

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
            if (path.isEmpty()) {
                // An untitled buffer has no path the tools could name, so
                // its content is the only way the model can see it at all.
                lines.append(u"Active file: (untitled) %1"_s.arg(view->document()->documentName()));
                const QString text = view->document()->text();
                if (!text.isEmpty()) {
                    lines.append(u"Unsaved buffer content:\n%1"_s.arg(text.left(2000)));
                }
            } else {
                lines.append(u"Active file: %1"_s.arg(path));
            }
            const KTextEditor::Cursor cursor = view->cursorPosition();
            lines.append(u"Cursor: line %1, column %2"_s.arg(cursor.line() + 1).arg(cursor.column() + 1));
            if (view->document()->isModified()) {
                lines.append(u"Active file has unsaved changes."_s);
            }
            const QString mode = view->document()->highlightingMode();
            if (!mode.isEmpty() && mode != u"None"_s) {
                lines.append(u"Language: %1"_s.arg(mode));
            }
            if (view->selection()) {
                // The range, not only the text: "where" is what the model
                // needs to point an edit at the same region.
                const KTextEditor::Range range = view->selectionRange();
                lines.append(u"Selection: lines %1-%2\n%3"_s
                                 .arg(range.start().line() + 1)
                                 .arg(range.end().line() + 1)
                                 .arg(view->selectionText().left(4000)));
            }
        }
        QStringList openFiles;
        QStringList bookmarks;
        for (auto *v : m_mainWindow ? m_mainWindow->views() : QList<KTextEditor::View *>()) {
            if (!v || !v->document()) {
                continue;
            }
            const QString path = v->document()->url().toLocalFile();
            const QString label = path.isEmpty()
                ? u"(untitled) %1"_s.arg(v->document()->documentName())
                : path;
            // The modified flag matters to the model: writing a modified
            // document is refused, so it should save or warn first.
            openFiles.append(v->document()->isModified() ? u"%1 [unsaved]"_s.arg(label) : label);
            // Bookmarks are the user's own "interesting lines" markers; the
            // agent should treat them with the same importance. KF6 exposes
            // marks directly on Document, no interface cast needed.
            const auto docMarks = v->document()->marks();
            for (auto markIt = docMarks.cbegin(); markIt != docMarks.cend(); ++markIt) {
                if (markIt.value()->type & KTextEditor::Document::Bookmark) {
                    bookmarks.append(u"%1:%2"_s.arg(label).arg(markIt.key() + 1));
                }
            }
        }
        if (!openFiles.isEmpty()) {
            lines.append(u"Open files:\n- %1"_s.arg(openFiles.join(u"\n- "_s)));
        }
        if (!bookmarks.isEmpty()) {
            // The list can grow with every open file; a compact cap keeps the
            // context budget for the files themselves.
            lines.append(u"Bookmarks:\n- %1"_s.arg(bookmarks.join(u"\n- "_s).left(1000)));
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

    bool KateAiView::ownsEditorView(KTextEditor::View *view) const
    {
        if (!m_mainWindow || !view) {
            return false;
        }
        return m_mainWindow->views().contains(view);
    }

    void KateAiView::trackDocument(KTextEditor::Document *document)
    {
        if (!document || m_docPaths.contains(document)) {
            return;
        }
        m_docPaths.insert(document, document->url().toLocalFile());
        // Save-As: the review queue and every in-flight edit of the turn have
        // to follow the file to its new path, or a later accept or reject
        // would settle a path that no longer exists.
        connect(document, &KTextEditor::Document::documentUrlChanged, this,
                [this, document](KTextEditor::Document *) {
                    const QString oldPath = m_docPaths.value(document);
                    const QString newPath = document->url().toLocalFile();
                    m_docPaths[document] = newPath;
                    if (!oldPath.isEmpty() && !newPath.isEmpty() && oldPath != newPath && m_chat) {
                        m_chat->renamePendingEdits(oldPath, newPath);
                    }
                    updateCompletions();
                });
        // A close while the agent is working on the file means the next
        // write_file would recreate it on disk, out of the editor. The
        // message view is where a background event like this belongs.
        connect(document, &KTextEditor::Document::aboutToClose, this, [this, document]() {
            const QString path = m_docPaths.take(document);
            m_bannerPaths.remove(path);
            if (!m_chat || !m_chat->agent() || path.isEmpty()) {
                return;
            }
            if (m_chat->agent()->isBusy() && m_mainWindow) {
                QVariantMap message;
                message.insert(u"type"_s, u"Warning"_s);
                message.insert(u"category"_s, i18n("Kate AI"));
                message.insert(u"categoryIcon"_s, QIcon::fromTheme(u"help-hint"_s));
                message.insert(u"text"_s, i18n("%1 was closed while the agent is working. Further edits to it are written to disk and reopen outside the editor.", QDir::toNativeSeparators(path)));
                m_mainWindow->showMessage(message);
            }
        });
        // The deletion of a document that was never closed still has to
        // drop the entry, or the hash would hold a dangling key. destroyed
        // fires while the pointer value is still meaningful, which is all
        // the hash removal needs.
        connect(document, &QObject::destroyed, this, [this, document]() {
            m_docPaths.remove(document);
        });
        updateCompletions();
    }

    void KateAiView::syncEditRanges()
    {
        if (!m_assist || !m_chat || !m_chat->editTracker()) {
            return;
        }
        EditTracker *tracker = m_chat->editTracker();
        for (const QString &path : tracker->rangedPaths()) {
            m_assist->updateEditRanges(path, tracker->pendingEditRanges(path),
                                       tracker->settledEditRanges(path));
        }
    }

    void KateAiView::postEditBanner(const QString &path)
    {
        if (path.isEmpty() || !m_plugin || !m_plugin->settings().editBannersEnabled) {
            return;
        }
        KTextEditor::Document *document = documentForPath(path);
        if (!document) {
            // Nothing to attach a banner to: the file is not open.
            return;
        }
        if (m_bannerPaths.contains(path)) {
            // One banner per file and review round, however many edits the
            // turn makes to it. The pending-edits-cleared signal resets the
            // set for the next round.
            return;
        }
        m_bannerPaths.insert(path);

        auto *message = new KTextEditor::Message(
            i18n("Kate AI changed this file. The change is queued for review in the Kate AI panel."),
            KTextEditor::Message::Information);
        message->setWordWrap(true);
        // The banner outlives the panel when its document is shared with
        // another window, so the actions go through a pointer that nulls out
        // with the view instead of capturing this directly.
        QPointer<KateAiView> self = this;
        auto *review = new QAction(i18n("Review"), message);
        connect(review, &QAction::triggered, this, [self]() {
            if (self) {
                self->showPanel();
            }
        });
        auto *reject = new QAction(i18n("Reject Changes"), message);
        connect(reject, &QAction::triggered, this, [self, path]() {
            if (self && self->m_chat) {
                self->m_chat->rejectPendingEdits(path);
            }
        });
        message->addAction(review);
        message->addAction(reject);
        document->postMessage(message);
    }

} // namespace KateAi
