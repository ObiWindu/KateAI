/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "editorassist.h"
#include "chattheme.h"
#include "documentbridge.h"

#include <ktexteditor/codecompletionmodel.h>
#include <ktexteditor/document.h>
#include <ktexteditor/inlinenote.h>
#include <ktexteditor/inlinenoteprovider.h>
#include <ktexteditor/texthintinterface.h>
#include <ktexteditor/view.h>

#include <KLocalizedString>

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPointer>
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

// --- Hover hints over agent-edited lines --------------------------------------
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
    // KTextEditor has no view-about-to-be-destroyed signal; the attached views
    // are QPointer-tracked, and the still-live ones are unregistered here.
    for (const ViewAttachment &attachment : m_attachments) {
        if (attachment.view) {
            attachment.view->unregisterInlineNoteProvider(m_noteProvider);
            attachment.view->unregisterTextHintProvider(m_hintProvider);
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

    view->registerInlineNoteProvider(m_noteProvider);
    view->registerTextHintProvider(m_hintProvider);
    view->setTextHintDelay(kTextHintDelayMs);

    ViewAttachment attachment;
    attachment.view = view;
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
}

} // namespace KateAi

#include "editorassist.moc"

