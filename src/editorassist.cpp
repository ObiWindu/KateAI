/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "editorassist.h"
#include "chattheme.h"
#include "documentbridge.h"

#include <ktexteditor/annotationinterface.h>
#include <ktexteditor/codecompletionmodel.h>
#include <ktexteditor/document.h>
#include <ktexteditor/inlinenote.h>
#include <ktexteditor/inlinenoteprovider.h>
#include <ktexteditor/texthintinterface.h>
#include <ktexteditor/view.h>

#include <KLocalizedString>

#include <QBrush>
#include <QColor>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QShortcut>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// Hover hints arrive after this dwell time. Short enough to feel live, long
// enough not to flash a tooltip over every line the mouse crosses.
constexpr int kTextHintDelayMs = 400;

bool isIdentChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_';
}

QString wordAtCursor(const QString &lineText, int column)
{
    if (lineText.isEmpty() || column < 0 || column >= lineText.size()) {
        return {};
    }
    int pos = column;
    if (!isIdentChar(lineText.at(pos)) && pos > 0 && isIdentChar(lineText.at(pos - 1))) {
        --pos;
    }
    if (!isIdentChar(lineText.at(pos))) {
        return {};
    }
    int start = pos;
    while (start > 0 && isIdentChar(lineText.at(start - 1))) {
        --start;
    }
    int end = pos;
    while (end + 1 < lineText.size() && isIdentChar(lineText.at(end + 1))) {
        ++end;
    }
    return lineText.mid(start, end - start + 1);
}

