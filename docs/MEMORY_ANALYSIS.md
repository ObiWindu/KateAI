# KateAI Memory Analysis

Status: analysis only. No code was changed for this report.
Branch: `feature/memory-savings` (HEAD `6992fb5`, "perf: bound retained memory across
transcripts, tools and sessions").
Scope: the whole `src/` tree — plugin UI, `AgentLoop`, ACP client/server, MCP,
tools, document bridge, session/event stores, project graph, harness, headless,
voice input, completion and the per-window lifecycle.

This report deliberately **excludes** the 38 mitigations that `docs/RAM_SAVINGS_PLAN.md`
already records as implemented. Where a remaining finding interacts with one of
those mitigations, the interaction is called out explicitly.

Severity is about user-visible RAM growth or a multi-megabyte allocation on the
GUI thread, not about correctness, unless stated.

## Follow-up implementation status

The low-risk, high-impact findings were implemented after this analysis; the
original text below is kept as the audit record. See the Phase 7 entries in
`docs/RAM_SAVINGS_PLAN.md` for the exact list.

| Finding | Status |
| --- | --- |
| F1 model-history ceiling | Implemented: 16 MB / 3,000-message target, trimmed only at user-turn boundaries. |
| F2 unbounded reasoning/text | Implemented: 1 MB reasoning in both clients, 4 MB ACP answer. |
| F3 sub-agent records | Implemented: 16 KB summary, 256 KB child stream. |
| F4 bookkeeping sets | Partially implemented: changed paths, settled ranges and sub-agent cost entries capped. |
| F5 ACP ceilings | Implemented: 8 MiB frames, 128 live tools / 8 MiB live-tool bytes, 32 parked methods / 8 MiB, 2 MiB single payload, 16 MiB file reads. |
| F6 ACP server sessions | Implemented: 64 concurrent sessions, plus a 15-minute idle reap that closes a session with no prompt in flight and no unanswered permission or question. |
| F7 Inspector retention | Implemented: documents cleared on hide. |
| F8 MCP ownership | Implemented: the constructor-owned manager sets `m_ownsMcp`; project config loads only after confirmation; shared managers use the confirmed state. |
| F9 completion body | Implemented: completion, model-list and ACP-registry replies are rejected above their byte cap before the copy and the JSON parse. |
| F10 memory counters | Implemented: `messageChars`, `thinkingChars`, `liveBufferChars`, `eventSeq`, `subagentSummaryChars`, plus restore and reasoning-retention tests. |
| T1 autosave | Implemented for large sessions: a message journal appends only the new tail; small sessions keep the single payload. |
| T2 restore peak | Mitigated by the journal: large sessions load chunked lists instead of one payload array. |
| T3 OTLP export | Implemented: newest 5,000 events, 30 s timeout, batch released on final failure. |
| T4 whole-file edits | Implemented: 16 MB file limit, 32 MB patch working set. |
| T5 graph typing update | Implemented: 1 MB analysis cap and 4 MB editor skip. |
| T6 ConfigPage tabs | Implemented: a tab's widgets are built the first time it is shown, including the remote ACP registry fetch; an unbuilt tab's stored fields survive Apply. |
| T7 compaction copies | Not changed. |
| T8 clear-all payloads | Mitigated: the cleanup path still reads payloads, but large message lists no longer live there. |
| T9 voice WAV copies | Implemented: the multipart upload streams the recording from disk instead of copying it into a body part. |
| T10 ACP stdout | Implemented: stdout is read in bounded 64 KiB chunks. |
| T11 streaming copies | Mitigated by the reasoning/answer caps. |
| T12 sub-task render | Implemented: live text capped, and the streamed answer is re-rendered at most every 80 ms instead of once per delta. |
| T13 headless diff | Implemented: 32 MB diff cap and 8 MB untracked-file skip. |
| P1 per-workspace services | Implemented: graph, MCP manager and event store shared by every window on a workspace (`WorkspaceServices`). |
| P5 session journal | Implemented: threshold-gated incremental message journal with a full rewrite on compaction. |
| P6 event-log retention | Implemented: 20,000 events per conversation, newest-window auto export. |
| P3 transcript virtualiser | Implemented: wired for restored transcripts, with a 256-entry window; the live append path is unchanged. |
| P4 reasoning retention | Implemented: reasoning is stripped from model-history turns older than `kThinkingRetainedTurns` (2), on both append and restore; the display transcript keeps its copy. |
| P2 card budgets | Partially implemented: the transcript caps are byte budgets converted once through a conservative `RetainedCost` factor. Calibration against measured bytes-per-char and per-card lazy rendering remain. |
| P7 graph layout | Partially implemented: the hand-maintained adjacency map is derived from the edges on demand and dropped when they change. Interned ids/paths and lazy symbols remain. |
| P8 per-loop event store | Partially implemented: a forked sub-agent shares the parent's event store, project graph and MCP manager; each root loop still opens its own connection. |
| P9 shared network managers | Implemented: the plugin owns one `QNetworkAccessManager` shared by the update check, the ACP installer and the settings page, which falls back to its own only when it has no plugin. |
| P10 bounded readers | Partially implemented: policy/hooks/ecosystem, the graph `.gitignore`, `package.json` and edit-tracker blobs use the file helper; HTTP replies are capped before they are copied. |
| P11 minor savings | Implemented: `sessionData()` iterates the sub-agent map directly instead of copying and sorting it, and edit-tracker snapshots are pruned as edits settle rather than at `clear()`. |
| Remaining structural | The calibration and per-card lazy rendering of P2, interned graph ids with lazy symbols (P7), one event-store connection per conversation (P8) and the graph-layout rewrite are still open. |

| Severity | Meaning |
| --- | --- |
| **Critical** | Grows without a bound for the lifetime of a conversation (in a supported configuration), or repeats a transient comparable to the whole retained history on a common path. |
| **High** | Grows linearly with conversation length/tool use, or a repeated multi-MiB spike on a common path. |
| **Medium** | Bounded but with a ceiling far above normal use, or a spike limited to a specific long-running action. |
| **Low** | Small/rare, dead code, or a hardening gap. |

Confidence is `confirmed` when the code path and the absence of a bound were read
end-to-end, `likely` when a bound may exist outside the inspected path.

---

## Executive summary

The dominant structural fact is that **the display transcript is bounded, but the
model history is not**. Commit `6992fb5` capped the widgets (`ChatWidget` cards),
the persisted transcript (`AgentLoop::m_transcriptMessages`) and many protocol
buffers. `AgentLoop::m_messages`, the list that feeds the model and the session
payload, still only shrinks when context compaction decides to adopt a smaller
history. That leaves several independent ways for a long conversation to keep
growing.

The second structural fact is **per-window duplication**: every Kate main window
constructs a `ChatWidget` (a value-member `AgentLoop`) with its own project graph,
MCP manager, ACP client, LLM client, web-search client, memory loader and tool
registries. Sub-agents reuse the graph and MCP manager but create the rest again.

Top findings, in priority order:

1. **`m_messages` has no hard cap** and only shrinks when compaction saves ≥30 %
   (or a summary changed). With auto-compaction disabled it never shrinks.
2. **Reasoning/text stream buffers are unbounded** in `AgentLoop` and, unlike the
   native client, in `AcpClient`. They are persisted into the session payload.
3. **Per-turn telemetry export reads the entire event log** (when telemetry is
   enabled and an endpoint is set) and builds a full `Span` list, then exports it
   without a network timeout.
4. **Session autosave serialises the whole conversation every 5 s while
   streaming**, producing a repeated multi-MiB allocation + SQLite bind on the
   GUI thread.
5. **Full-file reads in edit/write/patch tools** are unbounded, and `apply_patch`
   holds old + new content for every file in the patch simultaneously.
6. **Sub-agent records retain full final reports** and the child stream buffers
   are unbounded.
7. **ACP protocol ceilings are very high** (32 MiB live tools, 32 MiB parked
   methods, 16 MiB frame, 64 MiB file read), so a broken/hostile agent can still
   force large peaks.
8. **Every window duplicates the harness** (graph, MCP, ACP, QNAMs, registries);
   a 4-window Kate session has 4 independent copies.
9. **An ownership bug (`m_ownsMcp` is never true)** leaves the per-loop MCP
   manager dead and, more importantly, means workspace MCP configuration is never
   loaded for a normal chat window. Fixing it naively would activate the
   per-window MCP process duplication described above.
10. **`TranscriptVirtualizer` exists, compiles, and is never used.** The intended
    transcript-virtualisation mitigation is inactive.

---

## 1. Confirmed retained growth (leaks and unbounded retention)

### F1 — Model history `m_messages` is not hard-capped — **Critical**

**Where:** `src/agentloop.cpp:4631-4637` (`appendMessage`), `4639-4684`
(`pruneTranscript`), `src/agentloop.h:713`; compaction adoption is
`src/agentloop.cpp:4090-4152`; the ACP trigger is `5365-5367`.

**Mechanism.** `appendMessage()` appends every message to **both**
`m_messages` (model history) and `m_transcriptMessages` (display history), then
calls `pruneTranscript()`. That function only trims `m_transcriptMessages`
against the 1000-message / 8 MiB caps (`agentloop.cpp:96-97`). `m_messages` is
never trimmed there. It shrinks only through `maybeCompactStoredHistory()`, which:

- returns immediately when `m_settings.compressOldMessages` is false
  (`agentloop.cpp:4092-4094`);
- refuses to adopt the compacted request unless `summaryChanged` **or** the
  request saves at least 30 % of the stored tokens
  (`agentloop.cpp:4147-4152`);
- is only called for native requests (`agentloop.cpp:4288`) and, for ACP, once
  per turn when `m_messages.size() > 200` (`5365-5367`).

**Consequences.**

- A long native conversation keeps every message until the context window is
  nearly full, then keeps the compacted form. That is bounded by the window, but
  a 1M-token model can retain roughly 4 million characters of prose (about 8 MB
  in `QString`'s UTF-16) plus tool arguments and reasoning, i.e. tens of MiB. The
  display transcript is capped at 8 MiB, so the model history can be several
  times larger than the visible transcript.
- With auto-compaction disabled (a supported setting, `types.h:465`,
  `settings.cpp:393`), `m_messages` grows for the whole conversation, forever.
  Requests are still compacted on overflow (`onFailed()` increments
  `m_compactionLevel`), but `maybeCompactStoredHistory()` returns at the first
  check, so the stored list never shrinks.
- ACP sessions compact only at turn boundaries and only when >200 messages. A
  single turn producing many tool messages is not bounded at all until it ends.

**Why the 30 % gate is not protective.** When `ContextEngine` drops messages and
produces a summary, `summaryChanged` is true and `persistent.size() <
m_messages.size()`, so the gate is bypassed (`summaryDroppedMessages`,
`agentloop.cpp:4143-4152`). The gate only blocks the "request was slightly
smaller but no summary changed" case. So the practical growth driver is not the
gate; it is that **there is no count/byte ceiling independent of the token
window**, and no ceiling at all when compaction is off.

**Suggested fix.**

- Add a hard ceiling for `m_messages` expressed in bytes as well as count (e.g.
  2× the transcript cap), enforced in `appendMessage()` by dropping the oldest
  complete turn groups while preserving system/summary and tool-call/result
  pairing. This is the same boundary logic `ContextEngine` already uses; it is
  not a schema rewrite.
- Always adopt `persistent` when it is materially smaller (e.g. ≥10 %), and make
  the "compaction disabled" case still trim the *stored* list while only
  disabling the *proactive* summary path.
- Emit the resulting message/byte counts through `memoryCounters()` (currently
  only counts messages; see F10).

### F2 — Reasoning and answer stream buffers are unbounded — **High**

**Where:** `src/agentloop.cpp:387-389` (`m_currentThinking += delta`),
`461`, `4480` and `5044` (`assistant.thinking = m_currentThinking`);
`src/acpclient.cpp:3546-3558` (`m_currentText` / `m_currentThinking`);
`src/types.h:185`; serialisation at `src/sessionstore.cpp:123-124`.

**Mechanism.** `LlmClient` caps the visible answer at 4 MiB
(`llmclient.cpp:215-217`, `1989-1993`) and the SSE event at 8 MiB, and
`AcpClient` caps individual tool streams, but:

- `AgentLoop` accumulates every `thinkingDelta` into `m_currentThinking` with no
  cap, and stores it in `assistant.thinking` on completion. Thinking is then
  persisted in full (`sessionstore.cpp:123-124`) and counted in compaction
  tokens (`contextengine.cpp:205-214`).
- `AcpClient` accumulates `m_currentText` and `m_currentThinking` from
  `agent_message_chunk` / `agent_thought_chunk` with no cap
  (`acpclient.cpp:3546-3558`). A single frame is capped at 16 MiB
  (`acpclient.cpp:47`), but there is no cap on the sum.
- `SubtaskState::streamed` (`agentloop.cpp:7035-7052`, `7075-7078`) accumulates
  the child's entire streamed answer, as does `SubtaskWidget::m_streamedMessage`
  and `m_pendingStreamed` (`subtaskwidget.cpp:534-543`, `578-581`).

**Consequence.** A reasoning-heavy model (or a broken ACP agent) can retain
megabytes of hidden reasoning per turn, in memory and on disk. Every autosave
then re-serialises it.

**Suggested fix.** Add the same character ceiling used for the visible answer to
thinking (native and ACP). Thinking beyond the ceiling should be truncated with
a marker, not kept. Do not store reasoning older than N turns in `m_messages`;
the transcript copy can keep the display text. For a sub-agent, cap
`state->streamed` and `SubtaskWidget::m_streamedMessage` at a few hundred KiB
(the card already caps its activity log).

### F3 — Sub-agent records retain full final reports — **High**

**Where:** `src/agentloop.h:58-83` (`SubagentRecord`), `674`
(`m_subagentRecords`); assignment at `src/agentloop.cpp:7297`
(`record.summary = answer`), also `6738`; `sessionData()` copies the map at
`7690-7725`; `subagents()` returns a full copy at `1372-1379`.

**Mechanism.** Every completed sub-agent leaves a `SubagentRecord` in the root
loop for the rest of the conversation. `summary` is the child's *full* final
answer (not clipped; only the sidechain JSON is clipped to 8000 chars at
`agentloop.cpp:7321-7328`). `filesChanged`, `checks`, `worktreePath`, `patchPath`
and `sidechainPath` add more. There is no cap on the number of records; only
`resetConversation()` clears them (`agentloop.cpp:2887`).

**Consequence.** A conversation that delegates 50 times can retain several MiB of
duplicated child answers, and `sessionData()` copies the record list on every
autosave (cheap per-string because of implicit sharing, but O(records) per save).

**Suggested fix.** Keep the record summary bounded (the sidechain already has the
full transcript), e.g. 8 KiB, and drop the oldest completed records beyond a
count cap (the UI already caps visible sub-agent cards at 40/restored 40,
`chatwidget.cpp:1218`, `7718`). `patch` is already spilled and cleared; the same
policy belongs to `summary`.

### F4 — Per-conversation bookkeeping containers without count/byte caps — **Medium**

These are small individually but grow for the whole conversation:

| Container | Location | Growth | Note |
| --- | --- | --- | --- |
| `m_changedPaths` | `agentloop.h:780`, inserted at `agentloop.cpp:4926` | one entry per mutated path, until reset | A monorepo-wide edit run can reach thousands; cleared only in `resetConversation()` (`2867`). |
| `ToolRunner::m_readSnapshots` | `tools.h:248`, inserted `tools.cpp:1377-1382` | one 32-byte digest per file read | Cleared at turn start and after a mutation (`agentloop.cpp:3275`, `4940`), so it is bounded per turn; a single turn that reads 10k files still retains ~0.5 MB of digests. |
| `EditTracker::m_settledRanges` | `edittracker.h:194`, appended `edittracker.cpp:995` | one range list per accepted edit, per path | Removed only on undo/clear/rename (`edittracker.cpp:609-614`, `1067-1075`). A long session with many accepted edits accumulates ranges indefinitely. |
| `CostLedger::m_subagents` | `harness/cost.h:112`, inserted `cost.cpp:119` | one entry per `taskId` | Small entries, but no removal until `reset()`. |
| `m_actionRepeatCounts` | `agentloop.h:776` | bounded at `kMaxRepeatEntries` (`agentloop.cpp:2493-2496`) | Already bounded; listed for completeness. |
| `m_routeUsage` | `agentloop.h:843` | one entry per orchestration route | Cleared on reset; route count is small. |
| `m_invokedSkills` | `agentloop.cpp:6010-6016` | capped at 8 bodies | Bodies can be large; cap bytes too. |

**Suggested fix.** Give `m_changedPaths` and `m_settledRanges` a count cap with
oldest-first eviction (the UI does not need ranges for edits older than the undo
window of 50, `edittracker.cpp:1008-1010`). Cap the per-turn read ledger by count
as well (keep the newest N digests). Cap `CostLedger::m_subagents` by count.

### F5 — ACP protocol ceilings are far above normal use — **Medium**

**Where:** `src/acpclient.cpp`:

- live tools `512` / `32 MiB` (`3595-3608`, `3661-3671`);
- tool streams `8 MiB` (`3782-3795`);
- parked methods `64` / `32 MiB`, single incoming method `8 MiB`
  (`4423-4435`);
- frame `16 MiB` (`47`, `3145`, `3189`);
- ACP file read `64 MiB` (`4595-4600`);
- terminals `8` retained, 1 MiB each (`6119-6145`);
- stderr retained at 64 KiB (`2100-2104`).

**Mechanism.** These are safety ceilings, not normal limits. A hostile or buggy
agent can park 64 mutating methods, each carrying a full `apply_patch` body, and
retain 32 MiB plus the permission cards. The frame reader also buffers 16 MiB
before discarding. ACP `read_text_file` can read a 64 MiB file into one QString.

**Suggested fix.** Tighten to values that match the native path: 8-16 MiB total
for live tools, 1-2 MiB per stream, 8 MiB total parked payloads, 4-8 MiB frames,
and stream/paginate `read_text_file` above 8 MiB instead of reading whole.

### F6 — Headless ACP server keeps every session until explicitly closed — **Medium**

**Where:** `src/harness/acpserver.h:111` (`m_sessions`), `src/harness/acpserver.cpp:286-292`
(insert on `session/new`), `316-321` (remove on `session/close`);
`src/headless/acpsession.h:32` (`HeadlessSession m_session`),
`src/headless/session.h:84` (`std::unique_ptr<AgentLoop> m_loop`).

**Mechanism.** Each `session/new` creates a complete `HeadlessSession` with its
own `AgentLoop`, graph, LLM client, event store and SQLite connection. There is
no idle timeout, no session-count cap, and no cleanup when the client disappears
without sending `session/close` (the reader thread only quits the process on
`stdioClosed`). A front-end that opens a session per editor tab or retries a
failed session will accumulate them.

**Suggested fix.** Add a cap (e.g. 8 concurrent sessions) and an idle timeout
that closes sessions with no prompt for N minutes. Make `stdioClosed` close all
sessions before quitting.

### F7 — Inspector panel stays alive and retains the last full reports — **Low/Medium**

**Where:** `src/inspectorpanel.cpp:35` (`WA_DeleteOnClose, false`), `42-47`
(six `QTextBrowser` tabs), `107` (refresh in constructor), `138-151`
(`refresh()` builds all reports); created from `src/chatwidget.cpp:3856-3862`
with the `ChatWidget` as parent.

**Mechanism.** The first time the inspector is opened, it builds and stores the
context report, prompt report, tools report, memory report, skills/hooks/sub-agent
report, the trace report (up to 2000 events, `agentloop.cpp:1493-1515`) and the
usage report in six text documents. Refreshes while visible are already throttled
to 250 ms (`inspectorpanel.cpp:112-127`), but closing the dialog only hides it
(`inspectorpanel.cpp:95`), so the documents stay in memory until the `ChatWidget`
is destroyed. `refresh()` is also called on show, re-running
`contextReport()`/`promptReport()`, which call `m_messagesForRequest(false)`
(`agentloop.cpp:3610-3648`, `3927-3975`), i.e. a full compaction pass just to
render a report.

**Suggested fix.** Clear the six documents on `hideEvent`, or use
`WA_DeleteOnClose` and recreate the panel. Build the trace/report text lazily per
tab and only for the visible tab.

### F8 — `m_ownsMcp` is never true; the per-loop MCP manager is dead and workspace MCP config never loads — **High (correctness) / Low (RAM)**

**Where:** `src/agentloop.h:665` (`bool m_ownsMcp = false;`), constructor
`src/agentloop.cpp:367` (`m_mcp = new McpManager(this)`), guards at
`agentloop.cpp:857` and `1682-1686`; `McpManager::setWorkspace`
`src/mcp.cpp:1142-1159`; defaults `src/mcp.h:302-306`.

**Mechanism.** The root loop creates its own `McpManager`, but `m_ownsMcp` is
never set to `true` anywhere in the tree (verified with `grep -rn m_ownsMcp src`).
The only call to `McpManager::setWorkspace()` is inside the never-taken
`if (m_ownsMcp)` block in `AgentLoop::setWorkspace()`. Consequences:

- `AgentLoop::setWorkspace()` never calls `m_mcp->setWorkspace(workspace)`, so
  `McpManager::m_workspace` stays empty and `reloadFromDisk()` never loads
  `<workspace>/.kateai/mcp.json`. The MCP menu reads an empty server list.
- `setMcpManager()` never deletes the loop's own manager; every sub-agent keeps a
  second, unused `McpManager` parented to it (small, not a hard leak).
- Fixing this by setting `m_ownsMcp = true` in the constructor would activate the
  intended per-window MCP process duplication (each window starts its own copies
  of every configured MCP server). That duplication should be addressed at the
  same time (see P1).

**Suggested fix.** Set `m_ownsMcp = true` in the constructor (or drop the flag
and always apply the workspace), then make the manager workspace-scoped and
shared across windows for the same folder, or keep one manager per workspace in
the plugin and inject it into every `AgentLoop`.

### F9 — Completion response body is unbounded — **Low**

**Where:** `src/completionengine.cpp:301` (15 s transfer timeout), `318`
(`reply->readAll()`), no size guard before `QJsonDocument::fromJson`.

**Mechanism.** A misconfigured or hostile completion endpoint can return an
arbitrarily large body; it is copied into `QByteArray` and then parsed into a JSON
tree before the completion text is extracted. `LlmClient` caps its retained
response at 256 KiB (`llmclient.cpp:215`, `1040-1041`); the completion path has no
equivalent.

**Suggested fix.** Read at most `Content-Length`/a fixed 256 KiB and abort with
`failed()` beyond it. The existing `setTransferTimeout(15000)` bounds time, not
bytes.

### F10 — Memory counters do not cover the largest retained structures — **Low**

**Where:** `src/agentloop.cpp:3708-3722` (`memoryCounters`),
`src/agentloop.h:316`.

**Mechanism.** The counters expose message count, transcript count/chars,
action-repeat entries, sub-agent count/patch chars and changed-path count, but not
`m_messages` chars, thinking chars, session payload size, event count, or
QTextDocument character counts. The regression test therefore cannot catch
F1/F2/F3 growing.

**Suggested fix.** Add `messageChars` (content + thinking + tool args),
`thinkingChars`, `subagentSummaryChars`, `eventCount` and `sessionPayloadBytes`
(last-save size) to `memoryCounters()`, and assert them in
`tests/test_memory_budget.cpp`.

---

## 2. Transient allocation spikes

These do not retain memory after the operation, but they are the peaks that make a
long session feel heavy and can OOM a 32-bit or low-RAM machine.

### T1 — Session autosave serialises the whole conversation every 5 s while streaming — **Critical**

**Where:** `src/chatwidget.cpp:454-460` (5 s single-shot timer), `2126-2155`
(`saveCurrentConversation`), `2157-2170` (`scheduleSessionSave` called from every
delta at `1104`, `1142`, `1152`, `1192`, `1373`, `1858`, `1866`);
`src/agentloop.cpp:7690-7725` (`sessionData` copies `m_messages` and, when
diverged, the transcript); `src/sessionstore.cpp:300-328` (`sessionDataToJson`
builds a full `QJsonObject` + `QJsonDocument` + compact `QByteArray`);
`1171-1270` (`saveConversation` binds the blob).

**Mechanism.** While a reply streams, `scheduleSessionSave()` marks dirty and
starts the timer only if it is idle. Every 5 s the timer fires and the code:

1. copies the full `SessionData` (`m_messages`, optional
   `m_transcriptMessages`, sub-agent records, orchestration) — the `QList`
   copies are shallow because `ChatMessage`'s strings are implicitly shared, but
   they are still O(messages) pointer copies;
2. walks every message into a `QJsonObject`, then a `QJsonDocument`, then a
   compact `QByteArray` (2-3 deep copies of the payload);
3. binds the byte array into SQLite inside a transaction.

For a session at the 8 MiB transcript cap plus a larger model history, that is a
20-40 MiB transient every 5 s, on the GUI thread, for the entire duration of a
long answer. The save is also largely redundant with the completed-turn save at
`chatwidget.cpp:1788-1789`, the user-message save at `1101`, and the
switch/clear saves at `8199`/`8366`.

**Suggested fix.** While streaming, persist only the incremental tail
(`currentAssistant` + the last message) into a small side row/file, and do the
full serialisation once at turn end. Alternatively debounce to
`max(5 s, 10 s after the last delta)` and skip the save if only
`currentAssistant` changed and a delta journal is available. This is not a
session-schema rewrite; it is an incremental save on top of the existing payload.

### T2 — Session restore/switch builds the JSON tree, the parsed history and all cards — **High**

**Where:** `src/sessionstore.cpp:1141-1169` (`loadConversation` returns
`sessionDataFromJson(query.value(0).toByteArray())`), `331-380`
(`sessionDataFromJson`); `src/chatwidget.cpp:8206-8235`
(`switchToConversation`), `7529-7980` (`rebuildTranscript`).

**Mechanism.** Restore peaks at roughly: SQLite payload copy + `QJsonDocument`
tree + parsed `SessionData` (both `messages` and `transcript` when present) +
the `messages` snapshot taken by `rebuildTranscript()` (`chatwidget.cpp:7568`) +
the widgets/documents. Incremental pruning during rebuild
(`chatwidget.cpp:3572-3585`) already prevents the "build all cards, then prune"
peak, and the transcript is bounded on load (`agentloop.cpp:7806-7812`), but a
diverged session stores both lists, so the payload and the parsed result each
contain the history twice.

**Suggested fix.** When `transcript` is present and equal to `messages`, the
serialiser already omits it (`agentloop.cpp:7697-7700`); extend that to a cheap
"transcript == messages + summary" case, or store the transcript as a separate
paged blob. At minimum, clear the parsed lists from the local `SessionData`
after `restoreSession()` has taken them (the `QList` copy is shallow, so
clearing the local handle releases only the redundant reference) so the parse
copy does not stay live through the widget rebuild.

### T3 — Per-turn OTLP export reads the entire event log and builds all spans — **High**

**Where:** `src/agentloop.cpp:3853-3869` (`traceSpans()` calls
`store.read(conversation, 0, -1)`), `1900-1914` (called from `endHarnessTurn` on
every turn when telemetry + endpoint are configured); `src/harness/otlp.cpp:153-169`
(`exportSpans` copies the span list into `m_pending`), `173-230` (`send` builds the
full JSON body), no `setTransferTimeout`.

**Mechanism.** The RAM plan says `traceSpans` is "only exercised by explicit
telemetry export" (`docs/RAM_SAVINGS_PLAN.md:60-63`). That is not true on this
HEAD: `endHarnessTurn()` automatically constructs an `OtlpExporter` and calls
`exporter->exportSpans(traceSpans())` after every completed/failed/interrupted
turn. `traceSpans()` reads **all** events for the conversation (limit `-1`),
converts each to a `Span` with a `QJsonObject` attributes map, and the exporter
then copies the list and serialises it. Memory grows with the total event count,
not the turn, and is rebuilt from scratch every turn.

There is no transfer timeout on the OTLP request. If the collector accepts the
connection and stalls, `m_pending` spans, the request body and the reply buffer
stay alive until the loop is destroyed.

**Suggested fix.** Track the last exported sequence and read/convert only new
events, appending to a bounded span cache (keep the open parent spans, discard
closed spans after export). Cap the export batch and the event payload. Set a
transfer timeout and clear `m_pending` on failure after the retry budget.

### T4 — Full-file reads in edit/write/patch tools — **High**

**Where:**

- `DiskDocumentBridge::readDocument` — `src/documentbridge.cpp:225-251`
  (`QTextStream::readAll`), used by every edit tool.
- `ToolRunner::writeFile` — `src/tools.cpp:1477-1545`; the optimistic-concurrency
  branch reads the whole current file at `1517-1523`.
- `ToolRunner::editFile` — reads the whole file at `1577`.
- `ToolRunner::applyPatch` — `src/tools.cpp:3475-3565`: phase 1 reads every file
  and stores both `oldContent` and `newContent` in `PlannedWrite` for the whole
  patch, to support all-or-nothing rollback (`3564`, rollback `3610-3650`).
- ACP equivalents: `src/acpclient.cpp:4770-4790` (`fs/edit_file`),
  `4845-4875` (multi-edit), `5240-5300` (`apply_patch`, `PlannedFile` keeps
  `original` + `updated`), `4595-4600` (64 MiB read cap).

**Mechanism.** A patch touching ten 20 MiB files holds ~400 MiB of UTF-8 text —
and `QString` stores UTF-16, so the resident cost is roughly double — because
`oldContent` + `newContent` are kept for every file before any write. A single
`edit_file` on a 200 MiB file materialises the whole text (`contents`) and then a
second copy when the replacement is applied (`updated`), plus the diff/JSON
strings. `readFile` has a stat guard for the *ledger* (`tools.cpp:1363-1382`,
4 MiB) and streams line reads (`documentbridge.cpp:258-300`), but the ledger path
still reads the full file up to 4 MiB first, then streams it a second time.

**Suggested fix.** Refuse by stat size above a configurable edit limit (e.g.
8 MiB) with a clear message, or stream the unchanged head/tail and only
materialise the changed region. For `apply_patch`, write per-file and keep a
rollback journal on disk (the existing `EditTracker` blob store is the model)
instead of keeping old + new content in RAM for every file. This is already the
pattern used for large edit bodies (`edittracker.cpp:261-288`).

### T5 — Typing updates read the whole open document every 1.5 s and re-index it — **High**

**Where:** `src/pluginview.cpp:225-236` (graph timer reads
`m_bridge.readDocument(path, &content)`), `src/agentloop.cpp:2499-2512`,
`src/graph/projectgraph.cpp:266-296` (`updateGraph` assigns the whole string,
then metrics/symbols/imports, then trims to 4 KiB).

**Mechanism.** When the user stops typing for 1.5 s, the view reads the entire
open document (via `doc->text()`, a full copy) and passes it to
`updateGraph`, which runs `updateNodeMetrics`, `indexFileSymbols` and
`extractImportSpecs` on the whole string before trimming to
`kContentPreviewChars` (4 KiB). Unlike the scan path, which caps at 1 MiB
(`projectgraph.cpp:241`), the incremental path has **no size cap**. Editing a
50 MiB log or minified bundle produces a 50 MiB copy + regex work every pause.

**Suggested fix.** Apply the same 1 MiB analysis cap in `updateGraph`, or skip
graph re-indexing for documents beyond it and store a metadata-only node. For
large documents, compute line count and imports incrementally from the editor's
changed range.

### T6 — Configuration dialog constructs all 12 tabs eagerly — **Medium** *(fixed)*

Implemented: `KateAiConfigPage` lists all twelve labels up front and builds a
tab's widgets on first activation (`buildTab`/`showTab`). `apply()` guards each
section on `isTabBuilt`, so a tab the user never opened keeps its stored values
instead of writing widget defaults, and the workspace MCP file is only rewritten
when its tab was loaded. `tests/test_configpage.cpp` asserts that opening the
page builds one tab and that an unvisited tab survives Apply.

**Where:** `src/configpage.cpp:101-1421` (twelve `addTab` calls at `333`, `363`,
`695`, `927`, `981`, `1037`, `1100`, `1160`, `1211`, `1274`, `1345`, `1421`),
plus a dedicated `QNetworkAccessManager` and registry fetch at `366-372`;
`setWorkspace` loads `McpConfigStore` at `2427`.

**Mechanism.** Opening Settings builds every widget, every scroll area, every
model combo and the ACP registry list before the user sees the first tab. Most
of it is small, but the twelve tabs plus model catalogs and the registry JSON are
a visible pause and a few MiB. The dialog is `WA_DeleteOnClose`
(`pluginview.cpp:948-949`), so nothing persists after close — this is a transient
spike, not a leak.

**Suggested fix.** Build tabs lazily on first activation (`QTabWidget` +
placeholder), and fetch the registry only when the ACP tab is opened. This is a
UI-only change.

### T7 — Compaction/request building copies the history several times — **Medium**

**Where:** `src/harness/contextengine.cpp:390-420` (`result.messages = history`,
then `working` copy), `512-560` (dropped slice + summary), `228-340`
(`summarizeDropped`); `src/contextmanager.cpp:39-47` (`estimateTokens`
re-serialises tool-call objects); `src/agentloop.cpp:3487-3499`,
`3522-3536`.

**Mechanism.** Every model request runs `ContextEngine::build(m_messages, …)`,
which copies the list into `result.messages`, copies the non-summary messages
into `working`, may take `working.mid(...)` for the dropped block, and serialises
every tool-call object to estimate tokens. `QList<ChatMessage>` copies are shallow
(QString is implicitly shared), so this is O(n) pointer copies rather than O(bytes),
but for 10k messages it is still thousands of allocations per request, and the
`QJsonDocument` serialisation in `tokensOf` allocates per tool call.

**Suggested fix.** Build `working` once and move it into `result.messages`
instead of copying twice when no summarisation is needed; cache per-message token
costs on the `ChatMessage` (or in a side vector) between requests, invalidated on
append. `ContextEngine` already computes `workingCosts` once per build
(`contextengine.cpp:455-466`); persist that vector for the next request.

### T8 — `clearAllConversations()` reads every payload in one transaction — **Medium**

**Where:** `src/sessionstore.cpp:1485-1500` (`SELECT id, payload FROM
conversations`, then `cleanupInfoFromPayload` per row).

**Mechanism.** Clearing history parses every stored conversation payload to
recover workspace/sub-agent worktrees. The rows are processed one at a time, so
the peak is one payload plus the accumulated small `ConversationCleanup` structs,
but a history with hundreds of multi-MiB conversations is a long GUI-thread
operation with a large live SQLite read cursor. `deleteConversation()` does the
same per row (`1302-1310`).

**Suggested fix.** Store `workspace` and the sub-agent worktree list in indexed
columns (or a small side table) at save time, so cleanup does not need the
payload. This is additive to the existing schema, not a rewrite.

### T9 — Voice capture keeps several full copies of the WAV — **Medium**

**Where:** `src/voiceinput.cpp:42` (5-minute cap), `1630-1650`
(`rawBytes` -> `denoiseWavBuffer` -> `cleanBytes` -> file), `1680-1690`
(`wavBytes = file.readAll()`), `1739` (HTTP `readAll`), `1113-1199`
(`parsePcm16Wav` -> `QVector<qint16>`).

**Mechanism.** A 5-minute 16 kHz mono 16-bit recording is ~9.6 MB. At the
transcription step the code can hold the raw WAV, the decoded sample vector, the
denoised WAV, the base64/HTTP body and the reply at the same time — roughly
40-60 MB. It is bounded and short-lived, but on a low-RAM machine it is the
largest single allocation in the plugin.

**Suggested fix.** Stream the raw capture to disk (already the case) and decode
in place; free `rawBytes` before reading `wavBytes`; send the file with
`QHttpMultiPart` instead of base64 in memory. Keep the 5-minute cap.

### T10 — ACP stdout bursts and frame buffering — **Medium**

**Where:** `src/acpclient.cpp:3119-3151` (`readAllStandardOutput()` then
64 KiB chunks; `m_stdoutBuffer` up to 16 MiB), `3145-3150` (discard over
`MaxFrameBytes`).

**Mechanism.** A chatty agent that writes faster than the event loop drains can
make `readAllStandardOutput()` return a large `QByteArray` in one go; the code
then appends it to `m_stdoutBuffer`, which may hold up to 16 MiB before being
discarded. This is transient, but a broken agent can repeat it. Combined with
`m_currentText` (F2), a single turn can pin tens of MiB.

**Suggested fix.** Read in bounded chunks (`read(64 KiB)`) in a loop instead of
`readAll`, and lower `MaxFrameBytes` to 4-8 MiB.

### T11 — Streaming text exists in three places at once — **Medium**

**Where:** `src/llmclient.cpp:1989-1993` (`m_text` capped at 4 MiB),
`src/agentloop.cpp:385-389` (`m_currentAssistant += delta`),
`src/chatwidget.cpp:2284-2287` + `1160-1164` (`m_streamText`), plus the
`QTextDocument` built by `setStreaming()`.

**Mechanism.** While a native answer streams, the same text is retained by the
client, the loop and the widget, and rendered into a fourth representation. The
renderer backs off as the answer grows (`chatwidget.cpp:2363-2384`), so the
transient is not quadratic, but the 4 MiB client cap still implies ~12 MiB of
strings plus the document. ACP has no cap, so this compounds F2.

**Suggested fix.** Do not keep `m_currentAssistant` in the loop once deltas have
been forwarded; rebuild it at finish from the client's final text or from the
widget's text. Or have the loop keep only a rolling tail and a byte count for the
UI.

### T12 — Sub-task card re-renders the whole answer on every delta — **Medium**

**Where:** `src/subtaskwidget.cpp:534-543` (`m_streamedMessage += delta;
m_answer->setDocument(makeMarkdownDocument(...))`), `578-581`
(`m_pendingStreamed += delta`).

**Mechanism.** Each child delta rebuilds the entire markdown document for the
card, and the accumulated string is unbounded. A long child answer makes the
render cost quadratic and holds the text twice (accumulator + document). The
parent's `SubtaskState::streamed` holds a third copy (`agentloop.cpp:7075-7078`).

**Suggested fix.** Reuse the parent's streaming back-off/coalescing, cap the
retained text, and render only when the card is visible.

### T13 — Headless `gitDiff` materialises the whole workspace diff — **Medium**

**Where:** `src/headless/main.cpp:65-96` (`runGit`/`gitDiff`): `readAllStandardOutput`
for `git diff --binary`, then one `git diff --no-index` per untracked file,
concatenated into one `QString patch`.

**Mechanism.** A headless run on a workspace with a large modified binary or many
untracked files builds the entire diff as one UTF-16 string (plus the per-command
UTF-8 buffers). This is not retained after the JSON result, but it is a
multi-hundred-MB peak on a large workspace.

**Suggested fix.** Cap the combined patch (the sub-agent worktree path already
caps at 32 MB) and skip untracked files above a per-file limit.

---

## 3. General RAM-savings opportunities

### P1 — Share the harness per workspace instead of per window — **High impact**

**Where:** `src/pluginview.cpp:146-157` (new `ChatWidget` + `EditorAssist` per
Kate window), `src/chatwidget.h:366` (`AgentLoop m_agent`), constructor
`src/agentloop.cpp:360-370` (`ProjectGraph`, `McpManager`, `MemoryLoader`),
`src/agentloop.h:647-669` (`LlmClient m_client`, `AcpClient m_acp`,
`WebSearch m_web`, `ModeRegistry m_modes`, `CheckpointManager m_checkpoints`),
`803` (`ToolRegistry`), `852` (`EventStore`), `853` (`PolicyEngine`).

**Mechanism.** Every window owns:

- a `ProjectGraph` (up to 1000 nodes, 4 KiB preview each + symbols/dependency
  sets, ~5-15 MB depending on the project);
- an `McpManager` (currently inert because of F8, but intended to start every
  configured MCP server process);
- an `AcpClient` (a separate agent process when ACP is active);
- an `LlmClient` + `QNetworkAccessManager`, a `WebSearch` + QNAM, a
  `CompletionEngine` QNAM in `EditorAssist` (`completionengine.cpp:44`);
- `ToolRegistry`, `ModeRegistry`, `AgentTeam`, `CheckpointManager`, `EventStore`
  (one SQLite connection with a 1 MiB page cache, `eventstore.cpp:135-141`),
  `MemoryLoader`, `PolicyEngine`, hooks.

Two windows on the same folder duplicate all of it; N windows multiply it. Only
the root/child pair shares the graph and the MCP manager
(`agentloop.cpp:6623`, `7017`, `6574`, `6861`).

**Suggested fix.** Introduce a per-workspace "harness services" object owned by
the plugin (graph, MCP manager, event store, policy/hooks, memory loader) and
inject it into each `AgentLoop`. Keep per-loop what is genuinely per-conversation
(message history, tool ledger, cost, queues). This is a refactor, but it is the
single largest steady-state reduction for multi-window users, and it also fixes
F8 cleanly.

### P2 — Card budgets are measured in characters, but `QTextDocument` costs far more per character — **High impact** *(partly fixed)*

**Where:** `src/chatwidget.cpp:3099-3160` (`kMaxRetainedToolCards = 400` /
6 MiB chars, `kMaxRetainedTextCards = 300` / 4 MiB chars,
`kMaxRetainedThinkingBlocks = 200` / 2 MiB), `3138-3144`
(`bodyChars` counts `document()->characterCount()`);
`src/toolcallwidget.cpp:931-951` (`m_resultOutput` keeps the full result),
`1044-1051` (`retainedChars` counts the string **and** the document).

**Mechanism.** A character-count budget is not a byte budget. A `QTextDocument`
with inline formats, block layouts and a widget consumes roughly 20-100 bytes per
character depending on formatting. 6 MiB of tool-card characters can therefore
represent tens to hundreds of MiB of Qt layout/document memory in the worst case.
The same text is also retained both as `m_resultOutput` and as the rendered
document, roughly doubling the source-text contribution. The caps are much better
than unbounded, but the number users see (6 MiB) is not the number the process
uses.

**Suggested fix.** Calibrate the caps with RSS measurements
(`docs/MEMORY_BASELINE.md` has the harness) and lower them based on the measured
bytes-per-char factor, or count both source chars and document block counts.
Prefer a single `QTextDocument`/`QTextBrowser` with collapsed sections over one
rich document per card, and lazily render only the visible card's details (the
"lazy-render large results" item already does this for the largest bodies; extend
it to all cards). This is the incremental alternative to a wholesale
virtualiser, consistent with the existing safety rule.

### P3 — `TranscriptVirtualizer` is dead code — **Medium impact if wired, Low as-is**

**Where:** `src/transcriptvirtualizer.cpp`, `src/transcriptvirtualizer.h`,
compiled by `src/CMakeLists.txt`; `grep -rn TranscriptVirtualizer src` matches
only its own files.

**Mechanism.** An implementation for incremental transcript materialisation
exists (`setEntries`, `ensureAliasVisible`, `forgetWidget`, event filter), but
`ChatWidget` never instantiates it. The RAM plan explicitly forbids a "wholesale
transcript virtualizer", so this is not a recommendation to wire it wholesale.
It is, however, evidence that the intended bounded rendering path is inactive.

**Suggested fix.** Either delete the dead files (saves build time and avoids
confusion), or use it only for the *restored* transcript above a high threshold
(e.g. >200 cards), keeping the live append path unchanged. That is an incremental
cap, not a rewrite.

### P4 — Do not keep reasoning in long-term model history — **High impact for reasoning models**

**Where:** `src/types.h:178-195` (`ChatMessage::thinking`), storage at
`src/agentloop.cpp:4480`, `5044`, persistence at `src/sessionstore.cpp:123-124`.

**Mechanism.** Reasoning is useful for the current turn and the immediately
following one, but keeping every turn's thinking in `m_messages` doubles or
triples the history size for reasoning models, and it is serialised on every
autosave. Compaction counts it (`contextengine.cpp:205-214`) but only when the
window fills.

**Suggested fix.** Strip `thinking` from messages older than the last N turns
(e.g. 2) when appending or before serialisation, keeping the display transcript
copy if the user wants to scroll back. This is a bounded, reversible policy and
does not change the schema.

### P5 — Session payload is monolithic; save incrementally — **High impact on T1/T2**

**Where:** `src/sessionstore.cpp:300-328`, `331-380`, `1171-1270`.

**Mechanism.** The whole conversation is one JSON blob in one SQLite column.
Every save rewrites it; every load parses it. The payload grows with the
transcript and the model history, and SQLite binds/copies it again.

**Suggested fix.** Keep the existing schema, but add an append-only
`message_blobs`/journal table keyed by conversation and sequence, and store the
small metadata (plan, thinking snapshot, changed paths) in the existing payload.
On load, page the journal. The `EventStore` already implements paged
append/read (`eventstore.cpp:493-530`) and `projectMessages`, so the pattern
exists in-tree. This is not a schema rewrite of the visible session; it is an
additional storage path that preserves the current payload as a fallback.

### P6 — Event log has no per-conversation retention — **Medium impact**

**Where:** `src/harness/eventstore.cpp` (append-only; only `pruneOrphans()` at
`144-160`), `src/harness/replay.cpp:119-123` (`read(..., -1)`),
`src/headless/main.cpp:157-158` (`read(..., -1)`), `src/agentloop.cpp:3853-3869`.

**Mechanism.** Events are never pruned by count or age, so both the SQLite file
and any full read grow with conversation length. The trace report is bounded to
2000 events (`agentloop.cpp:1507-1515`), but replay/export/OTLP are not.

**Suggested fix.** Add a per-conversation event cap (e.g. 20k events or 32 MiB)
with head trimming, preserving the first `TurnStart` and the last N turns. Make
`Replay::analyze` page, and make the headless export stream pages instead of
`read(..., -1)`.

### P7 — Project graph memory layout — **Medium impact** *(partly fixed)*

**Where:** `src/graph/projectgraph.h:27-45` (`GraphNode` with `QString`,
`QSet<QString>` dependencies/dependents, `QJsonObject metadata`, `content`),
`151-164` (`QMap<QString, GraphNode*> m_nodes`, `QMap<QString, GraphEdge*>
m_edges`, `QMap<QString, QSet<QString>> m_adjacencyList`,
`QHash<QString, QStringList> m_pendingImports`).

**Mechanism.** Each node stores the same strings in `id`, `name`, `path` and in
the `QMap` key; dependencies are stored three times (node sets, adjacency map,
edges). `metadata` may hold a `symbols` array for each file. At 1000 nodes this
is a few MiB to tens of MiB, and every window has a copy (P1).

**Suggested fix.** Intern node ids/paths, drop the redundant `QMap` key or the
node's `id`, and store dependency edges only in one adjacency structure. Keep
`content` at 4 KiB (already done). If symbols are only needed on demand, index
them lazily per file instead of embedding them for all 1000 nodes.

### P8 — One SQLite connection and page cache per loop — **Medium impact with many sub-agents**

**Where:** `src/agentloop.h:852` (`EventStore m_events`), `src/harness/eventstore.cpp:115-141`
(connection per store, `cache_size=-1024`, WAL), `src/sessionstore.cpp:628-745`
(cached connection per thread, import/repair on open).

**Mechanism.** Each `AgentLoop` (root and every running sub-agent) has an
`EventStore`; the first event opens a SQLite connection with a 1 MiB page cache
plus WAL/SHM mappings. Parallel sub-agents multiply this. The `SessionStore`
connection is per thread and stays open, which is fine.

**Suggested fix.** Share one `EventStore`/connection per conversation (or per
thread) and serialize appends through it. This pairs naturally with P1.

### P9 — `ConfigPage` and ACP registry network managers are per dialog — **Low**

**Where:** `src/configpage.cpp:366-372` (own QNAM + registry fetch),
`src/plugin.cpp:97` (startup update check), `src/acpinstall.cpp:497`.

**Mechanism.** Each Settings open allocates a QNAM and downloads the registry
JSON. The dialog is deleted on close, so this is transient. A shared QNAM in the
plugin would remove the per-dialog connection pool and allow caching the registry
response for the session.

### P10 — Hardening gaps: unbounded config reads and remote JSON bodies — **Low**

**Where:** `src/harness/policy.cpp:26-36`, `src/harness/hooks.cpp:32-42`,
`src/ecosystem.cpp:86`, `164` (package.json/CMake replies),
`src/plugin.cpp:97`, `src/configpage.cpp:372` (registry downloads),
`src/completionengine.cpp:318`.

**Mechanism.** `McpConfigStore::load` has a 1 MiB stat/symlink guard
(`mcp.cpp:1495-1515`), but the other config readers use `readAll()` on a
workspace-controlled path with no size or file-type check. A symlink to a large
file or `/dev/zero` (where `size()` is 0) can block or exhaust memory. The
registry and completion responses have the same issue over HTTP: `readAll()`
before any size check. This is the same class of bug the MCP loader already
defends against.

**Suggested fix.** Reuse one `readBoundedRegularFile(path, maxBytes)` helper
(stat, `isFile`, `!isSymLink`, size cap, then `read(max+1)`) for policy, hooks,
ecosystem, registry and completion responses.

### P11 — Minor code-level savings

- `AgentLoop::sessionData()` calls `subagents()` which returns a full sorted copy
  (`agentloop.cpp:1372-1379`); iterate the hash directly when serialising.
- `AgentLoop::subagentsReport()` / `toolsReport()` rebuild large strings; they are
  only used by the inspector, so build them per tab on demand (F7).
- `memoryCounters()` is cheap but not shown in the Inspector; expose it there so
  users can see what is retained.
- `ToolCallWidget` keeps `m_resultOutput` after the card is finished for the copy
  button (`toolcallwidget.cpp:940`, `975`); copy from the content store by id
  instead for results above a threshold.
- `EditTracker` blob files (`/tmp/kateai-edits-*`) are removed only on
  `clear()`/destructor (`edittracker.cpp:256-258`, `302-306`, `447-455`).
  Accepted edits leave their blobs on disk until then; not RAM, but worth
  cleaning per settled group.

---

## 4. Prioritised plan

The plan is ordered by (impact ÷ risk), and stays inside the existing safety
rules: no wholesale transcript virtualiser, no graph-storage rewrite, no session
schema rewrite.

### Wave 0 — Bounds that stop unbounded growth (low risk, high impact)

1. Hard-cap `m_messages` by bytes and count, preserving turn groups and pairing
   (F1). Add the counts to `memoryCounters()` and a memory-budget test.
2. Cap thinking in `AgentLoop` and `AcpClient`, and stop persisting old thinking
   (F2, P4).
3. Cap `SubagentRecord::summary`, child streamed buffers and
   `SubtaskWidget::m_streamedMessage` (F3, T12).
4. Add `setTransferTimeout` + batch/seq tracking to the OTLP exporter, and read
   only new events for `traceSpans` (T3).
5. Cap `m_changedPaths`, `m_settledRanges` and `CostLedger::m_subagents` (F4).

### Wave 1 — Remove repeated large allocations (medium risk)

6. Incremental/streaming session save during a turn; full save at turn end
   (T1). Keep the current payload as the authoritative compact format.
7. Apply the 1 MiB analysis cap in `ProjectGraph::updateGraph` and skip
   re-indexing oversized documents (T5).
8. Stat-guard `write_file`/`edit_file`/`apply_patch` and spill patch originals to
   disk for rollback (T4).
9. Clear the parsed `SessionData` lists before rebuilding widgets; avoid storing
   the transcript twice (T2).
10. Lower the ACP ceilings and bound stdout reads (F5, T10).
11. Clear the Inspector documents on hide and build reports lazily (F7).

### Wave 2 — Structural reductions (refactor, larger effort)

12. Per-workspace harness services shared across windows: graph, MCP, event
    store, policy/hooks, memory loader (P1, F8). Fix `m_ownsMcp` as part of this,
    not by flipping the flag alone.
13. Add event-log retention and paged replay/export (P6).
14. Split the session payload into a paged message journal while keeping the
    current payload as a fallback (P5).
15. Calibrate and lower card budgets from measured bytes-per-char (P2), and
    either delete or narrowly use `TranscriptVirtualizer` for restored
    transcripts (P3).
16. Add a bounded-config reader helper and apply it to policy/hooks/ecosystem
    (P10).

### Wave 3 — Hygiene

17. Remove the dead per-loop `McpManager` allocation after the ownership fix.
18. Extend `memoryCounters()` and the budget tests to cover the new bounds.
19. Add a compile-time/CI check that no new `readAll()` call appears on a
    workspace-controlled path without a stat guard (grep-based lint).

---

## 5. Verification recommendations

- Extend `tests/test_memory_budget.cpp` with scenarios: 200-turn native
  conversation with thinking; 50 sub-agent runs; 5000-event telemetry export;
  5-minute voice capture; a 20 MiB `apply_patch`; 4 windows on one workspace.
  Measure RSS in a child process as the existing test does.
- Add assertions on `memoryCounters()`: `messageChars` below the cap,
  `thinkingChars` below the cap, `subagentSummaryChars` below the cap, and
  `sessionPayloadBytes` below a threshold.
- Instrument `traceSpans()` and `saveCurrentConversation()` with a one-line
  `qDebug` of elapsed ms and allocated bytes behind an env flag, to catch
  regressions in manual testing.
- Re-run the `docs/MEMORY_BASELINE.md` measurements after Wave 0 to confirm the
  high-water mark falls.

---

## 6. Checked and cleared (not findings)

These were inspected and are already bounded, or are not leaks in the current
code:

- Transcript/card caps: 1000 messages / 8 MiB, 400 tool cards / 6 MiB, 300 text
  cards / 4 MiB, 200 thinking blocks / 2 MiB, 50 activity pills, 100 suggestion
  cards, 40 sub-task cards, 64 pending tool calls, 64 pending edits.
- ACP tool results are clipped to 80 KiB history / 1 MiB UI
  (`agentloop.cpp:76-86`, `532-544`); native results use the 40k budget plus the
  content store (`agentloop.cpp:4879-4890`).
- LLM SSE 8 MiB, assistant 4 MiB, tool args 1 MiB/delta, completed calls 64,
  retained response 256 KiB.
- `read_file` streams line pages and caps the ledger at 4 MiB with a stat guard.
- `readDocumentLines` streams lines instead of `readAll` on the disk path.
- Tool result store: 32 MiB per entry, 200 MiB total prune.
- Check/hook/git output drains are capped at 8 MiB; sub-agent worktree diff at
  32 MB.
- `EditTracker` large old/new contents are spilled to an owner-only blob store;
  pending edits are capped (64, 4 MiB).
- `CompletionEngine` keeps one in-flight reply, cancels the old one and prunes
  request timestamps to 60 s.
- `WebSearch` caps fetches at 5 MiB, tracks replies/timers and aborts them.
- `McpClient` caps response bodies and times out initialize/tools/list/tools call.
- `SessionStore` fetches only titles for a valid-title row; `EventStore` pages
  export/fork/projection reads (the trace report is additionally capped at 2000
  events).
- `EditorAssist::m_attachments` removes entries on view destruction
  (`editorassist.cpp:684-693`), so the earlier "stale attachment" concern is
  already fixed.
- `DebugProvider` keeps 12 snapshots; `KateDebugBridge` 32k chars;
  `KateLspBridge` 50 messages.
- `ProjectGraph` caps nodes at 1000, previews at 4 KiB and the cache read at
  64 MiB.
- `ToolResults::prune` and `SessionStore` connection cleanup on thread finish.
- `ConfigPage` dialog is `WA_DeleteOnClose`; no persistent leak after close.
- `m_questionTexts` / `m_subtaskQuestionTaskIds` are cleared when answered and on
  turn end.
- `MemoryLoader` caps each file at 32 KiB and the set at 32 files.

---

## Appendix A — Exact references

| Finding | File:line |
| --- | --- |
| Model history append vs. transcript prune | `src/agentloop.cpp:4631-4684` |
| Transcript caps | `src/agentloop.cpp:96-97` |
| Compaction adoption gate | `src/agentloop.cpp:4090-4152` |
| Native request compaction call | `src/agentloop.cpp:4287-4290` |
| ACP turn-end compaction | `src/agentloop.cpp:5365-5367` |
| Reasoning accumulation (native) | `src/agentloop.cpp:387-389`, `4480`, `5044` |
| Reasoning/text accumulation (ACP) | `src/acpclient.cpp:3546-3558` |
| Thinking persisted | `src/sessionstore.cpp:123-124` |
| Sub-agent record + summary | `src/agentloop.h:58-83`, `674`; `src/agentloop.cpp:7297` |
| `sessionData()` copy | `src/agentloop.cpp:7690-7725` |
| Autosave timer | `src/chatwidget.cpp:454-460` |
| Autosave body | `src/chatwidget.cpp:2126-2170` |
| Session JSON | `src/sessionstore.cpp:300-380` |
| Session save | `src/sessionstore.cpp:1171-1270` |
| Session load | `src/sessionstore.cpp:1141-1169` |
| `clearAll` payload read | `src/sessionstore.cpp:1485-1500` |
| Full-file read | `src/documentbridge.cpp:225-251` |
| Save ledger read | `src/tools.cpp:1363-1395` |
| Write/edit full read | `src/tools.cpp:1517-1523`, `1577` |
| Patch old+new in RAM | `src/tools.cpp:3475-3565`, `3610-3650` |
| ACP full-file read | `src/acpclient.cpp:4781`, `4857`, `5257` |
| Trace spans full read | `src/agentloop.cpp:3853-3869` |
| Per-turn OTLP export | `src/agentloop.cpp:1900-1914` |
| OTLP pending/no timeout | `src/harness/otlp.cpp:153-230` |
| Replay full read | `src/harness/replay.cpp:119-123` |
| Headless full event read | `src/headless/main.cpp:157-158` |
| ACP live-tool cap | `src/acpclient.cpp:3595-3608`, `3661-3671` |
| ACP stream cap | `src/acpclient.cpp:3782-3795` |
| ACP parked methods | `src/acpclient.cpp:4423-4435` |
| ACP frame cap | `src/acpclient.cpp:47`, `3145`, `3189` |
| ACP read cap | `src/acpclient.cpp:4595-4600` |
| ACP terminals | `src/acpclient.cpp:6119-6145` |
| ACP stdout read | `src/acpclient.cpp:3119-3151` |
| ACP server sessions | `src/harness/acpserver.h:111`; `acpserver.cpp:286-322` |
| Headless session owns loop | `src/headless/session.h:84`; `src/headless/acpsession.h:32` |
| Inspector retention | `src/inspectorpanel.cpp:35`, `107`, `138-151` |
| Inspector creation | `src/chatwidget.cpp:3856-3862` |
| MCP ownership flag | `src/agentloop.h:665`; `agentloop.cpp:367`, `857`, `1682-1686` |
| MCP workspace load | `src/mcp.cpp:1142-1159` |
| MCP defaults | `src/mcp.h:302-306` |
| Graph update no cap | `src/graph/projectgraph.cpp:266-296` |
| Graph scan cap | `src/graph/projectgraph.cpp:138-139`, `241-251` |
| Typing graph update | `src/pluginview.cpp:225-236` |
| Config tabs | `src/configpage.cpp:101-1421` |
| Per-window widgets | `src/pluginview.cpp:146-157`; `src/chatwidget.h:366` |
| Per-loop services | `src/agentloop.cpp:360-370`; `src/agentloop.h:647-669`, `803`, `852-857` |
| Card caps | `src/chatwidget.cpp:3099-3160` |
| Card chars include result | `src/toolcallwidget.cpp:1044-1051` |
| Full result retained | `src/toolcallwidget.cpp:940` |
| Subtask stream | `src/subtaskwidget.cpp:534-543`, `578-581` |
| Voice WAV copies | `src/voiceinput.cpp:1630-1650`, `1680-1690`, `1739` |
| Headless full diff | `src/headless/main.cpp:65-96` |
| Completion body | `src/completionengine.cpp:318` |
| Event store cache | `src/harness/eventstore.cpp:115-141` |
| Event retention | `src/harness/eventstore.cpp:144-160` |
| Config readers unbounded | `src/harness/policy.cpp:26-36`; `src/harness/hooks.cpp:32-42`; `src/ecosystem.cpp:86`, `164` |
| Dead virtualizer | `src/transcriptvirtualizer.cpp`, `.h` (no external references) |

## Appendix B — Method

- Read the whole `src/` tree, the CMake source list, and the existing memory
  documentation (`RAM_SAVINGS_PLAN.md`, `MEMORY_BASELINE.md`).
- Traced every container that is a long-lived member of `AgentLoop`,
  `ChatWidget`, `AcpClient`, `McpManager`, `SessionStore`, `EventStore`,
  `ProjectGraph`, `EditTracker`, `ToolRunner`, `SubtaskWidget` and the UI maps.
- Traced every `readAll()` / `QByteArray` copy on a path that can carry
  workspace or model data, and checked for a preceding size/type guard.
- Checked call sites of the large readers (`traceSpans`, `sessionData`,
  `readDocument`, `applyPatch`, `configPath`, etc.) rather than only their
  definitions, because several "bounded" functions are called from a hotter path
  than their name suggests.
- No runtime profiling was performed in this pass; the byte estimates are
  order-of-magnitude and marked as such. The existing child-process RSS harness
  is the right place to turn them into measured assertions.
