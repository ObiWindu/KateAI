/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "katelspbridge.h"

#include <KTextEditor/Application>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>
#include <KTextEditor/Plugin>

#include <QDir>
#include <QFileInfo>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// The plugin id Kate's LSP client registers under, and the names its
// per-window view is created with.
constexpr auto kLspPluginName = "katelspclientplugin";

// How often open documents are re-scanned for diagnostic marks, so
// diagnosticsChanged fires without every query paying for a full scan.
constexpr int kPollIntervalMs = 2000;

// Ring buffer size for LSP window/show messages.
constexpr int kMaxMessages = 50;

QString canonicalPath(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : canonical;
}

} // namespace

KateLspBridge::KateLspBridge(QObject *parent)
    : LspProvider(parent)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &KateLspBridge::pollOpenDocuments);
    m_pollTimer->start();

    probeLspPlugin();

    // The LSP plugin can load after this bridge: re-probe when it appears.
    if (auto *editor = KTextEditor::Editor::instance()) {
        if (auto *app = editor->application()) {
            connect(app, &KTextEditor::Application::pluginCreated, this,
                    [this](const QString &name, KTextEditor::Plugin *plugin) {
                        onPluginCreated(name, plugin);
                    });
            connect(app, &KTextEditor::Application::pluginDeleted, this,
                    [this](const QString &name, KTextEditor::Plugin *plugin) {
                        onPluginDeleted(name, plugin);
                    });
        }
        for (KTextEditor::Document *document : editor->documents()) {
            trackDocument(document);
        }
        connect(editor, &KTextEditor::Editor::documentCreated, this,
                [this](KTextEditor::Editor *, KTextEditor::Document *document) {
                    trackDocument(document);
                });
    }
}

KateLspBridge::~KateLspBridge() = default;

void KateLspBridge::setMainWindow(KTextEditor::MainWindow *mainWindow)
{
    if (m_mainWindow == mainWindow) {
        return;
    }
    m_mainWindow = mainWindow;
    if (m_mainWindow) {
        connect(m_mainWindow, &KTextEditor::MainWindow::pluginViewCreated, this,
                [this](const QString &name, QObject *pluginView) {
                    onPluginViewCreated(name, pluginView);
                },
                Qt::UniqueConnection);
    }
    probeLspPlugin();
}

void KateLspBridge::probeLspPlugin()
{
    QObject *plugin = nullptr;
    if (auto *editor = KTextEditor::Editor::instance()) {
        if (auto *app = editor->application()) {
            plugin = app->plugin(u"%1"_s.arg(QLatin1String(kLspPluginName)));
        }
    }
    if (plugin == m_lspPlugin) {
        return;
    }
    m_lspPlugin = plugin;
    if (m_lspPlugin) {
        // LSPClientPlugin::showMessage is a public signal of the plugin
        // object; connecting by signature keeps working across Kate versions
        // that keep the signal and drops out harmlessly when it changes.
        connect(m_lspPlugin, SIGNAL(showMessage(KTextEditor::Message::MessageType, QString)), this,
                SLOT(onLspMessage(KTextEditor::Message::MessageType, QString)), Qt::UniqueConnection);
        // update() fires when the plugin's settings change, which is the
        // closest public signal to a state change.
        connect(m_lspPlugin, SIGNAL(update()), this, SIGNAL(availabilityChanged()), Qt::UniqueConnection);
    }
    Q_EMIT availabilityChanged();
}

void KateLspBridge::onPluginCreated(const QString &name, QObject *plugin)
{
    Q_UNUSED(plugin);
    if (name == QLatin1String(kLspPluginName)) {
        probeLspPlugin();
    }
}

void KateLspBridge::onPluginDeleted(const QString &name, QObject *plugin)
{
    Q_UNUSED(plugin);
    if (name == QLatin1String(kLspPluginName)) {
        m_lspPlugin = nullptr;
        Q_EMIT availabilityChanged();
    }
}

void KateLspBridge::onPluginViewCreated(const QString &name, QObject *pluginView)
{
    // The per-window view of the LSP plugin carries the document-level state.
    // Today the public surface worth watching is covered by the plugin object
    // and the document marks; when Kate exposes more, this is where it lands.
    Q_UNUSED(name);
    Q_UNUSED(pluginView);
}

void KateLspBridge::onLspMessage(KTextEditor::Message::MessageType level, const QString &msg)
{
    QString prefix;
    switch (level) {
    case KTextEditor::Message::Error:
        prefix = u"[LSP error] "_s;
        break;
    case KTextEditor::Message::Warning:
        prefix = u"[LSP warning] "_s;
        break;
    default:
        prefix = u"[LSP] "_s;
        break;
    }
    m_messages.append(prefix + msg);
    while (m_messages.size() > kMaxMessages) {
        m_messages.removeFirst();
    }
}

