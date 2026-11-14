# Plan: Tool Rework + Registry-Wide ACP Integration (Zed as reference)

Scope of the requested work, from a deep analysis of the KateAI codebase on branch
`acp_integration_tools`:

1. Rework all built-in tools so they are compatible with and up to date with the
   latest agentic-coding tendencies (structured todos, multi-file patches,
   plan-mode exit, full LSP surface, web research, sub-agents, ask-user).
2. Rework the ACP integration so every agent in the official ACP registry works,
   using Zed's ACP client (`crates/acp_thread`, `crates/acp_tools`,
   `crates/agent_servers` in zed-industries/zed) as the reference implementation.
3. Fix tools that have no real-time progress callbacks, and tools that simply do
   not work — specifically `question` and `suggest` over ACP.

Nothing in this plan is committed; it is the working document for the phases below.

## 0. References used

- **Zed ACP client**: `zed/crates/acp_thread` (Thread/Tool concepts routed
  out-of-process over stdio JSON-RPC), `crates/acp_tools`, `crates/agent_servers`
  (one module per external agent, incl. registry-driven launch lines), and Zed's
  tool-kind mapping from ACP `tool_call.kind` to editor tools.
- **ACP registry**: `https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json`
  (agentclientprotocol/registry). Distribution types: `binary` (per-platform
  `{archive, cmd, args, sha256}`), `npx` (`{package, args}`), `uvx` (`{package, args}`),
  plus top-level `env`, `version`, `homepage_url`, `license_url`.
- **ACP spec**: v1 stable (`protocolVersion: 1`; client methods
  `session/request_permission`, `fs/read_text_file|write_text_file`,
  `terminal/create|output|wait_for_exit|kill|release`). The v2 draft adds
  `elicitation/create|complete` (the standardized "ask the user" channel),
  session list/close, `$/cancel_request`, usage updates, `available_commands`.
  New features arrive as optional capabilities negotiated at `initialize`.

## 0.1 How the current code was analyzed

Read end to end or in targeted slices: `ARCHITECTURE.md`, `src/types.h/.cpp`
(tool defs, alias map, presets), `src/modes.cpp` (tool groups, `acpToolPermitted`),
`src/tools.h/.cpp` (`ToolRunner`, sync + async bash), `src/agentloop.h/.cpp`
(turn lifecycle, question parking, ACP signal wiring), `src/acpclient.h/.cpp`
(protocol, client-method dispatch, permissions, terminals, retries),
`src/acpinstall.cpp` (binary installer), `src/configpage.cpp` (ACP registry tab),
`src/chatwidget.cpp` (transcript, cards, question/suggest/progress wiring),
`src/toolcallwidget.cpp`, `src/questionwidget.cpp`, `src/suggestionwidget.cpp`,
`src/todowidget.cpp`, `tests/mock_acp_agent.py`, `tests/test_acpclient.cpp`,
`tests/test_questionflow.cpp`, `tests/test_suggestflow.cpp`, `tests/CMakeLists.txt`.
The previous integration plan (`docs/acp-tools-integration-plan.md`, removed in
commit `0a531a7`) was used as the format reference; its scope is finished, this
document covers the new scope.

---

## 1. Current state (analysis findings)

### 1.1 Native tool set

- Tool schemas: `toolDefinitions()` in `src/types.cpp:560` — `read_file`,
  `write_file`, `edit_file`, `multi_edit_file`, `list_dir`, `grep`, `glob`,
  `bash`, `web_search`, `web_fetch`, `query_project_graph`, `lsp_diagnostics`,
  `lsp_symbols`, `lsp_hover`, `lsp_definition`, `lsp_references`, `lsp_code_actions`,
  `write_plan_file`, `suggest`, `question`, `new_task` (subtaskToolName),
  `goal_report`.
- Canonical names: `allBuiltInToolNames()` in `src/modes.cpp:80`; tool groups
  (`read`/`edit`/`command`/`web`/`orchestrate`/`mcp`) in `builtInToolsForGroup()`
  `src/modes.cpp:51`; modes: `code`, `ask`, `architect`, `debug`, `orchestrator`
  + `.kateai/modes/` customs + a plan-mode toggle (`modes.cpp:121`).
