/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "katedebugbridge.h"

#include <KActionCollection>
#include <KSelectAction>
#include <KTextEditor/Application>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>
#include <KTextEditor/Plugin>
#include <KTextEditor/View>
#include <KXMLGUIClient>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

constexpr auto kGdbPluginId = "kategdbplugin";
constexpr int kMaxOutputBufferChars = 32000;
constexpr int kPollIntervalMs = 2500;

QString normalizeFilePath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    const QUrl url = QUrl::fromUserInput(path);
    if (url.isLocalFile()) {
        return QDir::cleanPath(url.toLocalFile());
    }
    return QDir::cleanPath(path);
}

void splitPathAndLine(const QString &text, QString *file, int *line)
{
    // Handles "/path/to/file.cpp:42", "file.cpp:42:10", "C:/foo/bar.cpp:12",
    // or "file.cpp" without line number.
    const QString trimmed = text.trimmed();
    static const QRegularExpression re(QStringLiteral("^(.+?):(\\d+)(?::\\d+)?$"));
    const QRegularExpressionMatch m = re.match(trimmed);
    if (m.hasMatch()) {
        *file = m.captured(1).trimmed();
        *line = m.captured(2).toInt();
        return;
    }
    *file = trimmed;
    *line = 0;
}

void collectLocalsRecursive(const QTreeWidgetItem *item, const QString &scope, QList<DebugVariable> *out, int depth)
{
    if (!item || depth > 3) {
        return;
    }
    const QString name = item->text(0).trimmed();
    if (name.isEmpty()) {
        return;
    }
    DebugVariable var;
    var.name = name;
    var.scope = scope;
    // LocalsView columns are: 0=Symbol, 1=Type, 2=Value (older builds: 0=Symbol, 1=Value).
    if (item->columnCount() >= 3) {
        var.type = item->text(1).trimmed();
        var.value = item->text(2).trimmed();
    } else {
        var.value = item->text(1).trimmed();
    }
    if (var.value.isEmpty()) {
        const QString raw = item->data(item->columnCount() >= 3 ? 2 : 1, Qt::UserRole).toString();
        if (!raw.isEmpty()) {
            var.value = raw;
        }
    }
    const QVariant refVar = item->data(0, Qt::UserRole + 1);
    if (refVar.isValid()) {
        var.variablesReference = refVar.toInt();
    }
    for (int i = 0; i < item->childCount() && i < 64; ++i) {
        collectLocalsRecursive(item->child(i), scope, &var.children, depth + 1);
    }
    out->append(std::move(var));
}

} // namespace

KateDebugBridge::KateDebugBridge(QObject *parent)
    : DebugProvider(parent)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &KateDebugBridge::pollDebuggerState);
    m_pollTimer->start();

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
    }
}

KateDebugBridge::~KateDebugBridge() = default;

void KateDebugBridge::setMainWindow(KTextEditor::MainWindow *mainWindow)
{
    if (m_mainWindow == mainWindow) {
        return;
    }
    m_mainWindow = mainWindow;
    m_gdbPluginView = nullptr;
    m_backend = nullptr;
    m_state = DebugSessionState::Unavailable;
    if (m_mainWindow) {
        connect(m_mainWindow, &KTextEditor::MainWindow::pluginViewCreated, this,
                [this](const QString &name, QObject *pluginView) {
                    onPluginViewCreated(name, pluginView);
                },
                Qt::UniqueConnection);
    }
    probeGdbPlugin();
}

void KateDebugBridge::onPluginCreated(const QString &name, QObject *plugin)
{
    Q_UNUSED(plugin);
    if (name == QLatin1String(kGdbPluginId)) {
        probeGdbPlugin();
    }
}

void KateDebugBridge::onPluginDeleted(const QString &name, QObject *plugin)
{
    Q_UNUSED(plugin);
    if (name == QLatin1String(kGdbPluginId)) {
        m_gdbPluginView = nullptr;
        m_backend = nullptr;
        m_state = DebugSessionState::Unavailable;
        Q_EMIT availabilityChanged();
        Q_EMIT stateChanged();
    }
}

void KateDebugBridge::onPluginViewCreated(const QString &name, QObject *pluginView)
{
    Q_UNUSED(pluginView);
    if (name == QLatin1String(kGdbPluginId)) {
        probeGdbPlugin();
    }
}

void KateDebugBridge::probeGdbPlugin()
{
    QObject *view = m_mainWindow ? m_mainWindow->pluginView(QLatin1String(kGdbPluginId)) : nullptr;
    if (view == m_gdbPluginView && (!view || m_backend)) {
        return;
    }
    m_gdbPluginView = view;
    m_backend = nullptr;
    if (!m_gdbPluginView) {
        if (m_state != DebugSessionState::Unavailable) {
            m_state = DebugSessionState::Unavailable;
            Q_EMIT availabilityChanged();
            Q_EMIT stateChanged();
        }
        return;
    }

    if (m_state == DebugSessionState::Unavailable) {
        m_state = DebugSessionState::Idle;
    }

    // Locate the Backend child object created inside KatePluginGDBView.
    const QList<QObject *> children = m_gdbPluginView->findChildren<QObject *>();
    for (QObject *child : children) {
        if (child && QByteArrayView(child->metaObject()->className()) == "Backend") {
            bindBackend(child);
            break;
        }
    }

    // Optional Tier-2 upstream signal if a future Kate exposes debugStopped(QVariantMap).
    if (m_gdbPluginView->metaObject()->indexOfSignal("debugStopped(QVariantMap)") >= 0) {
        connect(m_gdbPluginView, SIGNAL(debugStopped(QVariantMap)), this, SIGNAL(stateChanged()),
                Qt::UniqueConnection);
    }

    Q_EMIT availabilityChanged();
    Q_EMIT stateChanged();
}

void KateDebugBridge::bindBackend(QObject *backend)
{
    if (!backend || m_backend == backend) {
        return;
    }
    m_backend = backend;
    connect(m_backend, SIGNAL(debugLocationChanged(QUrl, int)), this,
            SLOT(onDebugLocationChanged(QUrl, int)), Qt::UniqueConnection);
    connect(m_backend, SIGNAL(stackFrameChanged(int)), this,
            SLOT(onStackFrameChanged(int)), Qt::UniqueConnection);
    connect(m_backend, SIGNAL(outputText(QString)), this,
            SLOT(onOutputText(QString)), Qt::UniqueConnection);
    connect(m_backend, SIGNAL(outputError(QString)), this,
            SLOT(onOutputError(QString)), Qt::UniqueConnection);
    connect(m_backend, SIGNAL(readyForInput(bool)), this,
            SLOT(onReadyForInput(bool)), Qt::UniqueConnection);
    connect(m_backend, SIGNAL(programEnded()), this,
            SLOT(onProgramEnded()), Qt::UniqueConnection);
    connect(m_backend, SIGNAL(gdbEnded()), this,
            SLOT(onGdbEnded()), Qt::UniqueConnection);
    connect(m_backend, SIGNAL(sourceFileNotFound(QString)), this,
            SLOT(onSourceFileNotFound(QString)), Qt::UniqueConnection);
}

