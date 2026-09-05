#include "pluginview.h"
#include "chatwidget.h"
#include "plugin.h"
#include "workspace.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KTextEditor/Document>
#include <KTextEditor/View>
#include <KXMLGUIFactory>

#include <QAction>
#include <QIcon>
#include <QKeySequence>
#include <QLayout>
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
    const QString prompt = i18n("Explain this code from %1:\n\n%2", path.isEmpty() ? view->document()->documentName() : path, snippet);
    m_chat->ask(prompt);
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
