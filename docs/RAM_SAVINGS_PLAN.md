# RAM Savings Implementation Plan

Status: **implemented and verified** — this file is both the plan and the progress log.

Scope: `feature/harness_complete` only. No code, design, or commit from
`telegram_bot_bridge` is used. The implementation is incremental: each item is
independently testable and revertable, and normal-sized sessions keep their
current behavior.

## Safety rules

- Caps are safety ceilings, not new normal limits; they must not change ordinary
  tool output, transcripts, or save behavior.
- Full tool output remains reachable through the content store when a display or
  history copy is bounded.
- No wholesale transcript virtualizer, graph-storage rewrite, or session-schema
  rewrite.
- No new synchronous multi-megabyte reads or serializations on the GUI thread.
- Every phase must pass the existing test suite plus new memory tests.

## Work items and progress

### Phase 0 — Baseline and guardrails

- [x] Add `tests/test_memory_budget.cpp` with child-process RSS scenarios.
- [x] Record baseline save/restore/render timings in `docs/MEMORY_BASELINE.md`.
- [x] Add optional runtime counters for retained bytes/records (`AgentLoop::memoryCounters`).

### Phase 1 — Low-risk removals

- [x] Remove dead `m_actionSignatures`; fingerprint repeat-detection keys.
- [x] Clear sub-agent `record.patch` after spilling and after apply/discard.
- [x] Clear `ChatWidget::m_pendingToolCalls` when a turn ends.
- [x] Parent per-menu `QActionGroup`s to the menu.
- [x] Delete/reuse `McpClient::m_startupTimer`.
- [x] Clear `m_pinnedViewportWidget` on transcript teardown.
- [x] Remove the unused leaking `markdownToHtml()` helper.
- [x] Remove editor attachment/LSP mark entries on document/view destruction.
- [x] Release `EditRangeTracker` moving cursors on document destruction.

### Phase 2 — Protocol and large-file caps

- [x] Cap ACP tool results before adding them to history/transcript.
- [x] Add ACP live-tool, stream, and terminal aggregate byte budgets.
- [x] Add LLM SSE/assistant/tool-call accumulator caps.
- [x] Bound UI `rawOutput` for native/MCP/ACP results.
- [x] Stream `read_file` ledger hashing; stat before reading.
- [x] Guard ACP write/edit inline diffs by file size.
- [x] Bound/stream `SubagentWorktree::diff` (32 MB hard cap).
- [x] Stream hook and check output up to a bounded 8 MB drain cap.

### Phase 3 — Transcript and session

- [x] Count tool arguments and plans in `transcriptMessageChars`.
- [x] Avoid serializing an identical transcript twice.
- [x] Add dirty-flag/less-frequent session autosave.
- [x] Omit empty fields from serialized messages.
- [x] Page event-log reads for trace/export/projection.
  - Export, fork-copy and projection rebuild now page through the log.
  - The trace report is bounded to the most recent 2,000 events; export remains
    the complete-data path.
  - `traceSpans` still uses one pass because span parenting needs event
    continuity; it is only exercised by explicit telemetry export.
- [x] Select only titles when a session title is valid.
- [x] Stream `ToolResults::read` pages instead of loading the whole store.

### Phase 4 — UI retention and rendering

- [x] Cap or coalesce activity pills.
- [x] Apply live caps to restored sub-agent and suggestion cards.
- [x] Remove answered question cards from live routing maps.
- [x] Lazy-render large tool/sub-agent results (bounded formatting path).
- [x] Back off large-answer streaming rendering.
- [x] Throttle Inspector refresh and stop reloading skills in reports.

### Phase 5 — Edit contents

- [x] Move large old/new edit contents to a bounded blob store.
- [x] Cap pending edits by count and bytes (64 entries, 4 MB baseline per file).

### Phase 6 — Graph and steady-state

- [x] Reduce project-graph previews to 4 KB.
- [x] Coalesce graph saves; stop saving on every typing pause.
- [x] Stream graph directory scanning with `QDirIterator`.

### Phase 7 — Follow-up audit hardening

- [x] Add a hard byte/count ceiling for the model history, trimmed only at
  user-turn boundaries.