void KateDebugBridge::pollDebuggerState()
{
    if (!m_gdbPluginView || !m_backend) {
        probeGdbPlugin();
    }
}

bool KateDebugBridge::hasTier2Method(const char *signature) const
{
    return m_gdbPluginView && m_gdbPluginView->metaObject()->indexOfMethod(signature) >= 0;
}

void KateDebugBridge::appendOutputBuffer(const QString &chunk)
{
    if (chunk.isEmpty()) {
        return;
    }
    m_outputBuffer.append(chunk);
    if (m_outputBuffer.size() > kMaxOutputBufferChars) {
        m_outputBuffer.remove(0, m_outputBuffer.size() - kMaxOutputBufferChars);
    }
    Q_EMIT outputReceived(chunk);
}

void KateDebugBridge::onDebugLocationChanged(const QUrl &url, int lineNum)
{
    const QString local = url.isLocalFile() ? QDir::cleanPath(url.toLocalFile()) : url.toString();
    m_lastStop.file = local;
    m_lastStop.line = lineNum;
    if (m_state != DebugSessionState::Paused) {
        m_state = DebugSessionState::Paused;
    }
    Q_EMIT stateChanged();
    if (!m_stopEmittedForCurrentPause && !local.isEmpty() && lineNum > 0) {
        m_stopEmittedForCurrentPause = true;
        Q_EMIT stopped(m_lastStop);
    }
}

void KateDebugBridge::onStackFrameChanged(int level)
{
    m_activeFrameLevel = qMax(0, level);
    Q_EMIT localsUpdated();
    Q_EMIT stateChanged();
}

void KateDebugBridge::onOutputText(const QString &text)
{
    appendOutputBuffer(text);

    // Capture expression evaluation reply from DapBackend::onExpressionEvaluated
    // which emits "\n(<expr>) = <result>".
    if (m_evalPending) {
        QString val;
        if (parseEvalResultFromOutput(text, m_evalExpression, &val)) {
            m_evalResult = val;
            m_evalCaptured = true;
            m_evalPending = false;
        }
    }

    // Parse stop metadata from DapBackend::onStopped output text.
    DebugStopInfo parsed = m_lastStop;
    if (parseStopEventFromOutput(text, &parsed)) {
        m_lastStop = parsed;
        m_state = DebugSessionState::Paused;
        m_stopEmittedForCurrentPause = false;
        Q_EMIT stateChanged();
        if (!m_lastStop.file.isEmpty() && m_lastStop.line > 0) {
            m_stopEmittedForCurrentPause = true;
            Q_EMIT stopped(m_lastStop);
        }
    } else if (text.contains(u"(running)"_s, Qt::CaseInsensitive)
               || text.contains(u"(continued)"_s, Qt::CaseInsensitive)) {
        m_state = DebugSessionState::Running;
        m_stopEmittedForCurrentPause = false;
        Q_EMIT stateChanged();
    } else if (text.contains(u"breakpoint set"_s, Qt::CaseInsensitive)
               || text.contains(u"breakpoint"_s, Qt::CaseInsensitive)) {
        Q_EMIT breakpointsChanged();
    }
}

void KateDebugBridge::onOutputError(const QString &text)
{
    appendOutputBuffer(text);
    if (m_evalPending && text.contains(u"error on response"_s, Qt::CaseInsensitive)) {
        m_evalResult = text.trimmed();
        m_evalCaptured = true;
        m_evalPending = false;
    }
}

void KateDebugBridge::onReadyForInput(bool ready)
{
    if (ready) {
        m_state = DebugSessionState::Paused;
        Q_EMIT stateChanged();
        if (!m_stopEmittedForCurrentPause) {
            // Pull location from active document Execution mark if debugLocationChanged
            // hasn't arrived yet.
            if (m_lastStop.file.isEmpty() || m_lastStop.line <= 0) {
                const DebugStopInfo s = currentStop();
                if (!s.file.isEmpty() && s.line > 0) {
                    m_lastStop.file = s.file;
                    m_lastStop.line = s.line;
                }
            }
            m_stopEmittedForCurrentPause = true;
            Q_EMIT stopped(m_lastStop);
        }
    } else if (m_state == DebugSessionState::Paused || m_state == DebugSessionState::Idle) {
        // When stepping/continuing, readyForInput(false) fires before (running).
        if (m_backend) {
            m_state = DebugSessionState::Running;
            m_stopEmittedForCurrentPause = false;
            Q_EMIT stateChanged();
        }
    }
}

void KateDebugBridge::onProgramEnded()
{
    m_state = DebugSessionState::Terminated;
    m_stopEmittedForCurrentPause = false;
    Q_EMIT stateChanged();
}

void KateDebugBridge::onGdbEnded()
{
    m_state = isAvailable() ? DebugSessionState::Idle : DebugSessionState::Unavailable;
    m_lastStop = {};
    m_activeFrameLevel = 0;
    m_stopEmittedForCurrentPause = false;
    Q_EMIT stateChanged();
}

void KateDebugBridge::onSourceFileNotFound(const QString &fileName)
{
    appendOutputBuffer(u"\n[source file not found: %1]\n"_s.arg(fileName));
}

bool KateDebugBridge::isAvailable() const
{
    return m_gdbPluginView != nullptr;
}

DebugSessionState KateDebugBridge::sessionState() const
{
    if (!m_gdbPluginView) {
        return DebugSessionState::Unavailable;
    }
    if (hasTier2Method("debugState()")) {
        QVariantMap map;
        if (QMetaObject::invokeMethod(m_gdbPluginView, "debugState", Qt::DirectConnection,
                                      Q_RETURN_ARG(QVariantMap, map))) {
            const QString st = map.value(u"state"_s).toString().toLower();
            if (st == u"paused"_s || st == u"stopped"_s) {
                return DebugSessionState::Paused;
            }
            if (st == u"running"_s) {
                return DebugSessionState::Running;
            }
            if (st == u"terminated"_s) {
                return DebugSessionState::Terminated;
            }
            return DebugSessionState::Idle;
        }
    }
    // Cross-check KXMLGUIClient actions: if "kill" is disabled, no session is live.
    if (QAction *killAct = findPluginAction(u"kill"_s)) {
        if (!killAct->isEnabled()) {
            return m_state == DebugSessionState::Terminated ? DebugSessionState::Terminated : DebugSessionState::Idle;
        }
        if (QAction *stepOverAct = findPluginAction(u"step_over"_s)) {
            if (stepOverAct->isEnabled()) {
                return DebugSessionState::Paused;
            }
            return DebugSessionState::Running;
        }
    }
    return m_state;
}