- Runners: `ToolRunner` in `src/tools.cpp` (2511 lines). Only `bash` is async
  (`runBashAsync`/`bashProgress`, `tools.h:50-56`); everything else runs
  synchronously in `AgentLoop::processQueue()` via `m_tools->run(call)`
  (`agentloop.cpp` ~2236). Web tools and MCP tools are dispatched async
  (`isAsyncTool`, `startWebTool`, `dispatchAsyncTool`) and emit `toolProgress`.
- `question` is special-cased natively: the turn parks in
  `State::WaitingForQuestion` (`agentloop.cpp:2147-2168`), `questionAsked` is
  emitted, and `answerQuestion()` (`agentloop.cpp:3017`) feeds the user's words
  back as the tool result. `suggest` posts a card from the tool call at
  `toolStarted` (`chatwidget.cpp:968-975`) and settles at `toolFinished`
  (`chatwidget.cpp:1052-1068`); `ToolRunner::suggest` (`tools.cpp:2439`) only
  validates and confirms.
- Sandbox/permissions gate every call (`PermissionPolicy`, `Sandbox`); write
  tools take checkpoints and edit locks.

### 1.2 ACP client (Kate as client, agent as stdio subprocess)

- `AcpClient` (`src/acpclient.cpp`, 3547 lines): spawn → `initialize`
  (`protocolVersion` pinned to 1, `acpclient.cpp:38,1220-1243`) →
  `session/new` (`load`/`resume` when advertised) → `session/prompt`.
- Client capabilities advertised (`acpclient.cpp:1185-1200`):
  `fs.readTextFile/writeTextFile`, `terminal`, `kateai.{queryProjectGraph,
  writePlanFile, suggest}`, and `_meta["com.kateai/methods"]` =
  `_kateai/{query_project_graph, write_plan_file, suggest, lsp_diagnostics}`.
- Incoming requests dispatched in `handleIncomingRequest`
  (`acpclient.cpp:1848-1910`): `session/request_permission`, `fs/read_text_file`
  (line/limit, `:2632`), `fs/write_text_file` (parked behind permission,
  `:2654`), `terminal/*` (`:3013+`), and the four `_kateai/*` methods
  (`handleWritePlanFile` `:2693`, `handleSuggest` `:2719`,
  `handleQueryProjectGraph` `:2746`, `handleLspDiagnostics` `:2887`).
  Unknown methods get `kMethodNotFound`.
- Tool calls arrive as `session/update` `tool_call` / `tool_call_update`
  (`:1960-2110`): names canonicalized via `canonicalToolName`
  (`types.cpp:86`, alias map `types.cpp:20-78`), diffs parsed from
  `content[]`/`locations[]`/`rawInput`, progress aggregated
  (cumulative vs delta) into `toolProgress`, late diffs via `toolDetailsChanged`,
  terminal output linked to cards (`linkToolTerminals`).
- Permissions: `session/request_permission` (`:2439`) → `permissionNeeded`;
  `AgentLoop::acpAutoDecision` is the single policy point; mutating client
  methods park in `m_parkedMethods` until resolved (`ensureMethodPermission`).
- Session notes (`sessionNotesBlock`, `:1542-1565`) tell the agent once per
  session about the four `_kateai/*` methods.
- Model/effort switching via `session/set_config_option` with `session/set_model`
  fallback (Zed-style live switching), `ingestSessionModels/Efforts`.

### 1.3 Registry & agent launch plumbing

- Registry tab (`configpage.cpp:229-470`) loads the official registry JSON
  (`:261`), lists all agents, installs via `npm install -g` / `uv tool install`
  for npx/uvx (`:380-385`) or `AcpBinaryInstaller` for binary (download → sha256
  → extract → record `command`/`args`/`binaryInstallDir`, `acpinstall.cpp`).
- Installed agents stored in `Settings::acpInstalledAgents` (JSON);
  selectable via `acpAgentChoices()` (`types.cpp:1227`) = 13 hard-coded presets
  (`acpAgentPresets()`, `types.cpp:1191`) + registry installs + Custom.
