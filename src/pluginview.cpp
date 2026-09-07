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
    setComponentName(u"kateai"_s, i18n("Kate AI"));
    setXMLFile(u"ui.rc"_s);

    m_toolView = m_mainWindow->createToolView(plugin,
                                              u"kateai"_s,
                                              KTextEditor::MainWindow::Right,
                                              QIcon::fromTheme(u"help-hint"_s),
                                              i18n("Kate AI"));
    m_chat = new ChatWidget(m_toolView);
    if (!m_toolView->layout()) {
        auto *layout = new QVBoxLayout(m_toolView);
        layout->setContentsMargins(0, 0, 0, 0);
    }
    m_toolView->layout()->addWidget(m_chat);

    m_chat->setSettings(plugin->settings());
    m_chat->agent()->setDocumentBridge(&m_bridge);
    refreshWorkspace();

    connect(plugin, &KateAiPlugin::settingsChanged, this, [this](const Settings &settings) {
        m_chat->setSettings(settings);
        refreshWorkspace();
    });
    connect(m_chat, &ChatWidget::settingsChanged, plugin, &KateAiPlugin::setSettings);
    connect(m_chat, &ChatWidget::configureRequested, this, &KateAiView::showConfiguration);
    connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, [this](KTextEditor::View *) {
        refreshWorkspace();
    });

    auto *ac = actionCollection();
    auto *toggle = ac->addAction(u"kateai_toggle"_s);
    toggle->setText(i18n("Show Kate AI"));
    toggle->setIcon(QIcon::fromTheme(u"help-hint"_s));
    KActionCollection::setDefaultShortcut(toggle, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A));
    connect(toggle, &QAction::triggered, this, &KateAiView::showPanel);

    auto *fresh = ac->addAction(u"kateai_new_chat"_s);
    fresh->setText(i18n("Kate AI: New Chat"));
    KActionCollection::setDefaultShortcut(fresh, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N));
    connect(fresh, &QAction::triggered, this, &KateAiView::newChat);

    auto *ask = ac->addAction(u"kateai_ask_selection"_s);
    ask->setText(i18n("Ask Kate AI About This"));
    connect(ask, &QAction::triggered, this, &KateAiView::askSelection);

    auto *fix = ac->addAction(u"kateai_fix_selection"_s);
    fix->setText(i18n("Fix Selection"));
    connect(fix, &QAction::triggered, this, [this]() {
        askSelectionWithInstruction(i18n("Find and fix problems in this code. Explain the change, then apply it."));
    });

    auto *refactor = ac->addAction(u"kateai_refactor_selection"_s);
    refactor->setText(i18n("Refactor Selection"));
    connect(refactor, &QAction::triggered, this, [this]() {
        askSelectionWithInstruction(i18n("Refactor this code for clarity and maintainability. Explain the change, then apply it."));
    });

    auto *tests = ac->addAction(u"kateai_test_selection"_s);
    tests->setText(i18n("Add Tests for Selection"));
    connect(tests, &QAction::triggered, this, [this]() {
        askSelectionWithInstruction(i18n("Add or improve focused tests for this code. Explain the proposed coverage first."));
    });

    auto *configure = ac->addAction(u"kateai_configure"_s);
    configure->setText(i18n("Configure Kate AI…"));
    configure->setIcon(QIcon::fromTheme(u"settings-configure"_s));
    connect(configure, &QAction::triggered, this, &KateAiView::showConfiguration);

    // KTextEditor does not merge plugin XML clients into a view whose context
    // menu was supplied by another plugin. Add this action at show time so it
    // is consistently available in every editor tab.
    for (auto *view : m_mainWindow->views()) {
        addEditorContextActions(view, {ask, fix, refactor, tests});
    }
    connect(m_mainWindow, &KTextEditor::MainWindow::viewCreated, this, [this, ask, fix, refactor, tests](KTextEditor::View *view) {
        addEditorContextActions(view, {ask, fix, refactor, tests});
    });

    m_mainWindow->guiFactory()->addClient(this);
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
    if (m_toolView) {
        m_mainWindow->showToolView(m_toolView);
        m_chat->focusPrompt();
    }
}

void KateAiView::newChat()
{
    showPanel();
    m_chat->newChat();
}

void KateAiView::askSelection()
{
    askSelectionWithInstruction(i18n("Explain this code."));
}

void KateAiView::askSelectionWithInstruction(const QString &instruction)
{
    showPanel();
    auto *view = m_mainWindow->activeView();
    if (!view) {
        return;
    }
    QString snippet = view->selectionText();
    if (snippet.isEmpty()) {
        snippet = view->document()->line(view->cursorPosition().line());
    }
    const QString path = view->document()->url().toLocalFile();
    const QString prompt = i18n("%1\n\nFile: %2\n\nCode:\n%3",
                                instruction,
                                path.isEmpty() ? view->document()->documentName() : path,
                                snippet);
    m_chat->ask(prompt);
}

void KateAiView::showConfiguration()
{
    if (m_configDialog) {
        m_configDialog->raise();
        m_configDialog->activateWindow();
        return;
    }

    auto *dialog = new QDialog(m_mainWindow->window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(i18n("Configure Kate AI"));
    dialog->resize(640, 520);

    auto *layout = new QVBoxLayout(dialog);
    auto *page = new KateAiConfigPage(dialog, m_plugin);
    layout->addWidget(page);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, dialog);
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, page, &KateAiConfigPage::apply);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(dialog, &QObject::destroyed, this, [this]() {
        m_configDialog = nullptr;
    });

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
        for (QAction *action : actions) {
            if (menu->actions().contains(action)) {
                return;
            }
        }
        auto *aiMenu = menu->addMenu(i18n("Kate AI"));
        for (QAction *action : actions) {
            aiMenu->addAction(action);
        }
    });
}

QString KateAiView::editorContext() const
{
    QStringList lines;
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
    const QString workspace = detectWorkspace(m_mainWindow);
    m_chat->agent()->setWorkspace(workspace);
    m_chat->agent()->setDocumentBridge(&m_bridge);
    m_chat->agent()->setEditorContext(editorContext());
}

} // namespace KateAi