QWidget *KateDebugBridge::findChildWidgetByClass(const char *className) const
{
    if (!m_mainWindow || !m_mainWindow->window()) {
        return nullptr;
    }
    const QList<QWidget *> widgets = m_mainWindow->window()->findChildren<QWidget *>();
    for (QWidget *w : widgets) {
        if (w && QByteArrayView(w->metaObject()->className()) == className) {
            return w;
        }
    }
    return nullptr;
}

QTreeView *KateDebugBridge::findStackTreeView() const
{
    QWidget *stackView = findChildWidgetByClass("StackView");
    return stackView ? stackView->findChild<QTreeView *>() : nullptr;
}

QComboBox *KateDebugBridge::findThreadCombo() const
{
    QWidget *stackView = findChildWidgetByClass("StackView");
    return stackView ? stackView->findChild<QComboBox *>() : nullptr;
}

QTreeWidget *KateDebugBridge::findLocalsTreeWidget() const
{
    QWidget *localsView = findChildWidgetByClass("LocalsView");
    if (!localsView) {
        return nullptr;
    }
    if (auto *tw = qobject_cast<QTreeWidget *>(localsView)) {
        return tw;
    }
    return localsView->findChild<QTreeWidget *>();
}

QTreeView *KateDebugBridge::findBreakpointTreeView() const
{
    QWidget *bpView = findChildWidgetByClass("BreakpointView");
    return bpView ? bpView->findChild<QTreeView *>() : nullptr;
}

QTextEdit *KateDebugBridge::findOutputTextEdit() const
{
    QWidget *configView = findChildWidgetByClass("ConfigView");
    QWidget *root = configView ? configView->parentWidget() : nullptr;
    while (root && !qobject_cast<QTabWidget *>(root)) {
        root = root->parentWidget();
    }
    if (!root) {
        return nullptr;
    }
    const QList<QTextEdit *> edits = root->findChildren<QTextEdit *>();
    for (QTextEdit *te : edits) {
        if (te && te->isReadOnly()) {
            return te;
        }
    }
    return nullptr;
}

QString KateDebugBridge::readIoViewOutput(int maxChars) const
{
    QWidget *ioView = findChildWidgetByClass("IOView");
    if (!ioView) {
        return {};
    }
    QStringList sections;
    const QList<QTextEdit *> edits = ioView->findChildren<QTextEdit *>();
    for (QTextEdit *te : edits) {
        if (!te) {
            continue;
        }
        const QString text = te->toPlainText().trimmed();
        if (!text.isEmpty()) {
            sections.append(text.right(qMax(256, maxChars / 2)));
        }
    }
    return sections.join(u"\n---\n"_s).right(maxChars);
}

QAction *KateDebugBridge::findPluginAction(const QString &actionName) const
{
    if (!m_gdbPluginView) {
        return nullptr;
    }
    auto *client = dynamic_cast<KXMLGUIClient *>(m_gdbPluginView.data());
    if (!client || !client->actionCollection()) {
        return nullptr;
    }
    return client->actionCollection()->action(actionName);
}

QString KateDebugBridge::activeTarget() const
{
    if (auto *targetsAct = qobject_cast<KSelectAction *>(findPluginAction(u"targets"_s))) {
        return targetsAct->currentText();
    }
    if (QWidget *cfg = findChildWidgetByClass("ConfigView")) {
        const QList<QComboBox *> combos = cfg->findChildren<QComboBox *>();
        for (QComboBox *cb : combos) {
            if (cb && cb->isEditable()) {
                return cb->currentText();
            }
        }
    }
    return {};
}

QList<DebugTarget> KateDebugBridge::targets() const
{
    QList<DebugTarget> out;
    if (!m_gdbPluginView) {
        return out;
    }
    if (QWidget *cfg = findChildWidgetByClass("ConfigView")) {
        QComboBox *targetCombo = nullptr;
        QComboBox *clientCombo = nullptr;
        for (QComboBox *cb : cfg->findChildren<QComboBox *>()) {
            if (!cb) {
                continue;
            }
            if (cb->isEditable()) {
                targetCombo = cb;
            } else if (!clientCombo) {
                clientCombo = cb;
            }
        }
        const QString currentClient = clientCombo ? clientCombo->currentText() : QString();
        if (targetCombo) {
            for (int i = 0; i < targetCombo->count(); ++i) {
                DebugTarget t;
                t.name = targetCombo->itemText(i);
                t.active = (i == targetCombo->currentIndex());
                const QJsonObject obj = targetCombo->itemData(i).toJsonObject();
                t.executable = obj.value(u"file"_s).toString();
                if (t.executable.isEmpty()) {
                    t.executable = obj.value(u"program"_s).toString();
                }
                t.workingDir = obj.value(u"workingDirectory"_s).toString();
                if (t.workingDir.isEmpty()) {
                    t.workingDir = obj.value(u"cwd"_s).toString();
                }
                t.arguments = obj.value(u"arguments"_s).toString();
                t.adapter = obj.value(u"debugger"_s).toString();
                if (t.adapter.isEmpty()) {
                    t.adapter = obj.value(u"type"_s).toString();
                }
                if (t.adapter.isEmpty()) {
                    t.adapter = currentClient;
                }
                t.profile = obj.value(u"debuggerProfile"_s).toString();
                t.fromLaunchJson = obj.value(u"isLaunchJson"_s).toBool();
                out.append(std::move(t));
            }
            if (!out.isEmpty()) {
                return out;
            }
        }
    }

    if (auto *targetsAct = qobject_cast<KSelectAction *>(findPluginAction(u"targets"_s))) {
        const QStringList items = targetsAct->items();
        const int cur = targetsAct->currentItem();
        for (int i = 0; i < items.size(); ++i) {
            DebugTarget t;
            t.name = items.at(i);
            t.active = (i == cur);
            out.append(std::move(t));
        }
    }
    return out;
}

