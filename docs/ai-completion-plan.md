# AI in-editor completion — implementation plan

Plan for the highest-impact Kate integration that was deliberately not built
yet: AI completions inside the editor, plus agent-edit annotations. This is
the roadmap item behind "Code completion model / text hints" from the
integration review. The plan is implemented: `CompletionEngine` and
`EditRangeTracker` live in `kateai_core`, `EditorAssist` and the providers in
the `kateai` target; this document remains as the design record.

## Goal

- Typing (or an explicit trigger) offers an AI completion at the cursor,
  rendered inline, accepted with Tab.
- Lines the agent changed carry a hover hint ("edited by Kate AI") that leads
  to the review queue, so a change made from the panel is discoverable from
  the editor days later, not just in the turn that made it.

Both are opt-in and cost-controlled. The chat agent loop is not touched.

## Kate/KTextEditor surface (KF6, verified against the installed headers)

| Capability | API | Notes |
|---|---|---|
| Inline "ghost text" | `KTextEditor::InlineNoteProvider` + `View::registerInlineNoteProvider()` | KTextEditor has no native ghost-text API; an inline note at the cursor is the established emulation. Notes re-render through `inlineNotes()` on demand. |
| Completion popup (optional) | `KTextEditor::CodeCompletionModel` + `View::registerCompletionModel()` | Native popup; useful as an alternative render mode and for symbol-style completions. |
| Hover hints | `KTextEditor::TextHintProvider` + `View::registerTextHintProvider()` | `textHint(view, cursor)` returns per-line text; delay via `View::setTextHintDelay()`. |
| Insert on accept | `View::insertText()` | Already used by the suggestion flow's planned insert action. |
| Per-view lifecycle | `MainWindow::viewCreated`, `MainWindow::viewChanged` | No view-about-to-be-destroyed signal exists: track views with `QPointer` and unregister providers when the pointer nulls. |

## Architecture

New component `EditorAssist` (in the `kateai` target — it is pixels plus
view wiring; the requestable logic below goes into `kateai_core` so it stays
testable):

```
KateAiView
  └── EditorAssist (one per main window)
        ├── CompletionEngine      (kateai_core: context + request + cancel)
        ├── AiCompletionModel     (CodeCompletionModel, popup mode)
        ├── GhostNoteProvider     (InlineNoteProvider, inline mode)
        └── EditHintProvider      (TextHintProvider, agent-edited lines)
```

`EditorAssist` attaches to every `View` of its window on `viewCreated` and
re-attaches its providers when a view's document changes. It never talks to
`AgentLoop`: completions must not interleave with a running chat turn, and a
turn abort must not kill an unrelated completion.

### Phase 1 — CompletionEngine (core, testable, ~2 days)

1. **Context builder**: from a `Document` + cursor, build a FIM-style prompt:
   - prefix: up to N chars before the cursor (default 4000), snapped to line
     starts; suffix: up to M chars after (default 1500).
   - language from `Document::highlightingMode()`; file path; nothing else —
     completions are local, cheap and fast. No project graph, no rules.
2. **Request formats** per provider (matrix, mirroring `llmclient.cpp`):
   - OpenAI-compatible `/chat/completions` with a "continue the code, output
     only the completion" system prompt and `stop` sequences (works on every
     provider that already works for chat — this is the default path).
   - Optional FIM path (`prompt`/`suffix` completion endpoints) when a
     provider supports it; capability flag in `Provider` metadata.
   - A dedicated `completionModel` setting (a small fast model), separate
     from the chat model; defaults to the chat model when empty.
3. **Cancellation and staleness**: every request carries the cursor position
   it was built at; a response that arrives after the cursor moved is dropped
   without rendering. One in-flight request per view, cancelled on retrigger.
4. **Budgets**: separate `completionRequestsPerMinute` (default 20) and a
   hard off-switch while the chat agent is streaming (`AgentLoop::isBusy()`).
5. Tests: prompt building (pure), staleness handling, budget window — all in
   `kateai_core` with the existing mock-LLM harness (`tests/mock_llm_server.py`).

### Phase 2 — inline rendering (~1 day)

1. `GhostNoteProvider` shows the pending suggestion as a translucent inline
   note right of the cursor, styled from `ChatTheme` muted colors; dimmed
   (not syntax highlighted) to read as a proposal.
2. Accept: Tab inserts the text via `View::insertText()` and clears the note;
   Esc or any typing clears it. Tab-accept must not fight Kate's own Tab:
   install as a `QShortcut` on the view with `Qt::WidgetWithChildrenShortcut`
   context only while a suggestion is showing.
3. Optional popup mode via `AiCompletionModel` for users who prefer the native
   completion list (setting `completionRenderMode`: `inline` | `popup`).

### Phase 3 — agent-edit text hints (~0.5 day)

1. `EditTracker` entries already hold per-path diffs; add a helper that maps
   a group's `combinedDiff()` hunks to line ranges (`QList<Range>`), with the
   ranges invalidated when the file is edited beyond them (track via
   `MovingCursor` per hunk start — KTextEditor keeps them correct under
   user edits).
2. `EditHintProvider::textHint()` returns "Kate AI edited this — N pending"
   for cursors inside a range of a document with a pending entry, and "edited
   by Kate AI (turn finished)" for ranges of the last settled turn (kept for
   one session, cleared on conversation switch — the tracker already clears
   then).
3. The hint is passive text; the banner's Review action keeps the click path.

### Phase 4 — settings and UI (~0.5 day)

New "Completion" section in the config page (follows the existing checkbox
pattern added for buffer mode):

- `completionEnabled` (default **false**)
- `completionTrigger`: `manual` (default; shortcut, e.g. `Alt+\`) | `auto`
  (idle timer, default 800 ms, only when the document is clean of a running
  turn)
- `completionModel`, `completionRequestsPerMinute`,
  `completionContextPrefixChars` / `...SuffixChars`

### Phase 5 — tests and rollout (~1 day)

- `tests/test_completionengine.cpp` (core): prompt, staleness, budget.
- `tests/test_editranges.cpp` (core): diff→range mapping, MovingCursor
  invalidation.
- Widget tests offscreen for the note provider (pattern: `test_livecards`).
- Manual pass: latency feel with a local model, Tab conflict audit, vi-mode
  insert compatibility.

## Risks and decisions to make before starting

1. **Tab handling** is the riskiest UX surface: vi-mode and snippets also
   want Tab. Alternative accept key (`Ctrl+Right` or `End`)? Decide during
   phase 2 with a spike in both modes.
2. **Provider cost surprise**: auto mode on a metered key can fire 100+
   requests/hour. The budget must be enforced in `CompletionEngine`, not the
   UI, and the first run with auto mode should show a one-time notice.
3. **No streaming into ghost text in v1**: render once when the response
   completes; per-token re-rendering of inline notes is measurable flicker.
   Streaming completions are a follow-up.
4. **Completion while the file has unsaved user edits**: allowed (context is
   the buffer, which is what the user sees), but completions are never
   auto-inserted — accept is always an explicit key press.

## Sequencing

Phase 1 and 3 are independent. Phase 2 depends on 1, phase 4 on 1–2, phase 5
runs alongside. A first shippable slice is phases 1+2 in manual-trigger-only
mode with a single provider; that is also the point to re-evaluate the
risk list with real usage.
