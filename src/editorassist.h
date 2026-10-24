/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "completionengine.h"
#include "editranges.h"
#include "types.h"

#include <KTextEditor/Range>

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
class EditHintProvider;
class AiCompletionModel;

/**
 * The in-editor half of AI completions and agent-edit hints, one instance per
 * main window. Attaches to every View of its window (attachView() on
 * viewCreated), renders suggestions inline at the cursor or in the native
 * completion list, and answers hovers over lines the agent edited.
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

    // --- Agent-edit hover hints ----------------------------------------------
    // Pushes the review queue's line ranges for one path into the anchored
    // range tracker, with the document they live in.
    void updateEditRanges(const QString &path,
                          const QList<KTextEditor::Range> &pending,
                          const QList<KTextEditor::Range> &settled);
    EditRangeTracker *editRanges() const
    {
        return m_editRanges;
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
    };

    void requestCompletion(KTextEditor::View *view);
    void onCompleted(const QString &text, const CompletionPromptContext &context);
    void onEngineFailed();
    void showSuggestion(const QString &text, const CompletionPromptContext &context);
    void installAcceptShortcuts(KTextEditor::View *view);
    void removeAcceptShortcuts();
    void scheduleAutoTrigger(KTextEditor::View *view);
    ViewAttachment *findAttachment(KTextEditor::View *view);

    Settings m_settings;
    bool m_agentBusy = false;
    std::function<bool()> m_busyCheck;

    CompletionEngine *m_engine = nullptr;
    EditRangeTracker *m_editRanges = nullptr;
    GhostNoteProvider *m_noteProvider = nullptr;
    EditHintProvider *m_hintProvider = nullptr;
    AiCompletionModel *m_popupModel = nullptr;

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
    friend class EditHintProvider;
};

} // namespace KateAi
