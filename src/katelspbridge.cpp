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
#include <KTextEditor/Range>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// Kate's LSP client plugin registers as "lspclientplugin" (matching
// lspclientplugin.so; "katelspclientplugin" is its Qt logging category).
// Keep the legacy name as a secondary probe so custom builds still match.
constexpr auto kLspPluginName = "lspclientplugin";
constexpr auto kLegacyLspPluginName = "katelspclientplugin";

// How often open documents are re-scanned for diagnostic marks and late
// DiagnosticsView creation, so diagnosticsChanged fires reliably.
constexpr int kPollIntervalMs = 2000;

// Ring buffer size for LSP window/show messages.
constexpr int kMaxMessages = 50;

QString canonicalPath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : canonical;
}

bool isLspPluginId(const QString &name)
{
    return name == QLatin1String(kLspPluginName) || name == QLatin1String(kLegacyLspPluginName);
}

int severityFromIconName(const QString &iconName)
{
    const QString lower = iconName.toLower();
    if (lower.contains(u"error"_s)) {
        return 1;
    }
    if (lower.contains(u"warning"_s)) {
        return 2;
    }
    if (lower.contains(u"info"_s)) {
        return 3;
    }
    if (lower.contains(u"hint"_s)) {
        return 4;
    }
    return 0;
}

int severityFromModelItem(const QAbstractItemModel *model, const QModelIndex &idx)
{
    // Kate's DiagnosticSeverity enum matches the LSP scale (1=Error, 2=Warning,
    // 3=Information, 4=Hint) and is stored in DiagnosticModelRole::KindRole
    // (UserRole + 2). Probe UserRole+2 first, then fall back to the icon name.
    const QVariant kindVar = model->data(idx, Qt::UserRole + 2);
    if (kindVar.isValid()) {
        bool ok = false;
        const int val = kindVar.toInt(&ok);
        if (ok && val >= 1 && val <= 4) {
            return val;
        }
    }
    const QVariant deco = model->data(idx, Qt::DecorationRole);
    if (deco.canConvert<QIcon>()) {
        const int byIcon = severityFromIconName(deco.value<QIcon>().name());
        if (byIcon > 0) {
            return byIcon;
        }
    }
    return 2; // default to warning when unknown
}

KTextEditor::Range rangeFromModelItem(const QAbstractItemModel *model, const QModelIndex &idx)
{
    // DiagnosticModelRole::RangeRole is Qt::UserRole + 1 in Kate; probe a small
    // window of user roles so a future shift in Kate's enum keeps working.
    for (int role = Qt::UserRole + 1; role <= Qt::UserRole + 5; ++role) {
        const QVariant v = model->data(idx, role);
        if (v.isValid() && v.canConvert<KTextEditor::Range>()) {
            const KTextEditor::Range r = v.value<KTextEditor::Range>();
            if (r.isValid()) {
                return r;
            }
        }
    }
    return KTextEditor::Range::invalid();
}

void parseSourceCodeMessage(const QString &rawText, QString *source, QString *message)
{
    // Kate's DiagnosticsView formats rows as:
    //   "[source] (code) message"  or  "[source] message"  or  "(code) message"
    static const QRegularExpression prefixRe(
        QStringLiteral("^\\s*(?:\\[([^\\]]+)\\]\\s*)?(?:\\(([^)]+)\\)\\s*)?(.*)$"));
    const QRegularExpressionMatch m = prefixRe.match(rawText);
    if (!m.hasMatch()) {
        *message = rawText.trimmed();
        return;
    }
    const QString src = m.captured(1).trimmed();
    const QString code = m.captured(2).trimmed();
    QString msg = m.captured(3).trimmed();
    if (!src.isEmpty()) {
        *source = src;
    }
    if (!code.isEmpty()) {
        msg = msg.isEmpty() ? u"(%1)"_s.arg(code) : u"(%1) %2"_s.arg(code, msg);
    }
    *message = msg.isEmpty() ? rawText.trimmed() : msg;
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
    m_diagnosticsModel = nullptr;
    if (m_mainWindow) {
        connect(m_mainWindow, &KTextEditor::MainWindow::pluginViewCreated, this,
                [this](const QString &name, QObject *pluginView) {
                    onPluginViewCreated(name, pluginView);
                },
                Qt::UniqueConnection);
    }
    probeLspPlugin();
    probeDiagnosticsView();
}

