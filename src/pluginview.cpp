/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "pluginview.h"
#include "chatwidget.h"
#include "configpage.h"
#include "plugin.h"
#include "sessionstore.h"
#include "workspace.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KTextEditor/Document>
#include <KTextEditor/View>
#include <KXMLGUIFactory>

#include <QAction>
#include <QDialog>
#include <QDir>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileInfo>
#include <QIcon>
#include <QSet>
#include <QTimer>
#include <QKeySequence>
#include <QLayout>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>


using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

    KateAiView::KateAiView(KateAiPlugin *plugin, KTextEditor::MainWindow *mainWindow)
    : QObject(plugin)
    , m_plugin(plugin)
    , m_mainWindow(mainWindow)
    {
        // Keep plugin-view construction side-effect free. Kate may construct
        // plugin views while it is still restoring its GUI. XMLGUI merging,
        // action creation, view enumeration, and context-menu wiring are all
        // deferred until the event loop is running.
        QTimer::singleShot(0, this, [this]() {
            initializeGui();
        });
    }

    void KateAiView::initializeGui()
    {
        if (m_guiInitialized || !m_mainWindow || !m_plugin) {
            return;
        }
        m_guiInitialized = true;

        setComponentName(u"kateai"_s, i18n("Kate AI"));
        setXMLFile(u"ui.rc"_s);

        if (m_plugin) {
            connect(m_plugin, &KateAiPlugin::settingsChanged, this, [this](const Settings &settings) {
                if (m_chat) {
                    m_chat->setSettings(settings);
                }
                refreshWorkspace();
            });
        }
        connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, [this](KTextEditor::View *) {
            refreshWorkspace();
        });

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

        for (auto *view : m_mainWindow->views()) {
            addEditorContextActions(view, {ask, fix, refactor, tests});
        }
        connect(m_mainWindow, &KTextEditor::MainWindow::viewCreated, this, [this, ask, fix, refactor, tests](KTextEditor::View *view) {
            addEditorContextActions(view, {ask, fix, refactor, tests});
            QTimer::singleShot(0, this, [this]() {
                if (m_toolView && m_toolView->isVisible()) {
                    updateCompletions();
                }
            });
        });

        if (m_mainWindow->guiFactory()) {
            m_mainWindow->guiFactory()->addClient(this);
            m_guiClientRegistered = true;
        }
    }

    KateAiView::~KateAiView()
    {
        // Session is saved in ChatWidget destructor before AgentLoop is destroyed
        // m_toolView is owned by the main window, do not delete it here

        if (m_guiClientRegistered && m_mainWindow && m_mainWindow->guiFactory()) {
            m_mainWindow->guiFactory()->removeClient(this);
        }
    }

    void KateAiView::ensureUiCreated()
    {
        if (m_uiInitialized || !m_mainWindow || !m_plugin) {
            return;
        }

        m_uiInitialized = true;

        m_toolView = m_mainWindow->createToolView(m_plugin,
                                                  u"kateai"_s,
                                                  KTextEditor::MainWindow::Right,
                                                  QIcon::fromTheme(u"help-hint"_s),
                                                  i18n("Kate AI"));
        if (!m_toolView) {
            m_uiInitialized = false;
            return;
        }

        m_chat = new ChatWidget(m_toolView);
        if (!m_chat) {
            m_uiInitialized = false;
            return;
        }

        if (!m_toolView->layout()) {
            auto *layout = new QVBoxLayout(m_toolView);
            layout->setContentsMargins(0, 0, 0, 0);
        }
        m_toolView->layout()->addWidget(m_chat);

        // These connections used to be made in the eager constructor. Keep
        // them here because ChatWidget itself is now lazy-created.
        connect(m_chat, &ChatWidget::settingsChanged, m_plugin, &KateAiPlugin::setSettings);
        connect(m_chat, &ChatWidget::configureRequested, this, &KateAiView::showConfiguration);
        connect(m_chat, &ChatWidget::aboutToSubmit, this, [this]() {
            if (m_mainWindow && m_chat && m_chat->agent()) {
                // Submission gets a fresh editor snapshot, but does not start
                // a project scan merely because the user pressed Send.
                m_chat->agent()->setEditorContext(editorContext());
            }
        });

        m_chat->setSettings(m_plugin->settings());
        if (m_chat->agent()) {
            const auto sessionData = SessionStore::load();
            if (!sessionData.messages.isEmpty()) {
                m_chat->agent()->restoreSession(sessionData);
                m_chat->rebuildTranscript();
            }
            m_chat->agent()->setEditorContext(editorContext());
            const QString workspace = detectWorkspace(m_mainWindow);
            if (!workspace.isEmpty()) {
                m_chat->agent()->setWorkspace(workspace);
            }
        }

        updateCompletions();
    }

    void KateAiView::showPanel()
    {
        initializeGui();
        ensureUiCreated();
        if (m_toolView && m_mainWindow) {
            m_mainWindow->showToolView(m_toolView);
            if (m_chat) {
                if (m_chat->agent()) {
                    m_chat->agent()->setEditorContext(editorContext());
                    const QString workspace = detectWorkspace(m_mainWindow);
                    if (!workspace.isEmpty()) {
                        m_chat->agent()->setWorkspace(workspace);
                    }
                }
                updateCompletions();
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
        dialog->resize(640, 520);

        // Set up the dialog layout with configuration page and buttons
        auto *layout = new QVBoxLayout(dialog);
        auto *page = new KateAiConfigPage(dialog, m_plugin);
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
        if (!m_mainWindow) {
            return {};
        }
        auto *view = m_mainWindow->activeView();
        if (view) {
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

    void KateAiView::refreshWorkspace()
    {
        if (!m_mainWindow || !m_chat || !m_chat->agent()) {
            return;
        }
        const QString workspace = detectWorkspace(m_mainWindow);
        if (!workspace.isEmpty()) {
            m_chat->agent()->setWorkspace(workspace);
        } else {
            m_chat->agent()->setWorkspace({});
        }
        m_chat->agent()->setEditorContext(editorContext());
        if (m_toolView && m_toolView->isVisible()) {
            updateCompletions();
        }
    }

    void KateAiView::updateCompletions()
    {
        if (!m_chat || !m_mainWindow) {
            return;
        }

        QStringList words = {u"active"_s, u"selection"_s, u"workspace"_s};
        QSet<QString> seen;
        for (const QString &word : words) {
            seen.insert(word);
        }
        const QString workspace = QDir::cleanPath(detectWorkspace(m_mainWindow));
        const QDir workspaceDir(workspace);

        auto appendWord = [&words, &seen](const QString &word) {
            const QString normalized = word.trimmed();
            if (normalized.isEmpty() || seen.contains(normalized)) {
                return;
            }
            seen.insert(normalized);
            words.append(normalized);
        };

        // Always include all currently open documents.
        for (auto *view : m_mainWindow->views()) {
            if (!view || !view->document()) {
                continue;
            }
            const QString fullPath = QDir::cleanPath(view->document()->url().toLocalFile());
            appendWord(view->document()->documentName());
            if (!fullPath.isEmpty() && fullPath != u"."_s && !workspace.isEmpty()) {
                const QString relative = workspaceDir.relativeFilePath(fullPath);
                if (relative != u"."_s && !relative.startsWith(u"../"_s) && relative != u".."_s) {
                    appendWord(relative);
                }
            }
        }

        // Add a bounded, source-oriented workspace index. This keeps @mention
        // useful for files that are not currently open without making typing
        // block on very large generated trees.
        if (!workspace.isEmpty() && QFileInfo::exists(workspace) && words.size() < 600) {
            static const QSet<QString> ignoredDirs = {
                u".git"_s, u".hg"_s, u".svn"_s, u"build"_s, u"node_modules"_s, u".cache"_s, u"dist"_s
            };
            static const QSet<QString> sourceSuffixes = {
                u"c"_s, u"cc"_s, u"cpp"_s, u"cxx"_s, u"h"_s, u"hh"_s, u"hpp"_s, u"hxx"_s,
                u"py"_s, u"js"_s, u"ts"_s, u"tsx"_s, u"jsx"_s, u"rs"_s, u"go"_s, u"java"_s,
                u"kt"_s, u"kts"_s, u"swift"_s, u"rb"_s, u"php"_s, u"qml"_s, u"ui"_s,
                u"json"_s, u"yaml"_s, u"yml"_s, u"toml"_s, u"md"_s, u"txt"_s, u"cmake"_s
            };

            QDirIterator it(workspace, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            constexpr int kMaxWorkspaceFiles = 500;
            int added = 0;
            while (it.hasNext() && added < kMaxWorkspaceFiles && words.size() < 600) {
                const QString path = it.next();
                const QFileInfo info(path);
                const QStringList parts = info.absolutePath().mid(workspace.size()).split(u'/', Qt::SkipEmptyParts);
                bool ignored = false;
                for (const QString &part : parts) {
                    if (ignoredDirs.contains(part) || part.startsWith(u'.')) {
                        ignored = true;
                        break;
                    }
                }
                if (ignored) {
                    continue;
                }

                const QString fileName = info.fileName();
                if (!sourceSuffixes.contains(info.suffix().toLower()) && fileName != u"CMakeLists.txt"_s) {
                    continue;
                }
                QString relative = QDir(workspace).relativeFilePath(path);
                if (relative.startsWith(u"./"_s)) {
                    relative.remove(0, 2);
                }
                appendWord(relative);
                ++added;
            }
        }

        m_chat->setCompletionWords(words);
    }

} // namespace KateAi