QStringList identifiersInLine(const QString &lineText)
{
    QStringList out;
    QSet<QString> seen;
    int i = 0;
    const int n = lineText.size();
    while (i < n) {
        const QChar c = lineText.at(i);
        if (c.isLetter() || c == u'_') {
            int j = i + 1;
            while (j < n && isIdentChar(lineText.at(j))) {
                ++j;
            }
            const QString tok = lineText.mid(i, j - i);
            if (!seen.contains(tok)) {
                seen.insert(tok);
                out.append(tok);
            }
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

bool pathsMatch(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    if (a == b) {
        return true;
    }
    const QString ca = QFileInfo(a).canonicalFilePath();
    const QString cb = QFileInfo(b).canonicalFilePath();
    return !ca.isEmpty() && ca == cb;
}

} // namespace

// --- Inline "ghost text" ----------------------------------------------------
//
// KTextEditor has no native ghost-text API; an inline note at the cursor is
// the established emulation. The note renders the first line of the
// suggestion, muted rather than syntax highlighted, so it reads as a
// proposal rather than as text that is already there.
class GhostNoteProvider : public KTextEditor::InlineNoteProvider
{
public:
    explicit GhostNoteProvider(EditorAssist *assist)
        : m_assist(assist)
    {
    }

    QList<int> inlineNotes(int line) const override
    {
        if (m_assist->hasSuggestion() && line == m_assist->suggestionLine()) {
            return QList<int>{m_assist->suggestionColumn()};
        }
        return {};
    }

    QSize inlineNoteSize(const KTextEditor::InlineNote &note) const override
    {
        const QString text = displayText();
        const QFontMetrics metrics(note.font());
        return QSize(metrics.horizontalAdvance(text) + 2, note.lineHeight());
    }

    void paintInlineNote(const KTextEditor::InlineNote &note, QPainter &painter,
                         Qt::LayoutDirection) const override
    {
        // Muted chat-palette text, dimmed on purpose: the note is a proposal.
        painter.setPen(QColor(ChatTheme::textMuted()));
        painter.setFont(note.font());
        painter.drawText(QRect(0, 0, note.width(), note.lineHeight()),
                         Qt::AlignLeft | Qt::AlignVCenter, displayText());
    }

private:
    // The full suggestion lives on accept; the note shows the first line plus
    // a marker when more follows, because a multi-line string paints as one
    // long smear in a single-line note.
    QString displayText() const
    {
        const QString text = m_assist->suggestionText();
        const int newline = text.indexOf(QChar(u'\n'));
        return newline < 0 ? text : text.left(newline) + u" ..."_s;
    }

    EditorAssist *m_assist = nullptr;
};

// --- Live debugger inline variable values -----------------------------------
//
// While Kate's debugger is paused in a file, renders muted end-of-line local
// variable values on lines near the active stop line whenever an identifier on
// that line matches a variable in the active stack frame.
class DebugInlineNoteProvider : public KTextEditor::InlineNoteProvider
{
public:
    DebugInlineNoteProvider(EditorAssist *assist, KTextEditor::View *view)
        : m_assist(assist)
        , m_view(view)
    {
        setParent(assist);
    }

    QList<int> inlineNotes(int line) const override
    {
        if (!m_assist || !m_view || !m_view->document() || !m_assist->hasLiveDebugOverlay()) {
            return {};
        }
        const QString docPath = m_view->document()->url().toLocalFile();
        if (!pathsMatch(docPath, m_assist->debugPausedFile())) {
            return {};
        }
        const int pausedLine0 = m_assist->debugPausedLine() - 1;
        if (pausedLine0 >= 0 && qAbs(line - pausedLine0) > 40) {
            return {};
        }
        if (noteTextForLine(line).isEmpty()) {
            return {};
        }
        return QList<int>{m_view->document()->lineLength(line)};
    }

    QSize inlineNoteSize(const KTextEditor::InlineNote &note) const override
    {
        const QString text = noteTextForLine(note.position().line());
        if (text.isEmpty()) {
            return {};
        }
        QFont f = note.font();
        f.setItalic(true);
        const QFontMetrics metrics(f);
        return QSize(metrics.horizontalAdvance(text) + 12, note.lineHeight());
    }

    void paintInlineNote(const KTextEditor::InlineNote &note, QPainter &painter,
                         Qt::LayoutDirection) const override
    {
        const QString text = noteTextForLine(note.position().line());
        if (text.isEmpty()) {
            return;
        }
        QFont f = note.font();
        f.setItalic(true);
        painter.setFont(f);
        painter.setPen(QColor(ChatTheme::textMuted()));
        painter.drawText(QRect(8, 0, qMax(0, int(note.width()) - 8), note.lineHeight()),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }

private:
    QString noteTextForLine(int line) const
    {
        if (!m_assist || !m_view || !m_view->document() || !m_assist->hasLiveDebugOverlay()) {
            return {};
        }
        const QString lineText = m_view->document()->line(line);
        const QString trimmed = lineText.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(u"//"_s) || trimmed.startsWith(u"/*"_s)
            || trimmed.startsWith(u"*"_s) || trimmed.startsWith(u"#"_s)) {
            return {};
        }
        const auto &locals = m_assist->debugLocalsByName();
        QStringList parts;
        for (const QString &ident : identifiersInLine(lineText)) {
            const auto it = locals.constFind(ident);
            if (it == locals.constEnd() || it->value.isEmpty()) {
                continue;
            }
            QString val = it->value.simplified();
            if (val.size() > 36) {
                val = val.left(35) + u"…"_s;
            }
            parts.append(u"%1 = %2"_s.arg(ident, val));
            if (parts.size() >= 3) {
                break;
            }
        }
        if (parts.isEmpty()) {
            return {};
        }
        QString combined = u"  // "_s + parts.join(u", "_s);
        if (combined.size() > 96) {
            combined = combined.left(95) + u"…"_s;
        }
        return combined;
    }

    EditorAssist *m_assist = nullptr;
    QPointer<KTextEditor::View> m_view;
};

// --- Toggleable left-gutter AI edit & debug stop annotations ----------------
class AiEditAnnotationModel : public KTextEditor::AnnotationModel
{
public:
    AiEditAnnotationModel(EditorAssist *assist, KTextEditor::View *view)
        : m_assist(assist)
        , m_view(view)
    {
        setParent(assist);
    }

    QVariant data(int line, Qt::ItemDataRole role) const override
    {
        if (!m_assist || !m_view || !m_view->document()) {
            return {};
        }
        const QString path = m_view->document()->url().toLocalFile();
        if (path.isEmpty()) {
            return {};
        }

        const bool isDebugStop = m_assist->hasLiveDebugOverlay()
            && pathsMatch(path, m_assist->debugPausedFile())
            && m_assist->debugPausedLine() == line + 1;
        int pendingCount = 0;
        const bool isPending = m_assist->editRanges()->lineInPendingRange(path, line, &pendingCount);
        const bool isSettled = !isPending && m_assist->editRanges()->lineInSettledRange(path, line);

        if (role == Qt::DisplayRole) {
            if (isDebugStop) {
                return u"DBG"_s;
            }
            if (isPending) {
                return u"AI*"_s;
            }
            if (isSettled) {
                return u"AI"_s;
            }
            return QString();
        }
        if (role == Qt::ToolTipRole) {
            if (isDebugStop) {
                return i18n("Debugger paused at this line");
            }
            if (isPending) {
                return i18np("Kate AI pending edit (%1 change awaiting review)",
                             "Kate AI pending edit (%1 changes awaiting review)", pendingCount);
            }
            if (isSettled) {
                return i18n("Edited by Kate AI in this session");
            }
            return {};
        }
        if (role == Qt::ForegroundRole) {
            if (isDebugStop) {
                return QBrush(QColor(QStringLiteral("#e5a50a")));
            }
            if (isPending) {
                return QBrush(QColor(QStringLiteral("#3584e4")));
            }
            if (isSettled) {
                return QBrush(QColor(ChatTheme::textMuted()));
            }
            return {};
        }
        return {};
    }

private:
    EditorAssist *m_assist = nullptr;
    QPointer<KTextEditor::View> m_view;
};

// --- Hover hints over agent-edited lines, diagnostics, & live debug vars ----
class EditHintProvider : public KTextEditor::TextHintProvider
{
public:
    explicit EditHintProvider(EditorAssist *assist)
        : m_assist(assist)
    {
    }

    QString textHint(KTextEditor::View *view, const KTextEditor::Cursor &position) override
    {
        if (!view || !view->document()) {
            return {};
        }
        const QString path = view->document()->url().toLocalFile();
        if (path.isEmpty()) {
            return {};
        }
        // Live debugger variable hover takes top priority when paused in this file.
        if (m_assist->hasLiveDebugOverlay() && pathsMatch(path, m_assist->debugPausedFile())) {
            const QString ident = wordAtCursor(view->document()->line(position.line()), position.column());
            if (!ident.isEmpty()) {
                const auto it = m_assist->debugLocalsByName().constFind(ident);
                if (it != m_assist->debugLocalsByName().constEnd()) {
                    QString tip = u"<b>%1</b>"_s.arg(it->name.toHtmlEscaped());
                    if (!it->type.isEmpty()) {
                        tip += u" : <code>%1</code>"_s.arg(it->type.toHtmlEscaped());
                    }
                    tip += u" = <code>%1</code>"_s.arg(it->value.toHtmlEscaped());
                    return tip;
                }
            }
        }
        // Pending wins: a line the queue still holds says "review me", not
        // just "the agent was here".
        int pendingCount = 0;
        if (m_assist->editRanges()->lineInPendingRange(path, position.line(), &pendingCount)) {
            return i18np("Kate AI edited this — %1 change pending review",
                         "Kate AI edited this — %1 changes pending review", pendingCount);
        }
        if (m_assist->editRanges()->lineInSettledRange(path, position.line())) {
            return i18n("edited by Kate AI (turn finished)");
        }
        // A diagnostic mark on this line (LSP error/warning and friends) is
        // the other thing worth a hover: the model can be asked to fix it.
        if (LspProvider *lsp = m_assist->lspProvider()) {
            for (const LspDiagnostic &d : lsp->diagnostics(path)) {
                if (d.line == position.line() + 1) {
                    const QString sev = d.severity == 1 ? i18n("Error") : i18n("Warning");
                    return i18n("%1: %2 (Kate AI can fix this — ask in the Kate AI panel)", sev, d.message);
                }
            }
        }
        return {};
    }

private:
    EditorAssist *m_assist = nullptr;
};

// --- The native completion list, as the alternative render mode ---------------
class AiCompletionModel : public KTextEditor::CodeCompletionModel
{
    Q_OBJECT

public:
    explicit AiCompletionModel(QObject *parent)
        : KTextEditor::CodeCompletionModel(parent)
    {
    }

    void setCompletionText(const QString &text)
    {
        m_text = text;
    }

    void completionInvoked(KTextEditor::View *, const KTextEditor::Range &,
                           KTextEditor::CodeCompletionModel::InvocationType) override
    {
        setRowCount(1);
    }

    QMap<int, QVariant> itemData(const QModelIndex &index) const override
    {
        QMap<int, QVariant> data;
        if (!index.isValid() || index.row() >= 1) {
            return data;
        }
        if (index.column() == KTextEditor::CodeCompletionModel::Name) {
            // The list shows the first line; the whole completion is inserted
            // by executeCompletionItem().
            const int newline = m_text.indexOf(QChar(u'\n'));
            data.insert(Qt::DisplayRole, newline < 0 ? m_text : m_text.left(newline) + u" ..."_s);
        }
        return data;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.column() != KTextEditor::CodeCompletionModel::Name
            || (role != Qt::DisplayRole && role != Qt::EditRole)) {
            return {};
        }
        const int newline = m_text.indexOf(QChar(u'\n'));
        return newline < 0 ? m_text : m_text.left(newline) + u" ..."_s;
    }

    void executeCompletionItem(KTextEditor::View *view, const KTextEditor::Range &word,
                               const QModelIndex &index) const override
    {
        if (!view || !view->document() || !index.isValid() || m_text.isEmpty()) {
            return;
        }
        view->document()->insertText(word.end(), m_text);
    }

private:
    QString m_text;
};

// --- EditorAssist -------------------------------------------------------------

EditorAssist::EditorAssist(QObject *parent)
    : QObject(parent)
    , m_engine(new CompletionEngine(this))
    , m_editRanges(new EditRangeTracker(this))
    , m_noteProvider(new GhostNoteProvider(this))
    , m_hintProvider(new EditHintProvider(this))
    , m_popupModel(new AiCompletionModel(this))
    , m_autoTimer(new QTimer(this))
{
    m_autoTimer->setSingleShot(true);
    connect(m_autoTimer, &QTimer::timeout, this, [this]() {
        KTextEditor::View *view = m_autoView;
        if (!view || !m_settings.completionEnabled || m_settings.completionTrigger != u"auto"_s) {
            return;
        }
        if (hasSuggestion() || m_agentBusy || (m_busyCheck && m_busyCheck())) {
            return;
        }
        if (!m_engine->canRequestNow()) {
            return;
        }
        if (!m_autoNoticeFired) {
            m_autoNoticeFired = true;
            Q_EMIT autoModeFirstRun();
        }
        triggerCompletion(view);
    });
    connect(m_engine, &CompletionEngine::completed, this,
            [this](const QString &text, const CompletionPromptContext &context) {
                onCompleted(text, context);
            });
    connect(m_engine, &CompletionEngine::failed, this, [this]() { onEngineFailed(); });
}

EditorAssist::~EditorAssist()
{
    removeAcceptShortcuts();
    for (const QMetaObject::Connection &c : std::as_const(m_debugConnections)) {
        disconnect(c);
    }
    // KTextEditor has no view-about-to-be-destroyed signal; the attached views
    // are QPointer-tracked, and the still-live ones are unregistered here.
    for (const ViewAttachment &attachment : m_attachments) {
        if (attachment.view) {
            attachment.view->unregisterInlineNoteProvider(m_noteProvider);
            if (attachment.debugNoteProvider) {
                attachment.view->unregisterInlineNoteProvider(attachment.debugNoteProvider);
            }
            attachment.view->unregisterTextHintProvider(m_hintProvider);
            if (attachment.annotationModel
                && attachment.view->annotationModel() == attachment.annotationModel) {
                attachment.view->setAnnotationModel(nullptr);
                attachment.view->setAnnotationBorderVisible(false);
            }
        }
    }
}

void EditorAssist::setSettings(const Settings &settings)
{
    m_settings = settings;
    m_engine->setSettings(settings);
    m_autoTimer->setInterval(qMax(100, settings.completionAutoTriggerDelayMs));
    if (!settings.completionEnabled) {
        m_autoTimer->stop();
        clearSuggestion();
    }
}

void EditorAssist::setAgentBusy(bool busy)
{
    m_agentBusy = busy;
    m_engine->setAgentBusy(busy);
    if (busy) {
        clearSuggestion();
        m_autoTimer->stop();
    }
}

void EditorAssist::setBusyCheck(const std::function<bool()> &check)
{
    m_busyCheck = check;
}

void EditorAssist::setDebugProvider(DebugProvider *provider)
{
    if (m_debug == provider) {
        return;
    }
    for (const QMetaObject::Connection &c : std::as_const(m_debugConnections)) {
        disconnect(c);
    }
    m_debugConnections.clear();
    m_debug = provider;
    if (m_debug) {
        m_debugConnections.append(connect(m_debug, &DebugProvider::stateChanged, this, &EditorAssist::refreshDebugOverlays));
        m_debugConnections.append(connect(m_debug, &DebugProvider::stopped, this, [this](const DebugStopInfo &) {
            refreshDebugOverlays();
        }));
        m_debugConnections.append(connect(m_debug, &DebugProvider::localsUpdated, this, &EditorAssist::refreshDebugOverlays));
    }
    refreshDebugOverlays();
}

void EditorAssist::refreshDebugOverlays()
{
    m_debugPaused = false;
    m_debugPausedFile.clear();
    m_debugPausedLine = 0;
    m_debugLocalsByName.clear();

    if (m_debug && m_debug->isAvailable() && m_debug->sessionState() == DebugSessionState::Paused) {
        const DebugStopInfo stop = m_debug->currentStop();
        m_debugPaused = true;
        m_debugPausedFile = stop.file;
        m_debugPausedLine = stop.line;
        if (m_debugPausedFile.isEmpty()) {
            const QList<DebugStackFrame> frames = m_debug->stackFrames();
            for (const DebugStackFrame &f : frames) {
                if ((f.active || m_debugPausedFile.isEmpty()) && !f.file.isEmpty()) {
                    m_debugPausedFile = f.file;
                    m_debugPausedLine = f.line;
                    if (f.active) {
                        break;
                    }
                }
            }
        }
        const QList<DebugVariable> vars = m_debug->locals();
        for (const DebugVariable &v : vars) {
            if (!v.name.isEmpty()) {
                m_debugLocalsByName.insert(v.name, v);
            }
        }
    }

    for (const ViewAttachment &attachment : std::as_const(m_attachments)) {
        if (attachment.debugNoteProvider) {
            attachment.debugNoteProvider->inlineNotesReset();
        }
        if (attachment.annotationModel && m_annotationGutterEnabled) {
            Q_EMIT attachment.annotationModel->reset();
        }
    }
}

void EditorAssist::setAnnotationGutterEnabled(bool enabled)
{
    if (m_annotationGutterEnabled == enabled) {
        return;
    }
    m_annotationGutterEnabled = enabled;
    for (ViewAttachment &attachment : m_attachments) {
        applyAnnotationGutter(attachment);
    }
}

void EditorAssist::applyAnnotationGutter(ViewAttachment &attachment)
{
    if (!attachment.view) {
        return;
    }
    if (m_annotationGutterEnabled) {
        if (!attachment.annotationModel) {
            attachment.annotationModel = new AiEditAnnotationModel(this, attachment.view);
        }
        attachment.view->setAnnotationModel(attachment.annotationModel);
        attachment.view->setAnnotationBorderVisible(true);
        Q_EMIT attachment.annotationModel->reset();
    } else if (attachment.annotationModel
               && attachment.view->annotationModel() == attachment.annotationModel) {
        attachment.view->setAnnotationModel(nullptr);
        attachment.view->setAnnotationBorderVisible(false);
    }
}

EditorAssist::ViewAttachment *EditorAssist::findAttachment(KTextEditor::View *view)
{
    for (ViewAttachment &attachment : m_attachments) {
        if (attachment.view == view) {
            return &attachment;
        }
    }
    return nullptr;
}

void EditorAssist::attachView(KTextEditor::View *view)
{
    if (!view || findAttachment(view)) {
        return;
    }

    auto *debugNotes = new DebugInlineNoteProvider(this, view);
    view->registerInlineNoteProvider(m_noteProvider);
    view->registerInlineNoteProvider(debugNotes);
    view->registerTextHintProvider(m_hintProvider);
    view->setTextHintDelay(kTextHintDelayMs);

    ViewAttachment attachment;
    attachment.view = view;
    attachment.debugNoteProvider = debugNotes;
    attachment.cursorConnection =
        connect(view, &KTextEditor::View::cursorPositionChanged, this, [this, view]() {
            // A suggestion only ever belongs to the position it was built at.
            if (hasSuggestion()
                && CompletionEngine::isStale(m_suggestionContext,
                                             view->cursorPosition().line(),
                                             view->cursorPosition().column())) {
                clearSuggestion();
            }
        });
    // Any typing invalidates a showing suggestion and (in auto mode) starts
    // the idle timer.
    if (KTextEditor::Document *document = view->document()) {
        attachment.documentConnections.append(
            connect(document, &KTextEditor::Document::textChanged, this, [this, view]() {
                clearSuggestion();
                scheduleAutoTrigger(view);
            }));
    }
    if (m_annotationGutterEnabled) {
        applyAnnotationGutter(attachment);
    }
    // A destroyed view otherwise leaves a permanent attachment entry (with
    // dead connection handles) for the rest of the editor session.
    connect(view, &QObject::destroyed, this, [this, view]() {
        for (int i = m_attachments.size() - 1; i >= 0; --i) {
            if (m_attachments.at(i).view.isNull()
                || m_attachments.at(i).view.data() == view) {
                m_attachments.removeAt(i);
            }
        }
    });
    m_attachments.append(attachment);
}

void EditorAssist::scheduleAutoTrigger(KTextEditor::View *view)
{
    if (!m_settings.completionEnabled || m_settings.completionTrigger != u"auto"_s) {
        return;
    }
    m_autoView = view;
    m_autoTimer->start();
}

int EditorAssist::suggestionLine() const
{
    return m_suggestionContext.cursorLine;
}

int EditorAssist::suggestionColumn() const
{
    return m_suggestionContext.cursorColumn;
}

void EditorAssist::triggerCompletion(KTextEditor::View *view)
{
    if (!view || !view->document()) {
        return;
    }
    if (!m_settings.completionEnabled || m_agentBusy || (m_busyCheck && m_busyCheck())) {
        return;
    }
    // The native completion popup (LSP and friends) is already answering at
    // this position; an AI ghost on top would fight it for Tab.
    if (view->isCompletionActive()) {
        return;
    }
    if (!m_engine->canRequestNow()) {
        return;
    }
    requestCompletion(view);
}

void EditorAssist::requestCompletion(KTextEditor::View *view)
{
    // The request's home: responses are delivered back to this view (the
    // staleness check below then drops anything that no longer fits).
    m_suggestionView = view;
    KTextEditor::Document *document = view->document();
    const KTextEditor::Cursor cursor = view->cursorPosition();
    const CompletionPromptContext context = CompletionEngine::buildContext(
        document->text(), cursor.line(), cursor.column(),
        document->url().toLocalFile(), document->highlightingMode(),
        m_settings.completionContextPrefixChars, m_settings.completionContextSuffixChars);
    m_engine->request(context);
}

void EditorAssist::onCompleted(const QString &text, const CompletionPromptContext &context)
{
    KTextEditor::View *view = m_suggestionView;
    // The response is only usable at the position it was requested for; a
    // reply that arrives after the cursor moved is dropped, not rendered.
    if (!view || !view->document()) {
        return;
    }
    const KTextEditor::Cursor cursor = view->cursorPosition();
    if (CompletionEngine::isStale(context, cursor.line(), cursor.column())) {
        return;
    }
    if (m_settings.completionRenderMode == u"popup"_s) {
        m_popupModel->setCompletionText(text);
        view->startCompletion(KTextEditor::Range(cursor, cursor), m_popupModel);
        return;
    }
    showSuggestion(text, context);
}

void EditorAssist::onEngineFailed()
{
    // Failures are silent by design: a completion is a convenience, and a
    // tooltip-level error on every dropped request would be noise. The budget
    // and staleness paths above mean most "failures" are races the user never
    // sees.
}

void EditorAssist::showSuggestion(const QString &text, const CompletionPromptContext &context)
{
    KTextEditor::View *view = m_suggestionView;
    if (!view) {
        return;
    }
    m_suggestion = text;
    m_suggestionContext = context;
    installAcceptShortcuts(view);
    m_noteProvider->inlineNotesReset();
}

void EditorAssist::installAcceptShortcuts(KTextEditor::View *view)
{
    removeAcceptShortcuts();
    // Tab-accept must not fight Kate's own Tab (indent, snippets, vi-mode), so
    // the shortcuts exist only while a suggestion is showing, scoped to the
    // view's children.
    m_tabShortcut = new QShortcut(QKeySequence(Qt::Key_Tab), view);
    m_tabShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(m_tabShortcut, &QShortcut::activated, this, [this, view]() {
        acceptSuggestion(view);
    });
    m_escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), view);
    m_escShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(m_escShortcut, &QShortcut::activated, this, [this]() {
        clearSuggestion();
    });
}