DebugStopInfo KateDebugBridge::currentStop() const
{
    DebugStopInfo info = m_lastStop;
    // Fallback to KTextEditor::Document::Execution mark if signal missed location.
    if (info.file.isEmpty() || info.line <= 0) {
        if (auto *editor = KTextEditor::Editor::instance()) {
            for (KTextEditor::Document *doc : editor->documents()) {
                if (!doc || !doc->url().isLocalFile()) {
                    continue;
                }
                const auto marks = doc->marks();
                for (auto it = marks.cbegin(); it != marks.cend(); ++it) {
                    if (it.value()->type & KTextEditor::Document::Execution) {
                        info.file = QDir::cleanPath(doc->url().toLocalFile());
                        info.line = it.key() + 1;
                        break;
                    }
                }
                if (!info.file.isEmpty()) {
                    break;
                }
            }
        }
    }
    if (info.file.isEmpty()) {
        const QList<DebugStackFrame> frames = stackFrames();
        for (const DebugStackFrame &f : frames) {
            if (f.active || f.level == 0) {
                info.file = f.file;
                info.line = f.line;
                break;
            }
        }
    }
    info.isCrashOrException = isCrashStopReason(info.reason, info.description);
    return info;
}

QList<DebugThread> KateDebugBridge::threads() const
{
    QList<DebugThread> out;
    if (QComboBox *combo = findThreadCombo()) {
        for (int i = 0; i < combo->count(); ++i) {
            DebugThread t;
            t.name = combo->itemText(i).trimmed();
            const QVariant idVar = combo->itemData(i, Qt::UserRole);
            t.id = idVar.isValid() ? idVar.toInt() : (i + 1);
            t.active = (i == combo->currentIndex());
            t.state = (sessionState() == DebugSessionState::Paused) ? u"stopped"_s : u"running"_s;
            out.append(std::move(t));
        }
    }
    return out;
}

QList<DebugStackFrame> KateDebugBridge::readStackFramesFromModel(const QAbstractItemModel *model, int activeRow)
{
    QList<DebugStackFrame> out;
    if (!model) {
        return out;
    }
    const int rows = model->rowCount();
    const int cols = model->columnCount();
    for (int r = 0; r < rows; ++r) {
        DebugStackFrame frame;
        if (cols >= 3) {
            const QString numStr = model->data(model->index(r, 0), Qt::DisplayRole).toString().trimmed();
            bool ok = false;
            const int parsed = numStr.toInt(&ok);
            frame.level = ok ? parsed : r;
            frame.function = model->data(model->index(r, 1), Qt::DisplayRole).toString().trimmed();
            const QString pathText = model->data(model->index(r, 2), Qt::DisplayRole).toString().trimmed();
            splitPathAndLine(pathText, &frame.file, &frame.line);
        } else if (cols == 2) {
            frame.level = r;
            frame.function = model->data(model->index(r, 0), Qt::DisplayRole).toString().trimmed();
            const QString pathText = model->data(model->index(r, 1), Qt::DisplayRole).toString().trimmed();
            splitPathAndLine(pathText, &frame.file, &frame.line);
        } else if (cols == 1) {
            frame.level = r;
            frame.function = model->data(model->index(r, 0), Qt::DisplayRole).toString().trimmed();
        }
        frame.active = (r == activeRow);
        out.append(std::move(frame));
    }
    return out;
}

QList<DebugStackFrame> KateDebugBridge::stackFrames(int threadId) const
{
    Q_UNUSED(threadId);
    if (hasTier2Method("stackFrames()")) {
        QVariantList list;
        if (QMetaObject::invokeMethod(m_gdbPluginView, "stackFrames", Qt::DirectConnection,
                                      Q_RETURN_ARG(QVariantList, list))) {
            QList<DebugStackFrame> out;
            for (int i = 0; i < list.size(); ++i) {
                const QVariantMap m = list.at(i).toMap();
                DebugStackFrame f;
                f.level = m.value(u"level"_s, i).toInt();
                f.id = m.value(u"id"_s, -1).toInt();
                f.function = m.value(u"function"_s).toString();
                f.file = m.value(u"file"_s).toString();
                f.line = m.value(u"line"_s).toInt();
                f.column = m.value(u"column"_s).toInt();
                f.active = m.value(u"active"_s, i == m_activeFrameLevel).toBool();
                out.append(std::move(f));
            }
            return out;
        }
    }

    QTreeView *tv = findStackTreeView();
    if (!tv || !tv->model()) {
        return {};
    }
    const QAbstractItemModel *model = tv->model();
    int activeRow = m_activeFrameLevel;
    if (tv->currentIndex().isValid()) {
        activeRow = tv->currentIndex().row();
    }
    return readStackFramesFromModel(model, activeRow);
}

QList<DebugVariable> KateDebugBridge::readLocalsFromTree(const QTreeWidget *tree)
{
    QList<DebugVariable> out;
    if (!tree) {
        return out;
    }
    const int topCount = tree->topLevelItemCount();
    for (int i = 0; i < topCount; ++i) {
        const QTreeWidgetItem *top = tree->topLevelItem(i);
        if (!top) {
            continue;
        }
        // In Kate's LocalsView, top-level items are Scopes ("Locals", "Globals",
        // "Registers") with variables as children. If a top-level item has a
        // value in col 2 (flat tree), treat it directly as a variable.
        const bool isScope = top->childCount() > 0 && top->text(1).trimmed().isEmpty() && top->text(2).trimmed().isEmpty();
        if (isScope) {
            const QString scopeName = top->text(0).trimmed();
            for (int c = 0; c < top->childCount(); ++c) {
                collectLocalsRecursive(top->child(c), scopeName, &out, 0);
            }
        } else {
            collectLocalsRecursive(top, u"Locals"_s, &out, 0);
        }
    }
    return out;
}

QList<DebugVariable> KateDebugBridge::locals(int frameLevel) const
{
    Q_UNUSED(frameLevel);
    if (hasTier2Method("locals()")) {
        QVariantList list;
        if (QMetaObject::invokeMethod(m_gdbPluginView, "locals", Qt::DirectConnection,
                                      Q_RETURN_ARG(QVariantList, list))) {
            QList<DebugVariable> out;
            for (const QVariant &item : list) {
                const QVariantMap m = item.toMap();
                DebugVariable v;
                v.name = m.value(u"name"_s).toString();
                v.type = m.value(u"type"_s).toString();
                v.value = m.value(u"value"_s).toString();
                v.scope = m.value(u"scope"_s, u"Locals"_s).toString();
                v.variablesReference = m.value(u"variablesReference"_s).toInt();
                out.append(std::move(v));
            }
            return out;
        }
    }
    return readLocalsFromTree(findLocalsTreeWidget());
}