- Launch line: `acpAgentLaunchLine` (`types.cpp:1255`) →
  `acpEffectiveCommand/Args/ApiKeyEnv` → `AcpClient::resolvedCommand`
  (`acpclient.cpp:554`, PATH/`~/.local/bin`/agent-specific fallbacks) and
  `agentArguments` (`acpclient.cpp:622`, per-agent fixups for grok / minimax /
  opencode `acp` / antigravity flags / `--model` insertion).

### 1.4 UI / transcript wiring

- `ChatWidget` (`chatwidget.cpp`, 6789 lines): tool cards via
  `toolStarted`/`toolFinished`/`toolDetailsChanged`, live output via
  `toolProgress` coalesced on a 50 ms timer (`flushToolProgress`, `:5817`),
  question card in the intent dock on `questionAsked` (`:1233` →
  `showQuestionCard` `:5269` → `answerQuestion` `:5279`), suggestion cards
  (`addSuggestionCard` `:5195`), subtask cards in the dock, permission bar.
- `ToolCallWidget` shows running state + elapsed time + streamed output;
  `TurnStatus` shows the current verb/target.

### 1.5 Tests

- `tests/mock_acp_agent.py` (scenario-driven stdio ACP agent; has `suggest`,
  double-permission, well-behaved edit scenarios), `tests/mock_llm_server.py`.
- Suites: `test_acpclient` (incl. `suggestClientMethodPostsAdviceTouchingNothing`
  `:849`), `test_acpparsing`, `test_acpstreaming`, `test_acpclientmethods`,
  `test_agentparity`, `test_questionflow`, `test_suggestflow`, `test_tools`,
  `test_modes`, `test_toolalias`, `test_livecards`/`test_toolcard`. Build+run:
  `./build.sh` (cmake + ctest).

---

## 2. Gap analysis — mapped to the three requested work items

### 2.1 Tool set vs. latest agentic tendencies (request 1)

| Gap | Where | Detail |
| --- | --- | --- |
| No structured todo tool | `types.cpp:560` (no `todo_write`/`todo_read`), `todowidget.cpp` exists but is driven by free-text plan | Latest agents (Claude Code-style, several registry agents) plan via a `todo_write` tool with statuses. Kate should add `todo_write`/`todo_read` wired to `TodoWidget` + plan checklist, replacing/augmenting text-plan parsing. |
| `apply_patch` aliased to `edit_file` | `types.cpp:41` (`map["apply_patch"] = "edit_file"`) | ACP agents (Codex, aider-style, OpenCode) send multi-file unified-diff patches. Canonicalizing to a single-file edit mangles them into one card/diff. Needs a first-class `apply_patch` tool (parse unified diff hunks, multi-file) or mapping to per-file `multi_edit_file`. |
| No `think`/reasoning tool | `modes.cpp:31` allows `think`, but no tool def | Some registry agents emit a think tool call; it currently falls through as an unknown tool. |
| No plan-mode exit tool | plan mode is a UI toggle (`Ctrl+Alt+P`, `modes.cpp:121`) | Latest tendency: model proposes plan, user approves, model calls `exit_plan_mode`. Add the tool so plan mode is model-drivable, not only UI-drivable. |
| `goal_report` is legacy | `types.cpp` last tool def | Keep for back-compat but fold progress reporting into `todo_write`; deprecate in the system prompt. |
| LSP surface incomplete over ACP | only `_kateai/lsp_diagnostics` advertised (`acpclient.cpp:1197`) | `lsp_symbols/hover/definition/references/code_actions` exist natively (`types.cpp`) but ACP agents cannot call them. |
| `web_fetch`/`web_search` thin | `types.cpp` web defs | No timeout / max-bytes / redirect policy params; provider fixed in settings. Add params with safe defaults. |
| Native/ACP arg drift | `fs/read_text_file` takes `line`/`limit` (`acpclient.cpp:2647`); native `read_file` takes `offset`/`limit` | Agents must learn two shapes; align native to accept `line` too (or document both), keep one canonical schema per tool. |