- [x] Cap native/ACP reasoning and ACP answer buffers.
- [x] Cap sub-agent record summaries, streamed text and sub-task card buffers.
- [x] Bound automatic OTLP export to recent events and add a transfer timeout.
- [x] Cap changed-path, settled-range and sub-agent cost bookkeeping.
- [x] Bound `ProjectGraph::updateGraph` input and skip oversized editor
  documents.
- [x] Reject whole-file edits above 16 MB and cap `apply_patch` working sets.
- [x] Clear Inspector documents when hidden.
- [x] Cap concurrent ACP server sessions.
- [x] Extend `memoryCounters` with message/sub-agent summary bytes.

### Phase 8 — Structural follow-up

- [x] Share the project graph, MCP manager and event store per workspace across
  windows (`WorkspaceServices`); views hold the shared reference.
- [x] Fix `m_ownsMcp` ownership and make project MCP configuration load only
  after the user confirms it.
- [x] Add an incremental session message journal for large conversations
  (threshold 200 messages, full rewrite on compaction/prune).
- [x] Retain the newest 20,000 events per conversation.
- [x] Defer the remote ACP registry fetch until its settings tab is opened.
- [x] Read ACP stdout in bounded chunks; cap headless workspace diffs.
- [x] Bound policy, hook and ecosystem config reads with a shared helper.

### Phase 9 — Remaining audit follow-up

- [x] Strip reasoning from model-history turns older than two, on both append
  and restore, while the display transcript keeps its copy (P4).
- [x] Reject oversized non-streaming HTTP bodies before the copy and the JSON
  parse: completions (256 KB), model lists and the ACP registry (4 MB) (F9, P10).
- [x] Lower the ACP ceilings to 8 MiB frames, 128 live tools / 8 MiB, 32 parked
  methods / 8 MiB, a 2 MiB single payload and 16 MiB file reads (F5).
- [x] Stream the voice multipart upload from disk instead of copying the
  recording into a body part (T9).
- [x] Iterate the sub-agent map directly in `sessionData()` (P11).
- [x] Wire `TranscriptVirtualizer` for restored transcripts, keeping the live
  append path unchanged (P3).
- [x] Extend `memoryCounters` with `thinkingChars`, `liveBufferChars` and
  `eventSeq`, and cover reasoning retention in `test_memory_budget` (F10).
- [x] Build `KateAiConfigPage` tabs lazily so opening Settings costs one tab
  instead of twelve, and keep stored values for tabs that were never opened
  (T6).
- [x] Re-render a sub-agent's streamed answer at most every 80 ms instead of
  rebuilding its Markdown document once per delta (T12).
- [x] Reap ACP server sessions that have no prompt in flight and no client
  traffic for 15 minutes (F6).
- [x] Express the transcript card budgets as byte budgets converted once through
  a conservative cost factor, and share one plugin network manager with the
  settings page and the ACP installer (P2, P9).
- [x] Build the project graph's forward adjacency from its edges on demand
  instead of retaining a second copy of every dependency, and give a forked
  sub-agent the parent's graph, event store and MCP manager (P7, P8).
- [x] Prune edit-tracker snapshots as edits settle, bound the remaining
  workspace-owned reads (`.gitignore`, `package.json`, blobs), and stop
  `memoryCounters()` from running a query (P10, P11).

## Progress log

- 2026-11-25: Plan created. Starting Phase 1 implementation.
- 2026-11-25: Implemented Phase 1 removals and several Phase 2/3 items:
  - action signatures are SHA-256 fingerprints; the dead full-argument set is
    no longer written to sessions;
  - sub-agent patches are cleared after a successful spill and after
    apply/discard;
  - pending edit snapshots are cleared at turn end;
  - menu action groups, MCP startup timer, pinned viewport pointer, editor
    attachments, and LSP mark counts are cleaned up;
  - ACP tool output is bounded to 80 KB in history and 1 MB for display, with
    the full result in the content store;
  - native/MCP UI output is bounded to 1 MB;
  - `read_file` no longer reads files above the ledger limit;
  - ACP write/edit diffs skip large existing files;
  - transcript budgeting includes tool arguments and plans;
  - an identical transcript is not serialized twice;
  - session autosave is dirty-flagged and reduced from 1.5 s to 5 s;
  - serialized messages omit empty fields;
  - `ToolResults` pages stream from disk and use a `.len` sidecar for total
    characters;
  - activity pills are capped at 50;
  - restored sub-agent/suggestion cards use the live caps;
  - large tool-card result formatting is capped at 256 KB;
  - project-graph previews are 4 KB and graph writes are coalesced to 30 s /
    turn boundaries;
  - graph directory scanning is iterative (`QDirIterator`) rather than
    materializing a full `QFileInfoList`;
  - Inspector refreshes are coalesced to 250 ms and no longer reload the skill
    registry from a report;
  - ACP live-tool and retained-terminal counts were reduced, and per-terminal
    output is capped at 1 MB;
  - pending-edit snapshots above 4 MB are not copied; Reject safely reports
    that the baseline was not retained or uses the checkpoint;
  - the trace report reads only the newest 2,000 events;
  - `runGit` drains output while running and rejects over 32 MB instead of
    materializing an unbounded diff;
  - hooks and verification checks drain incrementally and stop at an 8 MB hard
    cap (their existing display caps still apply);
  - answered question cards leave the live routing map but stay in the
    transcript;
  - pending edits are capped at 64 and large baselines are no longer copied;
  - large streaming answers back off to at most one full render per second.