QList<DebugVariable> KateDebugBridge::expandVariable(int variablesReference, int timeoutMs)
{
    if (variablesReference <= 0 || !m_backend) {
        return {};
    }
    QMetaObject::invokeMethod(m_backend, "requestVariables", Qt::DirectConnection,
                              Q_ARG(int, variablesReference));
    // Give the DAP adapter a short window to populate LocalsView children.
    QElapsedTimer timer;
    timer.start();
    const int cap = qBound(100, timeoutMs, 5000);
    while (timer.elapsed() < cap) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
        const QList<DebugVariable> all = locals();
        std::function<QList<DebugVariable>(const QList<DebugVariable> &)> findRef =
            [&](const QList<DebugVariable> &vars) -> QList<DebugVariable> {
            for (const DebugVariable &v : vars) {
                if (v.variablesReference == variablesReference && !v.children.isEmpty()) {
                    return v.children;
                }
                const QList<DebugVariable> nested = findRef(v.children);
                if (!nested.isEmpty()) {
                    return nested;
                }
            }
            return {};
        };
        const QList<DebugVariable> found = findRef(all);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

QList<DebugBreakpoint> KateDebugBridge::readBreakpointsFromModel(const QAbstractItemModel *model)
{
    QList<DebugBreakpoint> out;
    if (!model) {
        return out;
    }
    const int groups = model->rowCount();
    for (int g = 0; g < groups; ++g) {
        const QModelIndex groupIdx = model->index(g, 0);
        const QString groupLabel = model->data(groupIdx, Qt::DisplayRole).toString().toLower();
        QString kind = u"line"_s;
        if (groupLabel.contains(u"function"_s)) {
            kind = u"function"_s;
        } else if (groupLabel.contains(u"exception"_s)) {
            kind = u"exception"_s;
        }
        const int rows = model->rowCount(groupIdx);
        const int cols = model->columnCount(groupIdx);
        for (int r = 0; r < rows; ++r) {
            const QModelIndex idx0 = model->index(r, 0, groupIdx);
            const QString col0 = model->data(idx0, Qt::DisplayRole).toString().trimmed();
            const QString col1 = cols > 1 ? model->data(model->index(r, 1, groupIdx), Qt::DisplayRole).toString().trimmed() : QString();
            const QString col2 = cols > 2 ? model->data(model->index(r, 2, groupIdx), Qt::DisplayRole).toString().trimmed() : QString();
            const QString col3 = cols > 3 ? model->data(model->index(r, 3, groupIdx), Qt::DisplayRole).toString().trimmed() : QString();

            DebugBreakpoint bp;
            bp.kind = kind;
            const QVariant checkVar = model->data(idx0, Qt::CheckStateRole);
            if (checkVar.isValid()) {
                bp.enabled = (checkVar.toInt() != Qt::Unchecked);
            }
            if (kind == u"line"_s) {
                // Kate's BreakpointModel typically displays file:line in col 0 or
                // file in col 0 and line in col 1, plus condition / hit count.
                splitPathAndLine(col0, &bp.file, &bp.line);
                if (bp.line == 0 && !col1.isEmpty()) {
                    bool ok = false;
                    const int ln = col1.toInt(&ok);
                    if (ok && ln > 0) {
                        bp.line = ln;
                        bp.condition = col2;
                        bp.hitCondition = col3;
                    } else {
                        bp.condition = col1;
                        bp.hitCondition = col2;
                    }
                } else {
                    bp.condition = col1;
                    bp.hitCondition = col2;
                }
            } else {
                bp.functionName = col0;
                bp.condition = col1;
                bp.hitCondition = col2;
            }
            if (!bp.file.isEmpty() || !bp.functionName.isEmpty()) {
                out.append(std::move(bp));
            }
        }
    }
    return out;
}

QList<DebugBreakpoint> KateDebugBridge::breakpoints() const
{
    QList<DebugBreakpoint> out;
    if (QTreeView *tv = findBreakpointTreeView()) {
        if (const QAbstractItemModel *model = tv->model()) {
            while (auto *proxy = qobject_cast<const QAbstractProxyModel *>(model)) {
                if (!proxy->sourceModel()) {
                    break;
                }
                model = proxy->sourceModel();
            }
            out = readBreakpointsFromModel(model);
        }
    }

    // Supplement with KTextEditor::Document breakpoint marks so breakpoints set
    // before a session starts are always visible even if the toolview wasn't opened.
    QSet<QString> seen;
    for (const DebugBreakpoint &bp : out) {
        if (bp.kind == u"line"_s) {
            seen.insert(normalizeFilePath(bp.file) + u':' + QString::number(bp.line));
        }
    }
    if (auto *editor = KTextEditor::Editor::instance()) {
        for (KTextEditor::Document *doc : editor->documents()) {
            if (!doc || !doc->url().isLocalFile()) {
                continue;
            }
            const QString file = QDir::cleanPath(doc->url().toLocalFile());
            const auto marks = doc->marks();
            for (auto it = marks.cbegin(); it != marks.cend(); ++it) {
                const uint type = it.value()->type;
                const bool active = (type & KTextEditor::Document::BreakpointActive)
                    || (type & KTextEditor::Document::BreakpointReached);
                const bool disabled = (type & KTextEditor::Document::BreakpointDisabled);
                if (!active && !disabled) {
                    continue;
                }
                const int line = it.key() + 1;
                const QString key = file + u':' + QString::number(line);
                if (seen.contains(key)) {
                    continue;
                }
                seen.insert(key);
                DebugBreakpoint bp;
                bp.kind = u"line"_s;
                bp.file = file;
                bp.line = line;
                bp.enabled = !disabled;
                out.append(std::move(bp));
            }
        }
    }
    return out;
}

QString KateDebugBridge::recentOutput(int maxChars) const
{
    QString text = m_outputBuffer;
    if (text.isEmpty()) {
        if (QTextEdit *te = findOutputTextEdit()) {
            text = te->toPlainText();
        }
    }
    const QString ioTail = readIoViewOutput(maxChars / 2);
    if (!ioTail.isEmpty()) {
        text += u"\n[Debuggee IO]\n"_s + ioTail;
    }
    if (maxChars > 0 && text.size() > maxChars) {
        return text.right(maxChars);
    }
    return text;
}

bool KateDebugBridge::parseStopEventFromOutput(const QString &text, DebugStopInfo *info)
{
    if (!info || text.isEmpty()) {
        return false;
    }
    // DapBackend::onStopped emits:
    //   "stopped (<reason>). (<desc>) Active thread: <id> ... Breakpoint(s) reached: [1] [2]"
    static const QRegularExpression stoppedRe(
        QStringLiteral("stopped\\s*\\(([^)]+)\\)\\.?(?:\\s*\\(([^)]+)\\))?"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = stoppedRe.match(text);
    if (!m.hasMatch()) {
        return false;
    }
    info->reason = m.captured(1).trimmed();
    const QString desc = m.captured(2).trimmed();
    if (!desc.isEmpty()) {
        info->description = desc;
    }
    static const QRegularExpression threadRe(QStringLiteral("Active thread:\\s*(\\d+)"),
                                             QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch tm = threadRe.match(text);
    if (tm.hasMatch()) {
        info->threadId = tm.captured(1).toInt();
    }
    static const QRegularExpression bpRe(QStringLiteral("\\[(\\d+)\\]"));
    if (text.contains(u"Breakpoint"_s, Qt::CaseInsensitive)) {
        info->hitBreakpointIds.clear();
        auto it = bpRe.globalMatch(text);
        while (it.hasNext()) {
            info->hitBreakpointIds.append(it.next().captured(1).toInt());
        }
    }
    info->isCrashOrException = isCrashStopReason(info->reason, info->description);
    return true;
}

bool KateDebugBridge::parseEvalResultFromOutput(const QString &text, const QString &expression, QString *value)
{
    if (!value || text.isEmpty()) {
        return false;
    }
    // DapBackend::onExpressionEvaluated emits "\n(<expr>) = <result>".
    const QString prefix = u"(%1) = "_s.arg(expression);
    const int idx = text.indexOf(prefix);
    if (idx >= 0) {
        *value = text.mid(idx + prefix.size()).trimmed();
        return true;
    }
    static const QRegularExpression genericEval(QStringLiteral(R"(\([^)]+\)\s*=\s*(.+))"));
    const QRegularExpressionMatch m = genericEval.match(text.trimmed());
    if (m.hasMatch()) {
        *value = m.captured(1).trimmed();
        return true;
    }
    static const QRegularExpression gdbEval(QStringLiteral(R"(^\$\d+\s*=\s*(.+)$)"));
    const QRegularExpressionMatch g = gdbEval.match(text.trimmed());
    if (g.hasMatch()) {
        *value = g.captured(1).trimmed();
        return true;
    }
    return false;
}

bool KateDebugBridge::sendDapReplCommand(const QString &command)
{
    if (!m_gdbPluginView || command.trimmed().isEmpty()) {
        return false;
    }
    if (hasTier2Method("issueCommand(QString)")) {
        return QMetaObject::invokeMethod(m_gdbPluginView, "issueCommand", Qt::DirectConnection,
                                         Q_ARG(QString, command));
    }
    // Tier-1: locate the Debug View KHistoryComboBox input line and invoke
    // KatePluginGDBView::slotSendCommand().
    QWidget *configView = findChildWidgetByClass("ConfigView");
    QWidget *tabRoot = configView ? configView->parentWidget() : nullptr;
    while (tabRoot && !qobject_cast<QTabWidget *>(tabRoot)) {
        tabRoot = tabRoot->parentWidget();
    }
    if (!tabRoot) {
        return false;
    }
    for (QComboBox *cb : tabRoot->findChildren<QComboBox *>()) {
        if (!cb || !cb->isEditable()) {
            continue;
        }
        if (configView && configView->isAncestorOf(cb)) {
            continue;
        }
        cb->setEditText(command);
        return QMetaObject::invokeMethod(m_gdbPluginView, "slotSendCommand", Qt::DirectConnection);
    }
    return false;
}

bool KateDebugBridge::waitForNextPauseOrEnd(int timeoutMs, bool *ended)
{
    if (ended) {
        *ended = false;
    }
    if (!m_backend) {
        return false;
    }
    bool done = false;
    bool didEnd = false;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    const auto c1 = connect(this, &DebugProvider::stopped, &loop, [&](const DebugStopInfo &) {
        done = true;
        loop.quit();
    });
    const auto c2 = connect(m_backend, SIGNAL(programEnded()), &loop, SLOT(quit()));
    const auto c3 = connect(m_backend, SIGNAL(gdbEnded()), &loop, SLOT(quit()));
    const auto c4 = connect(this, &DebugProvider::stateChanged, &loop, [&]() {
        const auto st = sessionState();
        if (st == DebugSessionState::Terminated || st == DebugSessionState::Idle) {
            didEnd = true;
            done = true;
            loop.quit();
        }
    });

    timer.start(qBound(500, timeoutMs, 60000));
    loop.exec(QEventLoop::AllEvents);

    disconnect(c1);
    disconnect(c2);
    disconnect(c3);
    disconnect(c4);

    if (ended) {
        *ended = didEnd;
    }
    return done;
}

DebugActionResult KateDebugBridge::selectFrame(int frameLevel)
{
    if (!m_backend) {
        return {false, u"Kate's debugger plugin is not available."_s, {}};
    }
    if (sessionState() != DebugSessionState::Paused) {
        return {false, u"Debugger is not paused; cannot switch stack frames while running or idle."_s, {}};
    }
    if (!QMetaObject::invokeMethod(m_backend, "changeStackFrame", Qt::DirectConnection, Q_ARG(int, frameLevel))) {
        return {false, u"Failed to invoke changeStackFrame on Kate's debugger backend."_s, {}};
    }
    m_activeFrameLevel = frameLevel;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 120);
    return {true, u"Selected stack frame #%1."_s.arg(frameLevel), currentStop()};
}

DebugActionResult KateDebugBridge::selectThread(int threadId)
{
    if (!m_backend) {
        return {false, u"Kate's debugger plugin is not available."_s, {}};
    }
    if (sessionState() != DebugSessionState::Paused) {
        return {false, u"Debugger is not paused; cannot switch threads."_s, {}};
    }
    if (!QMetaObject::invokeMethod(m_backend, "changeThread", Qt::DirectConnection, Q_ARG(int, threadId))) {
        return {false, u"Failed to invoke changeThread on Kate's debugger backend."_s, {}};
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 120);
    return {true, u"Selected thread %1."_s.arg(threadId), currentStop()};
}

DebugActionResult KateDebugBridge::setBreakpoint(const QString &file,
                                                 int line,
                                                 const QString &condition,
                                                 const QString &hitCondition,
                                                 const QString &logMessage,
                                                 bool remove)
{
    if (!m_gdbPluginView) {
        return {false, u"Kate's debugger plugin is not available."_s, {}};
    }
    if (file.isEmpty() || line < 1) {
        return {false, u"A valid file path and 1-based line number are required."_s, {}};
    }
    const QString cleanFile = QDir::cleanPath(file);

    // Tier-2 direct API if present.
    if (hasTier2Method("setBreakpoint(QVariantMap)")) {
        QVariantMap args{
            {u"file"_s, cleanFile},
            {u"line"_s, line},
            {u"condition"_s, condition},
            {u"hitCondition"_s, hitCondition},
            {u"logMessage"_s, logMessage},
            {u"remove"_s, remove},
        };
        if (QMetaObject::invokeMethod(m_gdbPluginView, "setBreakpoint", Qt::DirectConnection, Q_ARG(QVariantMap, args))) {
            Q_EMIT breakpointsChanged();
            return {true, remove ? u"Removed breakpoint at %1:%2."_s.arg(cleanFile).arg(line)
                                 : u"Set breakpoint at %1:%2."_s.arg(cleanFile).arg(line), {}};
        }
    }

    // Tier-1 when a session is active: use DapBackend's REPL grammar
    // ("b <line> [when {cond}] [hitcount {cond}] [on <file>]" / "boff <line> [file]").
    const DebugSessionState st = sessionState();
    if (st == DebugSessionState::Paused || st == DebugSessionState::Running) {
        QString cmd;
        if (remove) {
            cmd = u"boff %1 %2"_s.arg(line).arg(cleanFile);
        } else {
            cmd = u"b %1"_s.arg(line);
            if (!condition.trimmed().isEmpty()) {
                cmd += u" when {%1}"_s.arg(condition.trimmed());
            }
            if (!hitCondition.trimmed().isEmpty()) {
                cmd += u" hitcount {%1}"_s.arg(hitCondition.trimmed());
            }
            cmd += u" on %1"_s.arg(cleanFile);
        }
        if (sendDapReplCommand(cmd)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
            Q_EMIT breakpointsChanged();
            QString msg = remove
                ? u"Removed breakpoint at %1:%2."_s.arg(cleanFile).arg(line)
                : u"Set breakpoint at %1:%2"_s.arg(cleanFile).arg(line);
            if (!remove && !condition.trimmed().isEmpty()) {
                msg += u" when (%1)"_s.arg(condition.trimmed());
            }
            if (!remove && !hitCondition.trimmed().isEmpty()) {
                msg += u" [hitcount %1]"_s.arg(hitCondition.trimmed());
            }
            return {true, msg, currentStop()};
        }
    }

    // Idle fallback: open the document at line and toggle via toggle_breakpoint action.
    if (m_mainWindow) {
        if (QAction *toggleAct = findPluginAction(u"toggle_breakpoint"_s)) {
            m_mainWindow->openUrl(QUrl::fromLocalFile(cleanFile));
            if (KTextEditor::View *v = m_mainWindow->activeView()) {
                const bool hasBp = v->document()
                    && (v->document()->mark(line - 1)
                        & (KTextEditor::Document::BreakpointActive
                           | KTextEditor::Document::BreakpointReached
                           | KTextEditor::Document::BreakpointDisabled));
                if ((remove && hasBp) || (!remove && !hasBp)) {
                    v->setCursorPosition(KTextEditor::Cursor(line - 1, 0));
                    toggleAct->trigger();
                    Q_EMIT breakpointsChanged();
                }
                QString note;
                if (!remove && (!condition.trimmed().isEmpty() || !hitCondition.trimmed().isEmpty())) {
                    note = u" (Note: debugger is idle; conditional expression will need to be applied once a session starts.)"_s;
                }
                return {true, (remove ? u"Removed breakpoint at %1:%2."_s : u"Set breakpoint at %1:%2."_s)
                                  .arg(cleanFile).arg(line) + note, {}};
            }
        }
    }
    return {false, u"Could not set breakpoint: debugger backend did not accept the command."_s, {}};
}

DebugActionResult KateDebugBridge::clearAllBreakpoints()
{
    if (QAction *act = findPluginAction(u"clear_all_breakpoints"_s)) {
        act->trigger();
        Q_EMIT breakpointsChanged();
        return {true, u"Cleared all breakpoints."_s, {}};
    }
    return {false, u"Clear-all-breakpoints action is not available."_s, {}};
}

DebugActionResult KateDebugBridge::step(const QString &action, const QString &file, int line, int timeoutMs)
{
    if (!m_backend) {
        return {false, u"Kate's debugger plugin is not available."_s, {}};
    }
    const QString a = action.trimmed().toLower();
    if (a == u"pause"_s || a == u"interrupt"_s) {
        if (!QMetaObject::invokeMethod(m_backend, "slotInterrupt", Qt::DirectConnection)) {
            return {false, u"Failed to pause debugger."_s, {}};
        }
        bool ended = false;
        waitForNextPauseOrEnd(timeoutMs, &ended);
        return {true, ended ? u"Program ended."_s : u"Paused execution."_s, currentStop()};
    }

    if (sessionState() != DebugSessionState::Paused) {
        return {false, u"Debugger is not paused (current state: %1). Pause or start a session first."_s
                           .arg(stateName(sessionState())), {}};
    }

    m_stopEmittedForCurrentPause = false;
    bool ok = false;
    if (a == u"continue"_s || a == u"resume"_s) {
        ok = QMetaObject::invokeMethod(m_backend, "slotContinue", Qt::DirectConnection);
    } else if (a == u"step_over"_s || a == u"next"_s) {
        ok = QMetaObject::invokeMethod(m_backend, "slotStepOver", Qt::DirectConnection);
    } else if (a == u"step_into"_s || a == u"step_in"_s || a == u"into"_s) {
        ok = QMetaObject::invokeMethod(m_backend, "slotStepInto", Qt::DirectConnection);
    } else if (a == u"step_out"_s || a == u"out"_s || a == u"finish"_s) {
        ok = QMetaObject::invokeMethod(m_backend, "slotStepOut", Qt::DirectConnection);
    } else if (a == u"run_to_cursor"_s) {
        if (!file.isEmpty() && line > 0) {
            ok = sendDapReplCommand(u"to %1 %2"_s.arg(line).arg(QDir::cleanPath(file)));
        } else if (QAction *rtc = findPluginAction(u"run_to_cursor"_s)) {
            rtc->trigger();
            ok = true;
        }
    } else {
        return {false, u"Unknown step action '%1'. Expected: continue, step_over, step_into, step_out, run_to_cursor, pause."_s.arg(action), {}};
    }

    if (!ok) {
        return {false, u"Failed to invoke '%1' on Kate's debugger backend."_s.arg(action), {}};
    }

    bool ended = false;
    const bool settled = waitForNextPauseOrEnd(timeoutMs, &ended);
    const DebugStopInfo stop = currentStop();
    if (ended) {
        return {true, u"Program terminated."_s, stop};
    }
    if (!settled) {
        return {true, u"Program is still running after %1 ms timeout."_s.arg(timeoutMs), stop};
    }
    QString msg = u"Stopped"_s;
    if (!stop.reason.isEmpty()) {
        msg += u" (%1)"_s.arg(stop.reason);
    }
    if (!stop.file.isEmpty() && stop.line > 0) {
        msg += u" at %1:%2"_s.arg(stop.file).arg(stop.line);
    }
    return {true, msg, stop};
}

DebugActionResult KateDebugBridge::evaluate(const QString &expression, int timeoutMs)
{
    if (!m_backend) {
        return {false, u"Kate's debugger plugin is not available."_s, {}};
    }
    if (sessionState() != DebugSessionState::Paused) {
        return {false, u"Debugger must be paused to evaluate expressions."_s, {}};
    }
    const QString expr = expression.trimmed();
    if (expr.isEmpty()) {
        return {false, u"Expression cannot be empty."_s, {}};
    }

    m_evalPending = true;
    m_evalExpression = expr;
    m_evalResult.clear();
    m_evalCaptured = false;

    QString directReturn;
    const bool invoked = QMetaObject::invokeMethod(m_backend, "slotPrintVariable", Qt::DirectConnection,
                                                   Q_RETURN_ARG(QString, directReturn),
                                                   Q_ARG(QString, expr));
    if (!invoked) {
        m_evalPending = false;
        return {false, u"Failed to invoke expression evaluation on Kate's debugger backend."_s, {}};
    }

    QElapsedTimer timer;
    timer.start();
    const int cap = qBound(300, timeoutMs, 15000);
    while (m_evalPending && !m_evalCaptured && timer.elapsed() < cap) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
    }
    m_evalPending = false;
    if (m_evalCaptured) {
        return {true, u"%1 = %2"_s.arg(expr, m_evalResult), currentStop()};
    }
    if (!directReturn.trimmed().isEmpty()) {
        return {true, u"%1 = %2"_s.arg(expr, directReturn.trimmed()), currentStop()};
    }
    return {false, u"Timed out waiting for evaluation of '%1'."_s.arg(expr), currentStop()};
}

DebugActionResult KateDebugBridge::controlSession(const QString &action, const QString &targetName, int timeoutMs)
{
    if (!m_gdbPluginView || !m_backend) {
        return {false, u"Kate's debugger plugin is not available."_s, {}};
    }
    const QString a = action.trimmed().toLower();

    // Optional target selection before starting.
    if (!targetName.trimmed().isEmpty()) {
        if (auto *targetsAct = qobject_cast<KSelectAction *>(findPluginAction(u"targets"_s))) {
            const QStringList items = targetsAct->items();
            int matchIdx = -1;
            for (int i = 0; i < items.size(); ++i) {
                if (items.at(i).compare(targetName.trimmed(), Qt::CaseInsensitive) == 0) {
                    matchIdx = i;
                    break;
                }
            }
            if (matchIdx >= 0) {
                targetsAct->setCurrentItem(matchIdx);
                Q_EMIT targetsAct->indexTriggered(matchIdx);
            } else {
                return {false, u"Debug target '%1' not found. Available targets: %2"_s
                                   .arg(targetName, items.join(u", "_s)), {}};
            }
        }
    }

    if (a == u"start"_s) {
        m_stopEmittedForCurrentPause = false;
        if (QAction *act = findPluginAction(u"debug"_s)) {
            act->trigger();
        } else if (!QMetaObject::invokeMethod(m_gdbPluginView, "slotDebug", Qt::DirectConnection)) {
            return {false, u"Failed to start debugging session."_s, {}};
        }
        bool ended = false;
        const bool settled = waitForNextPauseOrEnd(timeoutMs, &ended);
        const DebugStopInfo stop = currentStop();
        if (ended) {
            return {true, u"Debug session ran to completion."_s, stop};
        }
        if (settled) {
            return {true, u"Debug session started and stopped at %1:%2 (%3)."_s
                              .arg(stop.file.isEmpty() ? u"<unknown>"_s : stop.file)
                              .arg(stop.line)
                              .arg(stop.reason.isEmpty() ? u"paused"_s : stop.reason), stop};
        }
        return {true, u"Debug session started (currently running)."_s, stop};
    }

    if (a == u"restart"_s || a == u"rerun"_s) {
        m_stopEmittedForCurrentPause = false;
        const bool ok = QMetaObject::invokeMethod(m_backend, "slotReRun", Qt::DirectConnection);
        if (!ok) {
            return {false, u"Failed to restart debug session."_s, {}};
        }
        bool ended = false;
        waitForNextPauseOrEnd(timeoutMs, &ended);
        return {true, ended ? u"Restarted and ran to completion."_s : u"Debug session restarted."_s, currentStop()};
    }

    if (a == u"stop"_s || a == u"kill"_s) {
        const bool ok = QMetaObject::invokeMethod(m_backend, "slotKill", Qt::DirectConnection);
        if (!ok) {
            return {false, u"Failed to stop debug session."_s, {}};
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
        return {true, u"Debug session stopped."_s, {}};
    }

    if (a == u"hot_reload"_s) {
        const bool ok = QMetaObject::invokeMethod(m_backend, "slotHotReload", Qt::DirectConnection);
        return {ok, ok ? u"Triggered hot reload."_s : u"Hot reload is not supported by this debugger."_s, currentStop()};
    }

    if (a == u"hot_restart"_s) {
        const bool ok = QMetaObject::invokeMethod(m_backend, "slotHotRestart", Qt::DirectConnection);
        return {ok, ok ? u"Triggered hot restart."_s : u"Hot restart is not supported by this debugger."_s, currentStop()};
    }

    return {false, u"Unknown session action '%1'. Expected: start, restart, stop, hot_reload, hot_restart."_s.arg(action), {}};
}

DebugActionResult KateDebugBridge::sendConsoleCommand(const QString &command)
{
    if (!m_gdbPluginView) {
        return {false, u"Kate's debugger plugin is not available."_s, {}};
    }
    const int beforeLen = m_outputBuffer.size();
    if (!sendDapReplCommand(command)) {
        return {false, u"Failed to send console command to Kate's debugger."_s, {}};
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 800) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
        if (m_outputBuffer.size() > beforeLen + command.size() + 2) {
            break;
        }
    }
    const QString delta = m_outputBuffer.mid(beforeLen).trimmed();
    return {true, delta.isEmpty() ? u"Sent command: %1"_s.arg(command) : delta, currentStop()};
}

bool KateDebugBridge::reloadLaunchJsonTargets()
{
    if (QWidget *cfg = findChildWidgetByClass("ConfigView")) {
        if (QMetaObject::invokeMethod(cfg, "readTargetsFromLaunchJson", Qt::DirectConnection)) {
            return true;
        }
        for (QToolButton *btn : cfg->findChildren<QToolButton *>()) {
            if (btn && btn->toolTip().contains(u"launch.json"_s, Qt::CaseInsensitive)) {
                btn->click();
                return true;
            }
        }
    }
    return false;
}

} // namespace KateAi