### 2.2 Registry-wide ACP support, Zed as reference (request 2)

| Gap | Where | Detail |
| --- | --- | --- |
| npx/uvx install does a global package install | `configpage.cpp:380-385` (`npm install --global`, `uv tool install`) | Zed runs `npx -y <pkg>` / `uvx <pkg>` directly without mutating the global env. Global installs fail without write perms and pollute the user's npm. Switch to no-install launch (still offer install as an option), and pin `@<version>` from the registry entry. |
| Registry `env` not applied | `envArrayToObject` used only for `terminal/create` params (`acpclient.cpp:3081`) | Registry entries carry a top-level/distribution `env` array (API key names, feature flags). `processEnvironment()` (`acpclient.cpp:1149`) must merge it. |
| Per-agent CLI quirks keyed to hard-coded ids | `acpclient.cpp:622-700` (grok, minimax, antigravity, opencode `acp`) | Registry agents with similar quirks (e.g. needing an `acp` subcommand) get no fixup — the `./opencode` → `acp` default only matches fileName exactly `opencode` (`:632`). Move quirks into a registry-id-keyed table applied to any agent, incl. newly installed ones. |
| Protocol version hard-fails on mismatch | `acpclient.cpp:1233-1243` | Zed negotiates: send our max, accept the agent's choice, gate optional features on it. v2 draft adds `elicitation/create|complete`, session list, `$/cancel_request`, `available_commands`. Negotiate instead of failing; support elicitation when offered. |
| No `available_commands_update` / slash-command handling | `handleNotification` (`acpclient.cpp:1917`) only handles `session/update` + `config_option_update` | Registry agents advertise slash commands; surface them (at least log/store + expose to UI later). |
| Only 4 Kate client methods | `acpclient.cpp:1197`, `sessionNotesBlock` `:1557-1560` | Extend the `_kateai/*` surface: `lsp_symbols`, `lsp_hover`, `lsp_definition`, `lsp_references`, `lsp_code_actions`, `ask_user` (see 2.3), `todo_write`. Advertise in `_meta` + `clientCapabilities.kateai` + session notes. |
| Alias map misses registry-agent tool names | `types.cpp:20-78` | Add the names registry agents actually send: `ask_user`/`ask_user_question`/`request_user_input`, `TodoWrite`/`todo_write`, `exit_plan_mode`, `apply_patch` (first-class), `View`/`Read`/`LS`/`Glob`/`Grep` (case variants already handled), `WebFetch`, `Shell`/`RunCommand`/`Exec`, `Patch`/`UpdateFile`, `multi_replace_file_content`, `move_file`/`rename`, `delete_file`. Zed's kind→tool mapping (read/search/edit/execute/fetch/think/switch_mode) is the reference for kind-only calls. |
| `apply_patch` diff parsing | `permissionFromToolCall` scans content blocks (`acpclient.cpp:2160+`) | Unified-diff text blocks (`--- a/ +++ b/ @@`) are not parsed into per-file diffs today; only oldText/newText pairs are. Add unified-diff parsing so multi-file patches render per-file cards and track per-file. |
| clientInfo spoof only for antigravity | `acpclient.cpp:1208-1215` | Keep, but make it a per-registry-id flag rather than a hard-coded check, so other restrictive agents can be handled the same way. |

### 2.3 Real-time progress / broken question & suggest (request 3)

