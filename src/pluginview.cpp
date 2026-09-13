#include "pluginview.h"
#include "chatwidget.h"
#include "configpage.h"
#include "plugin.h"
#include "workspace.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KTextEditor/Document>
#include <KTextEditor/View>
#include <KXMLGUIFactory>

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QIcon>
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
            }
        }
        refreshWorkspace();

        if (plugin && m_mainWindow) {
            connect(plugin, &KateAiPlugin::settingsChanged, this, [this](const Settings &settings) {
                if (m_chat) {
                    m_chat->setSettings(settings);
                }
                refreshWorkspace();
            });
        }
        if (m_chat) {
            connect(m_chat, &ChatWidget::settingsChanged, plugin, &KateAiPlugin::setSettings);
            connect(m_chat, &ChatWidget::configureRequested, this, &KateAiView::showConfiguration);
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
        if (m_mainWindow && m_mainWindow->guiFactory()) {
            m_mainWindow->guiFactory()->removeClient(this);
        }
        delete m_toolView;
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

    void KateAiView::refreshWorkspace()
    {
        if (!m_mainWindow || !m_chat || !m_chat->agent()) {
            return;
        }
        const QString workspace = detectWorkspace(m_mainWindow);
        m_chat->agent()->setWorkspace(workspace);
        m_chat->agent()->setDocumentBridge(&m_bridge);
        m_chat->agent()->setEditorContext(editorContext());
    }

} // namespace KateAi