void KateLspBridge::trackDocument(KTextEditor::Document *document)
{
    if (!document || m_docPaths.contains(document)) {
        return;
    }
    const QString path = document->url().toLocalFile();
    if (path.isEmpty()) {
        return;
    }
    m_docPaths.insert(document, canonicalPath(path));
    connect(document, &KTextEditor::Document::marksChanged, this, [this, document](KTextEditor::Document *) {
        refreshDocumentMarks(document);
    });
    connect(document, &QObject::destroyed, this, [this, document]() {
        m_docPaths.remove(document);
    });
    refreshDocumentMarks(document);
}

// Reads the Error/Warning marks of one document into diagnostics. Marks carry
// the line and the severity class but no per-line text in the public API, so
// the message is the mark-type description plus the line.
void KateLspBridge::refreshDocumentMarks(KTextEditor::Document *document)
{
    if (!document) {
        return;
    }
    const QString path = m_docPaths.value(document);
    if (path.isEmpty()) {
        return;
    }
    int errors = 0;
    int warnings = 0;
    const auto marks = document->marks();
    for (auto it = marks.cbegin(); it != marks.cend(); ++it) {
        const uint type = it.value()->type;
        if (type & KTextEditor::Document::Error) {
            ++errors;
        } else if (type & KTextEditor::Document::Warning) {
            ++warnings;
        }
    }
    const QPair<int, int> counts{errors, warnings};
    if (m_markCounts.value(path) == counts) {
        return;
    }
    m_markCounts[path] = counts;
    Q_EMIT diagnosticsChanged(path);
}

void KateLspBridge::pollOpenDocuments()
{
    if (auto *editor = KTextEditor::Editor::instance()) {
        for (KTextEditor::Document *document : editor->documents()) {
            trackDocument(document);
            refreshDocumentMarks(document);
        }
    }
}

bool KateLspBridge::isAvailable() const
{
    return m_lspPlugin != nullptr;
}

QString KateLspBridge::serverState(const QString &path) const
{
    Q_UNUSED(path);
    // The plugin object being present means the LSP client is loaded; whether
    // a server is running for one file's language is not exposed publicly.
    return isAvailable() ? u"loaded"_s : u"none"_s;
}

QList<LspDiagnostic> KateLspBridge::diagnostics(const QString &path) const
{
    QList<LspDiagnostic> out;
    const QString wanted = path.isEmpty() ? QString() : canonicalPath(path);

    auto collect = [&](KTextEditor::Document *document) {
        const QString docPath = canonicalPath(document->url().toLocalFile());
        if (!wanted.isEmpty() && docPath != wanted) {
            return;
        }
        const auto marks = document->marks();
        for (auto it = marks.cbegin(); it != marks.cend(); ++it) {
            const uint type = it.value()->type;
            LspDiagnostic diag;
            diag.path = document->url().toLocalFile();
            diag.line = it.key() + 1; // marks are keyed by 0-based line
            diag.column = 1;
            if (type & KTextEditor::Document::Error) {
                diag.severity = 1;
                diag.message = document->markDescription(KTextEditor::Document::Error);
            } else if (type & KTextEditor::Document::Warning) {
                diag.severity = 2;
                diag.message = document->markDescription(KTextEditor::Document::Warning);
            } else {
                continue;
            }
            if (diag.message.isEmpty()) {
                diag.message = diag.severity == 1 ? u"error"_s : u"warning"_s;
            }
            diag.source = u"marks"_s;
            out.append(diag);
        }
    };

    if (auto *editor = KTextEditor::Editor::instance()) {
        for (KTextEditor::Document *document : editor->documents()) {
            if (document->url().isLocalFile()) {
                collect(document);
            }
        }
    }
    // A file that is not open has no live marks; the cached counts still say
    // whether it had problems when it was last open.
    if (!wanted.isEmpty() && out.isEmpty()) {
        const auto cached = m_markCounts.value(wanted);
        if (cached.first > 0 || cached.second > 0) {
            LspDiagnostic diag;
            diag.path = wanted;
            diag.severity = cached.first > 0 ? 1 : 2;
            diag.message = u"%1 error(s), %2 warning(s) when last open (file is not open now)"_s.arg(cached.first).arg(cached.second);
            diag.source = u"marks-cache"_s;
            out.append(diag);
        }
    }
    return out;
}

QStringList KateLspBridge::recentMessages(int maxCount) const
{
    if (maxCount <= 0 || m_messages.isEmpty()) {
        return m_messages;
    }
    return m_messages.mid(qMax(0, m_messages.size() - maxCount));
}

} // namespace KateAi

#include "katelspbridge.moc"