void KateLspBridge::probeLspPlugin()
{
    QObject *plugin = nullptr;
    if (auto *editor = KTextEditor::Editor::instance()) {
        if (auto *app = editor->application()) {
            plugin = app->plugin(QLatin1String(kLspPluginName));
            if (!plugin) {
                plugin = app->plugin(QLatin1String(kLegacyLspPluginName));
            }
        }
    }
    if (!plugin && m_mainWindow) {
        plugin = m_mainWindow->pluginView(QLatin1String(kLspPluginName));
        if (!plugin) {
            plugin = m_mainWindow->pluginView(QLatin1String(kLegacyLspPluginName));
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

void KateLspBridge::probeDiagnosticsView()
{
    if (!m_mainWindow || m_diagnosticsModel) {
        return;
    }
    QWidget *win = m_mainWindow->window();
    if (!win) {
        return;
    }
    const QList<QWidget *> widgets = win->findChildren<QWidget *>();
    for (QWidget *w : widgets) {
        if (!w || QByteArrayView(w->metaObject()->className()) != "DiagnosticsView") {
            continue;
        }
        auto *tree = w->findChild<QTreeView *>();
        if (!tree || !tree->model()) {
            continue;
        }
        QAbstractItemModel *model = tree->model();
        while (auto *proxy = qobject_cast<QAbstractProxyModel *>(model)) {
            if (!proxy->sourceModel()) {
                break;
            }
            model = proxy->sourceModel();
        }
        m_diagnosticsModel = model;
        connect(model, &QAbstractItemModel::rowsInserted, this, &KateLspBridge::onDiagnosticsModelChanged,
                Qt::UniqueConnection);
        connect(model, &QAbstractItemModel::rowsRemoved, this, &KateLspBridge::onDiagnosticsModelChanged,
                Qt::UniqueConnection);
        connect(model, &QAbstractItemModel::dataChanged, this, &KateLspBridge::onDiagnosticsModelChanged,
                Qt::UniqueConnection);
        connect(model, &QAbstractItemModel::modelReset, this, &KateLspBridge::onDiagnosticsModelChanged,
                Qt::UniqueConnection);
        Q_EMIT diagnosticsChanged(QString());
        return;
    }
}

const QAbstractItemModel *KateLspBridge::diagnosticsSourceModel() const
{
    if (m_diagnosticsModel) {
        return m_diagnosticsModel.data();
    }
    return nullptr;
}

void KateLspBridge::onDiagnosticsModelChanged()
{
    Q_EMIT diagnosticsChanged(QString());
}

void KateLspBridge::onPluginCreated(const QString &name, QObject *plugin)
{
    Q_UNUSED(plugin);
    if (isLspPluginId(name)) {
        probeLspPlugin();
    }
}

void KateLspBridge::onPluginDeleted(const QString &name, QObject *plugin)
{
    Q_UNUSED(plugin);
    if (isLspPluginId(name)) {
        m_lspPlugin = nullptr;
        Q_EMIT availabilityChanged();
    }
}

void KateLspBridge::onPluginViewCreated(const QString &name, QObject *pluginView)
{
    Q_UNUSED(pluginView);
    if (isLspPluginId(name)) {
        probeLspPlugin();
    }
    probeDiagnosticsView();
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
// they act as a fallback when Kate's DiagnosticsView model is unavailable.
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
    if (!m_diagnosticsModel) {
        probeDiagnosticsView();
    }
    if (!m_lspPlugin) {
        probeLspPlugin();
    }
    if (auto *editor = KTextEditor::Editor::instance()) {
        for (KTextEditor::Document *document : editor->documents()) {
            trackDocument(document);
            refreshDocumentMarks(document);
        }
    }
}

bool KateLspBridge::isAvailable() const
{
    return m_lspPlugin != nullptr || m_diagnosticsModel != nullptr;
}

QString KateLspBridge::serverState(const QString &path) const
{
    Q_UNUSED(path);
    if (m_lspPlugin) {
        return m_diagnosticsModel ? u"loaded (DiagnosticsView)"_s : u"loaded"_s;
    }
    if (m_diagnosticsModel) {
        return u"diagnostics-view"_s;
    }
    return u"none"_s;
}

QList<LspDiagnostic> KateLspBridge::readDiagnosticsFromModel(const QAbstractItemModel *model,
                                                             const QString &wantedCanonicalPath)
{
    QList<LspDiagnostic> out;
    if (!model) {
        return out;
    }
    const int fileRows = model->rowCount();
    for (int f = 0; f < fileRows; ++f) {
        const QModelIndex fileIdx = model->index(f, 0);
        if (!fileIdx.isValid()) {
            continue;
        }
        QString filePath;
        const QVariant urlVar = model->data(fileIdx, Qt::UserRole);
        if (urlVar.canConvert<QUrl>()) {
            const QUrl url = urlVar.toUrl();
            filePath = url.isLocalFile() ? url.toLocalFile() : url.toString();
        }
        if (filePath.isEmpty()) {
            filePath = model->data(fileIdx, Qt::DisplayRole).toString().trimmed();
        }
        if (filePath.isEmpty()) {
            continue;
        }
        const QString canonFile = canonicalPath(filePath);
        if (!wantedCanonicalPath.isEmpty() && canonFile != wantedCanonicalPath) {
            continue;
        }

        const int diagRows = model->rowCount(fileIdx);
        for (int d = 0; d < diagRows; ++d) {
            const QModelIndex diagIdx = model->index(d, 0, fileIdx);
            if (!diagIdx.isValid()) {
                continue;
            }
            // Skip suppressed/disabled rows when Kate marks them disabled.
            if (!(model->flags(diagIdx) & Qt::ItemIsEnabled)) {
                continue;
            }
            const QString rawText = model->data(diagIdx, Qt::DisplayRole).toString().trimmed();
            if (rawText.isEmpty()) {
                continue;
            }
            LspDiagnostic diag;
            diag.path = filePath;
            const KTextEditor::Range range = rangeFromModelItem(model, diagIdx);
            if (range.isValid()) {
                diag.line = range.start().line() + 1;
                diag.column = range.start().column() + 1;
            } else {
                diag.line = 1;
                diag.column = 1;
            }
            diag.severity = severityFromModelItem(model, diagIdx);
            diag.source = u"diagnostics"_s;
            parseSourceCodeMessage(rawText, &diag.source, &diag.message);

            // Append continuation lines and related information stored as child
            // rows under the diagnostic item in Kate's DiagnosticsView.
            const int subRows = model->rowCount(diagIdx);
            QStringList extras;
            for (int s = 0; s < subRows && extras.size() < 4; ++s) {
                const QModelIndex subIdx = model->index(s, 0, diagIdx);
                const QString subText = model->data(subIdx, Qt::DisplayRole).toString().trimmed();
                if (!subText.isEmpty()) {
                    extras.append(subText);
                }
            }
            if (!extras.isEmpty()) {
                diag.message += u" | "_s + extras.join(u" | "_s);
            }
            out.append(diag);
        }
    }
    return out;
}

QList<LspDiagnostic> KateLspBridge::diagnostics(const QString &path) const
{
    QList<LspDiagnostic> out;
    const QString wanted = path.isEmpty() ? QString() : canonicalPath(path);

    // Primary source: Kate's DiagnosticsView model, which carries real
    // diagnostic messages, exact line/column ranges, sources, codes, and
    // diagnostics on unopened workspace files from LSP, katebuildplugin, etc.
    QSet<QString> coveredFiles;
    if (const QAbstractItemModel *model = diagnosticsSourceModel()) {
        out = readDiagnosticsFromModel(model, wanted);
        for (const LspDiagnostic &d : out) {
            coveredFiles.insert(canonicalPath(d.path));
        }
    }

    // Fallback: KTextEditor::Document marks for open files not already covered
    // by DiagnosticsView (e.g., unit tests, KWrite, or marks from other plugins).
    auto collect = [&](KTextEditor::Document *document) {
        const QString docPath = canonicalPath(document->url().toLocalFile());
        if (!wanted.isEmpty() && docPath != wanted) {
            return;
        }
        if (coveredFiles.contains(docPath)) {
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
    return out;
}

QStringList KateLspBridge::recentMessages(int maxCount) const
{
    if (maxCount <= 0 || m_messages.size() <= maxCount) {
        return m_messages;
    }
    return m_messages.mid(m_messages.size() - maxCount);
}

} // namespace KateAi
