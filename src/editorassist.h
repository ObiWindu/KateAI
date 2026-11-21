/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "completionengine.h"
#include "debug.h"
#include "editranges.h"
#include "lsp.h"
#include "types.h"

#include <KTextEditor/Range>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QShortcut;
class QTimer;

namespace KTextEditor
{
class View;
}

namespace KateAi
{

class GhostNoteProvider;
class DebugInlineNoteProvider;
class EditHintProvider;
class AiCompletionModel;
class AiEditAnnotationModel;

/**
 * The in-editor half of AI completions, agent-edit hints, live debugger
 * inline values/hover hints, and toggleable AI-edit gutter annotations.
 * One instance per main window. Attaches to every View of its window
 * (attachView() on viewCreated).
 *
 * Never talks to AgentLoop: it only observes a busy flag and a busy check,
 * so a completion can never interleave with a running chat turn and a turn
 * abort never kills an unrelated completion.
 */
class EditorAssist : public QObject
{
    Q_OBJECT

public:
    explicit EditorAssist(QObject *parent = nullptr);
    ~EditorAssist() override;

    void setSettings(const Settings &settings);
    // Chat turn running: completions are switched off while this is true.
    void setAgentBusy(bool busy);
    // Cheap check run before every request (AgentLoop::isBusy()).
    void setBusyCheck(const std::function<bool()> &check);

    // Registers providers on the view; safe to call twice for one view.
    void attachView(KTextEditor::View *view);

    // Manual trigger (the completion action); also what the auto idle timer
    // ends up calling.
    void triggerCompletion(KTextEditor::View *view);

    // --- Suggestion state, read by the render providers ---------------------
    bool hasSuggestion() const
    {
        return !m_suggestion.isEmpty();
    }
    QString suggestionText() const
    {
        return m_suggestion;
    }
    // The line and column the showing suggestion was built for.
    int suggestionLine() const;
    int suggestionColumn() const;

    // Accept (Tab): insert at the cursor and clear. No-op without a live
    // suggestion at the current position.
    void acceptSuggestion(KTextEditor::View *view);
    // Drop the suggestion: Esc, cursor moved, or any typing.
    void clearSuggestion();

    // --- Agent-edit hover hints & gutter annotations ------------------------
    // Pushes the review queue's line ranges for one path into the anchored
    // range tracker, with the document they live in.
    void updateEditRanges(const QString &path,
                          const QList<KTextEditor::Range> &pending,
                          const QList<KTextEditor::Range> &settled);
    EditRangeTracker *editRanges() const
    {
        return m_editRanges;
    }

    void setAnnotationGutterEnabled(bool enabled);
    bool annotationGutterEnabled() const
    {
        return m_annotationGutterEnabled;
    }

    // The editor's language-server surface, used to show diagnostic hovers.
    // Null in tests: the hint then just skips the diagnostic part.
    void setLspProvider(LspProvider *provider)
    {
        m_lsp = provider;
    }
    LspProvider *lspProvider() const
    {
        return m_lsp;
    }

    // The editor's live debugger surface, used for inline variable values and
    // hover tooltips while the debuggee is paused.
    void setDebugProvider(DebugProvider *provider);
    DebugProvider *debugProvider() const
    {
        return m_debug;
    }

    // Live cached debug state read by DebugInlineNoteProvider & EditHintProvider.
    bool hasLiveDebugOverlay() const
    {
        return m_debugPaused && !m_debugPausedFile.isEmpty() && !m_debugLocalsByName.isEmpty();
    }
    QString debugPausedFile() const
    {
        return m_debugPausedFile;
    }
    int debugPausedLine() const
    {
        return m_debugPausedLine;
    }
    const QHash<QString, DebugVariable> &debugLocalsByName() const
    {
        return m_debugLocalsByName;
    }

Q_SIGNALS:
    // The auto trigger is about to fire for the first time this session; the
    // view shows the one-time notice about request volume.
    void autoModeFirstRun();

private:
    struct ViewAttachment
    {
        QPointer<KTextEditor::View> view;
        QList<QMetaObject::Connection> documentConnections;
        QMetaObject::Connection cursorConnection;
        QPointer<DebugInlineNoteProvider> debugNoteProvider;
        QPointer<AiEditAnnotationModel> annotationModel;
    };

    void requestCompletion(KTextEditor::View *view);
    void onCompleted(const QString &text, const CompletionPromptContext &context);
    void onEngineFailed();
    void showSuggestion(const QString &text, const CompletionPromptContext &context);
    void installAcceptShortcuts(KTextEditor::View *view);
    void removeAcceptShortcuts();
    void scheduleAutoTrigger(KTextEditor::View *view);
    void refreshDebugOverlays();
    void applyAnnotationGutter(ViewAttachment &attachment);
    ViewAttachment *findAttachment(KTextEditor::View *view);

    Settings m_settings;
    bool m_agentBusy = false;
    bool m_annotationGutterEnabled = false;
    std::function<bool()> m_busyCheck;

    CompletionEngine *m_engine = nullptr;
    EditRangeTracker *m_editRanges = nullptr;
    LspProvider *m_lsp = nullptr;
    DebugProvider *m_debug = nullptr;
    QList<QMetaObject::Connection> m_debugConnections;
    GhostNoteProvider *m_noteProvider = nullptr;
    EditHintProvider *m_hintProvider = nullptr;
    AiCompletionModel *m_popupModel = nullptr;

    bool m_debugPaused = false;
    QString m_debugPausedFile;
    int m_debugPausedLine = 0;
    QHash<QString, DebugVariable> m_debugLocalsByName;

    QPointer<KTextEditor::View> m_suggestionView;
    QString m_suggestion;
    CompletionPromptContext m_suggestionContext;

    QPointer<QShortcut> m_tabShortcut;
    QPointer<QShortcut> m_escShortcut;

    QTimer *m_autoTimer = nullptr;
    QPointer<KTextEditor::View> m_autoView;
    bool m_autoNoticeFired = false;

    QList<ViewAttachment> m_attachments;

    friend class GhostNoteProvider;
    friend class DebugInlineNoteProvider;
    friend class EditHintProvider;
    friend class AiEditAnnotationModel;
};

} // namespace KateAi