| Gap | Where | Detail |
| --- | --- | --- |
| `question` does not work over ACP | `agentloop.cpp:99-104` forwards ACP `toolStarted` as a plain card; no `WaitingForQuestion`, no `questionAsked`, `answerQuestion` cannot reach an ACP agent | ACP v1 has no ask-user method, so agents either use `session/request_permission` with answer-like options (shown as a permission card, not a question card) or their own question tool (shown as a dead card). Fix: (a) add Kate extension client method `_kateai/ask_user {question, options?, allow_freeform?}` — the RPC itself parks the agent, Kate shows the question card and replies with the answer; (b) support v2 `elicitation/create`/`complete` when negotiated; (c) route ACP tool_calls whose canonical name is `question`/`ask_user` to the same question UI. |
| `suggest` over ACP only via one method | `handleSuggest` (`acpclient.cpp:2719`) | Works for `_kateai/suggest` and bare `suggest` RPCs, and a `suggest` tool_call gets a card via `chatwidget.cpp:968`. Gaps: no validation parity for `path` resolution against workspace, and no `_kateai/suggest` mention in some paths; unify through one handler used by both RPC names and tool_calls. |
| Native tools block the loop — no progress | `agentloop.cpp` ~2236 `m_tools->run(call)` synchronous; only bash/web/MCP/subtask async (`tools.h:50`) | `grep` over a big tree, `lsp_*`, big `read_file` freeze the transcript with only a spinner. Fix: make `ToolRunner` run every tool async with a generic `toolProgress(toolCallId, status/output)` callback (keep serialization, locks, checkpoints), mirroring how `runBashAsync` works today. |
| ACP title-only updates emit nothing | `acpclient.cpp:2085-2095` (title updates mutate `existing.summary` but return without a signal) | Card header goes stale. Emit `toolDetailsChanged` when the title changes. |
| Parked client methods are silent while waiting | `m_parkedMethods` / `ensureMethodPermission` | An agent parked on `fs/write_text_file` shows nothing until answered. Emit a status/progress line for parked calls. |
| Terminal output only streams when embedded in the tool call | `linkToolTerminals` (`acpclient.cpp`) | Agents that create a terminal via `terminal/create` and only reference it by id in updates get no streamed output on the card; final result falls back to `m_toolStreamed` (`:2054`). Link terminals by id reference in updates too. |
| `fs/read_text_file` result shape vs native | `:2632-2652` returns `{content}`; native `read_file` returns formatted text + pagination note | Agents comparing outputs get different shapes; align (add pagination note to ACP result too). |

---

## 3. Implementation plan

Phases are ordered so each is independently buildable and testable
(`./build.sh` + `ctest` after each). No phase commits; work stays on
`acp_integration_tools`.

### Phase 1 — Modernize the native tool schema (request 1)

1. `src/types.cpp` `toolDefinitions()` / `src/modes.cpp` `allBuiltInToolNames()` /
   `builtInToolsForGroup()`:
   - Add `todo_write` {todos: [{content, status: pending|in_progress|completed}]}
     and `todo_read`. Group: read+orchestrate-safe (like `write_plan_file`).
   - Add first-class `apply_patch` {patch: string} — unified diff, possibly
     multi-file — implemented in `tools.cpp` by parsing `--- a/ +++ b/ @@` hunks
     and applying per file (reuse `multi_edit_file` machinery per file, atomic:
     all-or-nothing). Group: edit.
   - Add `exit_plan_mode` {plan?} (leaves plan mode after user approval; no-op
     outside plan mode, reports current plan). Group: read.
   - Add `think` {thought} (records reasoning into the thinking block). Group: read.
   - Enrich `web_fetch` with `timeout_ms`, `max_bytes`; `web_search` keeps provider
     from settings but gains optional `site`.
   - Accept `line` as an alias of `offset` in `read_file`.
2. `src/tools.cpp` `ToolRunner`: implement `applyPatch`, `todoWrite`, `todoRead`,
   `exitPlanMode`, `think`; keep `question` defensive and `suggest` validation-only.
   Wire `todo_write` results to `TodoWidget` via a new `AgentLoop::todosUpdated`
   signal; `exit_plan_mode` flips the same plan-mode state as `Ctrl+Alt+P`.
3. `src/types.cpp` alias map: stop mapping `apply_patch`→`edit_file` (map to the new
   tool); add `ask_user`, `ask_user_question`, `request_user_input`, `TodoWrite`,
   `exit_plan_mode`, `WebFetch`, `Shell`, `RunCommand`, `Exec`, `Patch`,
   `UpdateFile`, `multi_replace_file_content`, `move_file`, `rename_file`,
   `delete_file`, `View`, `Read`, `LS`; extend kind fallbacks (`think`,
   `switch_mode`).