- 2026-11-25 (continued):
  - added `tests/test_memory_budget.cpp`; measured restore, write-heavy
    transcript, tool-result paging and graph-scan budgets in
    `docs/MEMORY_BASELINE.md`;
  - added `AgentLoop::memoryCounters()` for retained-state diagnostics;
  - exported traces page through the event log; fork-copy and projection
    rebuild also page;
  - ACP live-tool details and streamed text now have aggregate byte budgets,
    and their counters are reset with the live maps;
  - `EditRangeTracker` releases its moving cursors on document destruction;
  - added the memory-budget test to CMake/CTest.
- 2026-11-25 (final):
  - `EditTracker` now writes old/new contents above 256 KB to a temporary
    owner-only blob directory and resolves them only for review/undo; a new
    test covers a 300 KB old/new edit through blob storage;
  - export traces, conversation fork-copy and projection rebuild page through
    the event log; the trace report is bounded to the newest 2,000 events;
  - ACP live-tool details/streams have aggregate byte budgets and their
    counters reset with the live maps.
- Final verification: full build succeeds and **all 80 CTest tests pass**
  (`ctest --test-dir build --output-on-failure`, 48.58 s), including the new
  memory-budget target. `test_memory_budget` reports:
  - restore 2,000 × 4 KB: ~5.1 MB RSS delta (budget 40 MB),
  - 100 KB tool arguments: transcript bounded to 75 messages / ~7.5 MB,
  - 8 MB tool-result page: 0-byte measured page delta,
  - 3,000-file graph scan: ~0.13 MB delta, 1,000-node cap.
- `git diff --check` is clean. All 38 original plan checkboxes are complete;
  Phase 7/8 add the follow-up hardening and structural items.

- 2026-11-25 (follow-up audit):
  - the model history now has an independent 16 MB / 3,000-message safety
    ceiling; trimming cuts only at user-turn boundaries and preserves the
    system prompt and compaction summary;
  - reasoning is capped at 1 MB in `AgentLoop` and `AcpClient`; ACP answers are
    capped at 4 MB;
  - sub-agent summaries are capped at 16 KB, child stream buffers at 256 KB and
    sub-task card live text at 256 KB (pending text 64 KB);
  - automatic OTLP export reads at most the newest 5,000 events, sets a 30 s
    transfer timeout and releases the batch when retries are exhausted;
  - changed paths (4,096), settled edit ranges (64 per path) and per-sub-agent
    cost entries (256) are capped;
  - project-graph updates analyse at most 1 MB and skip files above 4 MB;
  - native and ACP whole-file edit tools reject files above 16 MB;
    `apply_patch` caps the old+new working set at 32 MB;
  - the Inspector clears its text documents on hide, and the ACP server caps
    concurrent sessions at 64;
  - session autosave while streaming moved from 5 s to 15 s; the completed turn
    is still saved synchronously;
  - `memoryCounters` now reports `messageChars` and `subagentSummaryChars`, and
    `test_memory_budget` covers an oversized restored history.

