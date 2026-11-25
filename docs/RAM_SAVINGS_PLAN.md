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
- `git diff --check` is clean. All 38 plan checkboxes are complete.