4. `defaultSystemPrompt()` (`types.cpp:~870+`): rewrite the tool guidance to the new
   set (todo-first planning, apply_patch for multi-file changes, exit_plan_mode in
   plan mode, lsp_* after edits, question/suggest semantics unchanged).
5. `README.md` tool table + ACP sections: update to match.
6. Tests: extend `tests/test_tools.cpp` (apply_patch multi-file atomicity,
   reject overlapping hunks, todo round-trip), `tests/test_modes.cpp` (new tools in
   groups), `tests/test_toolalias.cpp` (new aliases), `tests/test_agentparity.cpp`
   (native vs ACP parity for the new tools).

### Phase 2 — Full Kate client-method surface over ACP + working question/suggest (requests 2+3)

1. `src/acpclient.cpp` `handleIncomingRequest`: add handlers and register them in
   `kateClientMethod` aliases + `sendInitialize` `_meta["com.kateai/methods"]` +
   `clientCapabilities.kateai` + `sessionNotesBlock`:
   - `_kateai/ask_user {question, options?, allow_freeform?}` → park the RPC,
     emit a new `questionAsked(PermissionRequest)` signal, reply with
     `{answer}` when `AgentLoop` calls the new `answerAcpQuestion()`; refusal/
     cancel replies `{cancelled: true}`. Read-only: no sandbox, no permission.
   - `_kateai/lsp_symbols|lsp_hover|lsp_definition|lsp_references|lsp_code_actions`
     mirroring the native tools (`tools.cpp` lsp runners) through `m_lsp`, with the
     same "no LSP available" answer as `_kateai/lsp_diagnostics`.
   - `_kateai/todo_write|todo_read` forwarding to the same todo store the native
     tools use.
   - v2 draft: when negotiated (Phase 3), answer `elicitation/create` through the
     same ask-user path and `elicitation/complete` from the user's reply.
2. `src/agentloop.cpp`: connect `AcpClient::questionAsked` → re-emit
   `questionAsked` and park the ACP turn (`WaitingForQuestion`-equivalent state for
   ACP: the RPC is already parked client-side, so the loop just must not finish
   the turn UI); `answerQuestion()` routes to the ACP RPC when the waiting call
   came from ACP. Also route ACP `tool_call`s canonicalizing to `question`/
   `ask_user` into the same UI (agent-side tool; answer goes back as a normal
   `tool_call_update` result the agent produces — at minimum show the question
   card and let the user answer in the transcript).
3. `src/chatwidget.cpp`: show the question card for ACP-originated questions
   (same `showQuestionCard`, keyed by a synthetic toolCallId), settle suggestion
   cards from both RPC and tool_call paths through one code path; resolve
   `suggest.path` against the workspace before anchoring.
4. `permissionFromToolCall`: parse unified-diff text content blocks into per-file
   `describeDiff`/`additionalDiffs` so `apply_patch` renders per-file cards and
   edit tracking works per file.
5. Tests: `tests/mock_acp_agent.py` new scenarios (`ask_user` RPC, elicitation,
   apply_patch tool_call with unified diff, lsp_symbols call, title-only update);
   `tests/test_acpclient.cpp` for each; `tests/test_questionflow.cpp` gains an
   ACP-driven case; `tests/test_suggestflow.cpp` gains ACP path coverage.

### Phase 3 — Registry-wide agent support, Zed-referenced (request 2)

1. Protocol negotiation (`acpclient.cpp` `sendInitialize`): send our max supported
   `protocolVersion`, accept the agent's answer, store it; gate optional features
   (elicitation, session list, available_commands, `$/cancel_request`) on the
   negotiated version + advertised capabilities instead of hard-failing.