void EditorAssist::removeAcceptShortcuts()
{
    // QPointer: the shortcuts are children of the view and die with it, so a
    // raw pointer here would dangle after a view close.
    if (m_tabShortcut) {
        delete m_tabShortcut.data();
    }
    m_tabShortcut = nullptr;
    if (m_escShortcut) {
        delete m_escShortcut.data();
    }
    m_escShortcut = nullptr;
}

void EditorAssist::acceptSuggestion(KTextEditor::View *view)
{
    if (!view || !view->document() || !hasSuggestion()) {
        return;
    }
    const KTextEditor::Cursor cursor = view->cursorPosition();
    if (CompletionEngine::isStale(m_suggestionContext, cursor.line(), cursor.column())) {
        clearSuggestion();
        return;
    }
    // Accept is always an explicit key press; completions are never
    // auto-inserted, even when the buffer holds unsaved user edits.
    view->insertText(m_suggestion);
    clearSuggestion();
}

void EditorAssist::clearSuggestion()
{
    if (!hasSuggestion()) {
        return;
    }
    m_suggestion.clear();
    removeAcceptShortcuts();
    m_noteProvider->inlineNotesReset();
}

void EditorAssist::updateEditRanges(const QString &path,
                                    const QList<KTextEditor::Range> &pending,
                                    const QList<KTextEditor::Range> &settled)
{
    m_editRanges->setRanges(path, documentForPath(path), pending, settled);
    if (m_annotationGutterEnabled) {
        for (const ViewAttachment &attachment : std::as_const(m_attachments)) {
            if (attachment.annotationModel) {
                Q_EMIT attachment.annotationModel->reset();
            }
        }
    }
}

} // namespace KateAi

#include "editorassist.moc"