- 2026-11-25 (structural pass):
  - `WorkspaceServices` now owns the project graph, MCP manager and event store
    for a workspace; `KateAiPlugin` keeps weak references and every view on the
    same folder shares one instance. Closing the last view releases the graph,
    stops the MCP servers and closes the event connection.
  - `AgentLoop` adopts the shared services before `setWorkspace()`, so the
    already-built graph is not indexed a second time. A mid-turn workspace
    switch is deferred to the turn boundary to keep in-flight tool pointers
    valid.
  - `m_ownsMcp` is now true for a loop's constructor-created manager, and the
    default `m_loadWorkspaceConfig` is false: a project's `mcp.json` starts
    servers only after the view's confirmation, including for shared managers.
  - large sessions (200+ messages) store their message lists in
    `session_message_chunk` and append only the new tail on autosave. The
    payload keeps the metadata, title repair reads the journal, and compaction
    or transcript pruning triggers one full revision. The journal is deleted
    with its conversation.
  - the session schema version is 2 and adds only the chunk table.
  - conversation event logs retain the newest 20,000 events; automatic OTLP
    export already reads the newest 5,000.
  - the ACP registry is fetched when its settings tab is first opened, and ACP
    stdout is drained in 64 KiB chunks.
  - headless `gitDiff` caps the patch at 32 MB and skips untracked files above
    8 MB.
  - policy, hook and ecosystem config reads use `readBoundedFile()`.

- 2026-11-26 (remaining audit follow-up):
  - reasoning is stripped from model-history messages further back than two
    user turns, both when a turn appends and when a session restores; the
    display transcript keeps its own copy, so scrollback is unaffected. A
    restored session is measured by `thinkingChars` in `test_memory_budget`;
  - `boundedfile.h` gained `replyBodyWithinLimit()` and it is applied before
    every non-streaming body copy: completions (256 KB), model lists (4 MB) and
    the ACP registry in both the settings page and the update check (4 MB);
  - the ACP ceilings were lowered to 8 MiB frames, 128 live tools / 8 MiB,
    32 parked methods / 8 MiB, a 2 MiB single parked payload and 16 MiB for
    `fs/read_text_file`, with the user-facing messages updated to match;
  - the voice upload streams the recording from disk through
    `QHttpPart::setBodyDevice()` instead of holding a second full copy;
  - `sessionData()` iterates `m_subagentRecords` directly rather than copying
    and sorting it;
  - `memoryCounters` also reports `thinkingChars`, `liveBufferChars` and
    `eventSeq`.

- 2026-11-26 (settings page tabs):
  - `KateAiConfigPage` now lists all twelve tab labels up front but builds a
    tab's widgets the first time that tab is shown; the ACP registry fetch
    follows its tab, as before;
  - `apply()` guards each section with `isTabBuilt()`, so a tab the user never
    opened keeps its stored values instead of writing widget defaults, and the
    workspace `mcp.json` is only rewritten when the MCP tab was loaded;
  - `tests/test_configpage.cpp` (added to CTest) asserts every label is present,
    that opening the page builds exactly one tab, that visiting a tab
    materialises it, and that unvisited fields survive Apply;
  - all 82 CTest tests pass.

- 2026-11-26 (retained memory and per-loop resources):
  - the transcript caps are byte budgets converted once through
    `RetainedCost`: a single conservative bytes-per-character factor replacing
    the 2 MiB and 6 MiB character figures that counted a `QTextDocument` as if
    it were a plain string. The factor is a deliberate upper estimate, not a
    calibration; measuring the real bytes-per-char is still open (P2);
  - a sub-agent's streamed answer is accumulated and its Markdown document
    rebuilt at most every 80 ms instead of once per delta; a finished or
    abandoned card discards the pending render (T12);
  - an ACP session with no prompt in flight and no unanswered permission or
    question is closed after 15 minutes of silence, swept once a minute (F6);
  - `KateAiPlugin` owns one `QNetworkAccessManager` and the update check, the
    ACP installer and the settings page share it, so the per-dialog connection
    pool is gone (P9);
  - the project graph's forward adjacency is derived from `m_edges` on demand
    and dropped when the edges change, instead of retaining a copy of every
    dependency beside them (P7);
  - a forked sub-agent adopts the parent's project graph, event store and MCP
    manager, so parallel children do not index the workspace again or open one
    SQLite connection each (P8);
  - `AgentLoop` tracks the newest event sequence it appended, so the polled
    `memoryCounters()` no longer runs a query; edit-tracker snapshots are pruned
    when edits settle rather than at `clear()`, and the `.gitignore`,
    `package.json` and blob reads go through `readBoundedFile()` (P10, P11).