2. Launch-line rework (`types.cpp` presets/launch line, `acpclient.cpp`
   `resolvedCommand`/`agentArguments`, `acpinstall.cpp`):
   - Registry-driven everything: for an installed registry agent, command/args/env
     come from the registry entry (distribution `npx`/`uvx`/`binary` per platform),
     with `@<version>` pinning; presets become thin fallbacks for the same ids.
   - npx/uvx without global install by default (Zed-style `npx -y pkg` /
     `uvx pkg`); keep "Install globally" as an explicit secondary action.
   - Merge registry `env` (top-level and per-distribution) into
     `processEnvironment()`; keep the user's API-key env mapping on top.
   - Replace the hard-coded per-agent fixups with a quirks table keyed by registry
     id (grok stdio/--model/--always-approve, minimax mcode acp + credential
     tightening, antigravity sanitizer flags + page4k shim + zed clientInfo,
     opencode `acp` arg, generic "needs `acp` subcommand" default for binary
     entries whose `cmd` has no args).
   - Update flow: registry "Refresh" diffs versions; offer update (re-download
     binary / bump pinned version) instead of only install/uninstall.
   - `available_commands_update` notifications: store + `logMessage`, surface in
     the UI in a later phase.
3. `configpage.cpp` registry tab: show distribution kind per entry, version,
   env requirements (which env var holds the API key), update availability; keep
   search. Uninstall removes binary dir / pinned version entry and cached models.
4. `chatwidget.cpp` agent menu (`acpAgentChoices`) automatically picks up registry
   agents; ensure model/effort pickers work for any registry agent via
   `set_config_option` (already Zed-style), with `session/set_model` fallback.
5. Tests: `tests/test_acpclient.cpp` / new `test_acpinstall`-style suite with a
   fixture registry JSON (npx/uvx/binary, env, versions): launch-line resolution,
   env merging, quirks table, version pinning, update detection. Extend
   `mock_acp_agent.py` to advertise `available_commands` + elicitation capability.

### Phase 4 — Real-time progress for every tool (request 3)

1. `src/tools.h/.cpp` `ToolRunner`: generalize async execution —
   `runAsync(const ToolCall&)` returning immediately, emitting
   `toolProgress(toolCallId, partial)` for long tools (grep/glob with match-count
   heartbeats, read_file pagination, lsp queries "waiting on LSP…", apply_patch
   per-file progress) and `toolFinished(result)`; keep the existing sync `run()`
   for tests. Preserve ordering: one tool at a time per loop unless already
   async-safe (web/MCP/subtask), same locks/checkpoints.
2. `src/agentloop.cpp` `processQueue`: dispatch all tools through `runAsync`;
   remove the special-case sync `m_tools->run(call)`; keep `WaitingForQuestion`
   and permission parking semantics.
3. `src/acpclient.cpp` update handling: emit `toolDetailsChanged` when a
   title-only update changes the summary (`:2085-2095`); link terminals referenced
   by id in updates (`linkToolTerminals` on `{type:"terminal", terminalId}`);
   emit a progress line for parked client methods; keep cumulative/delta
   detection.
4. `src/chatwidget.cpp`: `flushToolProgress` already coalesces; make sure status
   text (`TurnStatus`) also updates on `toolDetailsChanged` summaries.
5. Tests: `tests/test_tools.cpp` async runner cases (progress emitted, result
   delivered, cancel works); `test_acpstreaming.cpp` for title-only + terminal-by-id
   updates; `test_livecards.cpp` for card updates.

### Phase 5 — UI wiring polish

1. Question cards from ACP in the intent dock (reuse `QuestionWidget`), retired
   on answer/cancel; `clearQuestionCards` on turn end already exists — verify it
   covers the ACP path.
2. Suggestion cards: single code path for RPC + tool_call; workspace-resolved
   anchor; accept still jumps to `path:line`.
3. TodoWidget driven by `todo_write` (native + ACP), merged with the plan
   checklist rendering; plan file (`write_plan_file`) gets an "open plan" affordance
   in the transcript header when the file exists.
4. `ARCHITECTURE.md` + `README.md` updated for every behavior change above.

### Phase 6 — Verification

- `./build.sh` (cmake + build + ctest) after each phase; fix fallout per suite:
  `test_tools`, `test_sandbox`, `test_modes`, `test_toolalias`, `test_acpclient`,
  `test_acpparsing`, `test_acpstreaming`, `test_acpclientmethods`,
  `test_agentparity`, `test_questionflow`, `test_suggestflow`, `test_livecards`,
  `test_toolcard`, `test_subtaskparallel`, `test_lsp`, `test_projectgraph`.
- Manual smoke with `tests/mock_acp_agent.py` scenarios: ask_user, elicitation,
  apply_patch, suggest, permission parking, terminal streaming.
- No commits at any point (per the task constraint); everything stays as working
  tree changes on `acp_integration_tools` for review.

## 4. Risks & notes

- Making all native tools async touches the loop's core (locks, checkpoints,
  duplicate-call recovery); Phase 4 is the riskiest and is sequenced after the
  protocol work so regressions are attributable.
- ACP v2 is a draft; gate elicitation/session-list behind negotiation so v1
  agents are unaffected.
- `apply_patch` parsing must reject overlapping hunks per file (same rule as
  `multi_edit_file`) and stay atomic across files.
- The `_kateai/*` extension namespace is ours; keep advertising both bare and
  `_kateai/` names for back-compat with agents already using them.
- Registry network calls already use `NoLessSafeRedirectPolicy`; keep that for
  update/download endpoints and verify sha256 on every binary download (already
  in `AcpBinaryInstaller`).

---

## 5. Implementation status (branch `acp_integration_tools`, uncommitted)

All phases implemented; `./build.sh` (cmake + build + ctest) is green: 50/50
test binaries pass, including the new cases below.

- **Phase 1 — tool schema**: `apply_patch` (first-class, multi-file unified
  diff, atomic; parser `parseUnifiedPatch`/`applyPatchToContent` in
  `types.cpp`, shared with ACP ingest), `todo_write`/`todo_read` (shared store
  in `ToolRunner`, rendered by `TodoWidget` via `planUpdated`),
  `think`, `exit_plan_mode` (new `AgentLoop::exitPlanModeRequested`, ChatWidget
  leaves plan mode), `web_fetch` gained `timeout_ms`/`max_bytes`, `web_search`
  gained `site`, `read_file` accepts the ACP-style `line` alias. Tool groups,
  alias map, system prompt, README/ARCHITECTURE updated. Tests:
  `test_tools` (apply-patch atomicity/per-file diffs, todo round-trip, think/
  exit_plan_mode, line alias, parser), `test_modes`, `test_llmparse`,
  `test_agentparity`.
- **Phase 2 — ACP surface**: new client methods `_kateai/ask_user` (parked RPC
  answered from `AgentLoop::answerQuestion`; v2 `elicitation/create` reaches the
  same path), `_kateai/todo_write|todo_read` (shared store + `todosChanged`),
  `_kateai/lsp_symbols|lsp_hover|lsp_definition|lsp_references|lsp_code_actions`;
  all advertised in `_meta`/`clientCapabilities`/session notes. ACP tool calls
  canonicalizing to `question`/`ask_user` raise the question card. Unified-diff
  text blocks in `tool_call`/`tool_call_update` are parsed into per-file diffs.
  `fs/read_text_file` results carry the pagination note. Tests: mock agent
  scenarios `ask_user`, `todo`, `lsp-symbols`, `title-update`, `apply-patch`,
  `commands` + 7 new `test_acpclient` cases.
- **Phase 3 — registry**: protocol version negotiated (no hard failure),
  `available_commands_update` handled, registry `env` merged into the agent
  process, per-agent launch quirks moved to a registry-id-keyed table
  (`acpAgentQuirks`), npx/uvx installs launch `npx --yes <pkg>@<version>` /
  `uvx` with no global install, registry list marks available updates.
- **Phase 4 — progress**: `ToolRunner::runReadToolAsync` offloads
  read_file/list_dir/grep/glob to a dedicated worker thread (disk-only bridge,
  stopped in `~ToolRunner`), wired through `AgentLoop::startReadTool` with the
  same progress/finish plumbing as bash. ACP side: title-only updates emit
  `toolDetailsChanged`, terminals referenced by id in updates are linked, parked
  permission-gated methods emit a progress line.
- **Phase 5 — UI**: ACP questions show the intent-dock card and are cleared on
  turn end; suggestion anchors are resolved against the workspace on both paths;
  todos render in `TodoWidget` from both native and ACP calls.
- **Verification**: full `ctest` run 50/50 green after the changes.
