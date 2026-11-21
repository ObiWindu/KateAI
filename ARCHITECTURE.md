# Kate AI — Architecture

Developer map of the plugin. User-facing documentation lives in `README.md`.

## Layout

```
src/
  plugin.cpp / pluginview.cpp     KTextEditor plugin + tool view, actions, shortcuts
  kateaicommand.cpp               the ":kateai" editor command-line command
  chatwidget.cpp                  transcript, composer, model picker, mode menus
  toolcallwidget.cpp              one tool-call card (header, diff, output, copy)
  turnstatus.cpp                  live status strip (elapsed time, current activity)
  agentloop.cpp                   the turn: prompt -> model -> tools -> model -> ...
  llmclient.cpp / acpclient.cpp   provider transports (HTTP formats, native ACP)
  tools.cpp / sandbox.cpp         every built-in tool and the path sandbox around them
  modes.cpp                       built-in + .kateai/modes/ mode definitions
  sessionstore.cpp                conversation persistence (SQLite)
  contextmanager.cpp              prompt assembly, context window, compaction
  graph/projectgraph.cpp          workspace structure graph
  mcp.cpp                         Model Context Protocol stdio clients
  lsp.h / lsp.cpp                 the editor's language-server surface (LspProvider) the core queries
  katelspbridge.cpp               Kate-facing LspProvider: probes the lspclientplugin at runtime,
                                  reads the diagnostics model (document marks as fallback), captures LSP messages
  debug.h / debug.cpp             debugger session model, snapshot parsing/diffing, launch.json generation
  katedebugbridge.cpp             Kate-facing DebugProvider: kategdbplugin Tier-1 model/signal bridge,
                                  Tier-2 Q_INVOKABLE upgrade when the plugin view exposes it
  ecosystem.h / ecosystem.cpp     offline parsers and models for the Kate plugin bridges
  kateecosystembridge.cpp         bridges to katebuildplugin, katekonsoleplugin, kateprojectplugin,
                                  katebacktracebrowserplugin, and the native review/debug/graph tabs
  checkpoint.cpp                  pre-turn shadow-git snapshots
  edittracker.cpp                 review queue for agent edits
  completionengine.cpp / editranges.cpp  in-editor completions (core half): FIM/chat prompts, budgets, MovingCursor-anchored edit ranges
  editorassist.cpp               in-editor pixels: ghost text, Tab accept, hover hints, one per main window
tests/                            one QTest binary per component (see below)
```

Two link targets:

- **`kateai_core`** (static lib) — everything without a Kate GUI dependency.
  AgentLoop, LlmClient, AcpClient, ToolRunner, Sandbox, SessionStore, ProjectGraph,
  ModeRegistry. Tests link against this.
- **`kateai`** (KTextEditor plugin) — Kate-facing widgets only: ChatWidget,
  ToolCallWidget, TurnStatus, ConfigPage, PluginView.

Rule of thumb: new logic that a test can drive belongs in `kateai_core`; new
pixels belong in `kateai`.

## The turn

`AgentLoop` owns one conversation. The lifecycle:

1. `submit()` appends the user message and builds the system prompt
   (`buildSystemPrompt()`): mode persona, rules (`.kateai/`, `KATEAI.md`), editor
   context, project graph summary, sandbox/permission summary.
2. `LlmClient::complete()` streams tokens. Thinking deltas and content deltas
   arrive separately, so the UI can render hidden reasoning collapsibly.
3. Tool calls are executed through `ToolRunner`, gated by `PermissionPolicy`
   (Ask / AcceptEdits / AlwaysApprove) and `Sandbox` (path + command checks).
   File-mutating calls are snapshotted by `CheckpointManager` first and queued
   into `EditTracker` under AcceptEdits.
4. Tool results go back to the model; the loop repeats until the model finishes
   or a budget (`maxModelRequests`, `maxToolCalls`) is exhausted.

Everything is asynchronous: one `bash` process runs detached, MCP calls run over
stdio JSON-RPC, sub-agents (`new_task`) run as nested `AgentLoop`s.

## ACP tool integration

When the provider is an ACP agent (`usesAcpNative`), `AgentLoop` delegates the
turn to `AcpClient` instead of `LlmClient`. Tool calls arrive as
`session/update` notifications (`tool_call`, `tool_call_update`); Kate renders
cards and tracks edits from those.

### Name normalisation

ACP agents use their own tool names (`fs/read_text_file`, `terminal/exec`,
`edit`, `write`, `apply_patch`, …). `canonicalToolName()` in `types.cpp` maps
these to KateAI's canonical names (`read_file`, `bash`, `edit_file`, `write_file`,
…) on ingest in `AcpClient::permissionFromToolCall()`. MCP-prefixed names
(`mcp__*`) are preserved. The canonical name drives `ToolAccess`, `ChatTheme`,
`shellCommandFor`, edit tracking, and history restoration; the original name is
kept in `PermissionRequest::originalToolName` for display/debugging.

A call that names no tool falls back to its ACP `kind` (`read` → `read_file`,
`search` → `grep`, `fetch` → `web_fetch`), and a built-in name in another case
(`Grep`, `BASH`) is the same tool.

### Parsing & diffs

`permissionFromToolCall()` normalises `rawInput` (object/string/array), scans
`content[]` and `locations[]` for paths and diff blocks, resolves relative paths
against the workspace, and builds a unified diff from `oldText`/`newText` or
`old_string`/`new_string` (camelCase and snake_case both accepted). Each diff is
kept with the file it edits: the primary diff is the one for the primary file,
and the others go to `additionalPaths`/`additionalDiffs` in matching order. A
diff that arrived only as a content block is also written into the stored
arguments, so a restored transcript rebuilds the same card.

`tool_call_update` merges what an update adds into the live entry
(`mergeToolCallDetails`) and emits `toolDetailsChanged` when a diff or path
arrives late, so the card and the review entry catch up. Progress is aggregated
(cumulative or delta).

### Permissions

`AcpClient::handleRequestPermission()` merges the request with the tool call
the agent already announced (a request often names only the call's id) and
emits `permissionNeeded`. `AgentLoop::acpAutoDecision()` is the one decision
point for ACP, the same policy native turns use: session grants, the
auto-approve list, AlwaysApprove and AcceptEdits allow, and everything else
asks the user. A tool the mode does not allow is not declined here; the
permission card still lets you approve it.

`acpToolPermitted()` (in `modes.cpp`) judges a tool the mode allows: a tool Kate
knows is judged by its name, and an agent's own tool by what it does (running a
command needs the command tool, changing a file needs the edit tools, anything
else is not restricted). Reasoning is always allowed.

The agent runs its own tools, so Kate cannot stop a tool that asks no permission;
it shows a notice instead, and the permission card still lets you approve a tool
the active mode does not list.

### Terminals

A terminal the agent creates with `terminal/create` and embeds in a tool call
(`{type: "terminal", terminalId}`) is linked to that call. Its output streams
into the card while the command runs, and the final result falls back to that
output. The terminal reference itself is never printed as output.

### Edit tracking

In every permission mode for ACP turns, `ChatWidget` tracks mutations detected
by protocol kind, `mutatesFiles`, diff presence, or name heuristics. Multi-file
edits are tracked per file. Snapshots are taken at `toolStarted`; paths are
resolved against the workspace.

### History restoration

`ChatWidget::restoreHistoricalTool()` canonicalises the tool name and reconstructs
diffs from stored arguments for native and ACP shapes, sets the file path for
language highlighting, and restores subtask/suggestion cards.

### Client methods

Kate exposes `fs/read_text_file`, `fs/write_text_file`, `terminal/*`, and its own
`_kateai/query_project_graph`, `_kateai/write_plan_file`, `_kateai/suggest`,
`_kateai/ask_user`, `_kateai/todo_write`, `_kateai/todo_read`, and the `lsp_*`
family (`_kateai/lsp_diagnostics`, `_kateai/lsp_symbols`, `_kateai/lsp_hover`,
`_kateai/lsp_definition`, `_kateai/lsp_references`, `_kateai/lsp_code_actions`).
The extension names are the ones the protocol reserves for
extensions; they are advertised in the `initialize` capabilities under `_meta`
(`com.kateai/methods`). The bare names (`write_plan_file`, …) still reach the
same handlers. Results are formatted to match native tool output, including the
pagination note on reads. `suggest` emits `suggestionPosted` which `ChatWidget`
turns into a suggestion card; `ask_user` parks the RPC, emits `questionAsked`
(the same signal the native `question` tool drives), and answers the RPC from
`AgentLoop::answerQuestion` — the v2-draft `elicitation/create` reaches the same
path.

Agents that expose their own question/suggestion tools are normalized too:
`permissionFromToolCall()` accepts the flat `{question, options}` shape, the
Claude/OpenCode `{questions:[{question, options:[{label}]}]}` shape and plain
text, and canonicalizes names like `suggestion`, `ask_question`,
`ask_user_question` or a title that says "ask user". A question tool call shows
the same dock card; because ACP has no client-to-agent tool-result channel, its
answer is sent as the next user prompt after the current turn closes. A
`session/request_permission` whose options are answer labels rather than
allow/reject kinds is shown as a question card too, and the selected chip is
returned as the permission request's `optionId`. A suggestion tool call becomes
a normal suggestion card through `toolStarted`. `todo_write`/`todo_read` share the native todo store through a
getter/setter pair installed by `AgentLoop::syncAcpClient`, and emit
`todosChanged`, which the loop forwards as `planUpdated` so `TodoWidget` renders
them. Slash commands are stored and announced via `availableCommandsChanged`:
the `session/new`/`session/resume` `availableCommands` field is read, and the
protocol's `session/update` → `available_commands_update` variant replaces the
list (the old top-level notification is accepted as a compatibility fallback).

### Protocol negotiation and agent quirks

`initialize` sends Kate's maximum `protocolVersion` and accepts the agent's
answer clamped to what Kate speaks (a v2 agent is clamped to v1 and simply does
not get the v2 features; elicitation is enabled when the agent advertises the
`elicitation` capability). Per-agent launch quirks (an `acp` subcommand,
npm-token stripping, `--model` support, sanitizer flags, Zed clientInfo spoof)
live in `acpAgentQuirks()` keyed by registry id, so agents installed from the
ACP registry get the same treatment as the built-in presets. Registry `env`
arrays are merged into the agent process environment in `processEnvironment()`.
Package-distribution agents are launched via `npx --yes <pkg>@<version>` /
`uvx <pkg>@<version>` with no global install.

## LSP bridge

`LspProvider` (lsp.h) is the interface the core (`ToolRunner`, `AcpClient`)
queries for language-server data: availability, per-file diagnostics, server
messages, and optional symbols/hover/definition/references/code-action answers.
`KateLspBridge` (katelspbridge.cpp) implements it against Kate's LSP client
plugin: it looks up `lspclientplugin` through the application plugin list
(the same string-based, runtime-probed pattern used for `kateprojectplugin`),
captures its public `showMessage` signal, and reads diagnostics from the
model behind the plugin's DiagnosticsView — severity, source, code, and range,
including for workspace files that are not open. When that model is not
available it falls back to the Error/Warning marks of open documents,
re-scanned on `marksChanged` plus a slow poll so `diagnosticsChanged` fires.
When the plugin is absent or changes its internals, `isAvailable()` is false
and the `lsp_*` tools report the feature as unavailable instead of failing;
symbol/hover/definition/references then fall back to the project graph (which
now indexes per-file symbols with line ranges) and plain text search. The
`lsp_*` tools are read-only, so they are allowed in
plan/ask/architect/orchestrator modes and never prompt.

Agents do not receive Kate's system prompt, so the first prompt of each ACP
session starts with a notes block: the user's global rules and extra system
prompt (except for Grok, which gets them through `_meta`), and the list of
Kate's client methods. The notes are sent once per session.

## Debugger bridge

`DebugProvider` (debug.h) is the interface the core (`ToolRunner`, `AgentLoop`)
queries for debugger data: availability, session state, stop reason, threads,
frames, locals, breakpoints, recent output, and actions (select frame/thread,
set/clear breakpoints, step, evaluate, control the session, console, reload
launch targets). `KateDebugBridge` (katedebugbridge.cpp) implements it against
Kate's debugger plugin (`kategdbplugin`, GDB/DAP):

- **Tier 1** works on stock Kate 25.x/26.x. The bridge finds the plugin view
  through `MainWindow::pluginView()` (and follows `pluginViewCreated`), reads
  the plugin's stack/frame/locals/breakpoint models, and watches their change
  signals, so the snapshot the agent receives tracks the live session.
- **Tier 2** is auto-detected at runtime: when the plugin view exposes the
  richer `Q_INVOKABLE` surface, the bridge calls it for exact state and
  actions, and keeps Tier 1 as the fallback.
- The model-layout parsers are static members of `DebugProvider`, so
  `test_debug` covers them offline without a GUI or a live GDB.
- Without the plugin, `isAvailable()` is false, the debugger tools return an
  actionable message, and `bash` remains the fallback.

`AgentLoop` folds the current stop into the prompt and editor context, and the
plugin view raises the crash/exception banner, inline variable notes, hover
values, and the status pill from the same state.

## Ecosystem bridge

`KateEcosystemBridge` (kateecosystembridge.cpp) resolves the sibling Kate
plugins by id at runtime — `katebuildplugin`, `katekonsoleplugin`,
`kateprojectplugin`, `katebacktracebrowserplugin` — and exposes only what each
one advertises. A missing plugin makes the matching tool unavailable instead of
failing. `build_targets`/`build_run` read the Build plugin's target model,
`terminal_run` drives the Konsole plugin, project-tree and backtrace actions
use their menu/view surfaces, and review diffs, debug diffs, and the project
graph open as native `KTextEditor` tabs from the plugin view.

## Settings and modes

`Settings` is one flat struct persisted by `settings.cpp` (KConfig). Modes
(`modes.cpp`) narrow **which tools** the model may call via `ToolAccess`
(exact names + prefixes for MCP tools) and may override the **sampling knobs**:

```
mode.temperature / mode.topP   # -1 = not set, use the global Settings value
```

`ModeRegistry::settingsForMode()` merges the two; `AgentLoop` hands the merged
copy to `LlmClient` and keeps the raw struct for the config page, so an override
never leaks back into saved settings. Built-in modes set neither override.

## Sandbox invariants

`Sandbox::normalizePath()` is the single gate for file tools:

- Relative paths resolve against the workspace root.
- Symlinks are resolved with `canonicalFilePath()`, and containment is
  re-checked **after** resolution — the classic escape (`workspace/link ->
  /etc`) is refused for writes in every profile but `Off`.
- Deny globs (`.env`, `**/.ssh/**`) apply on top of the profile.
- Commands are classified read-only / dangerous / always-denied and run inside
  an OS sandbox: bubblewrap on Linux and `sandbox-exec` on macOS. The
  workspace, private temporary directories, credentials, environment, and
  network are confined by the profile; Read-only/Strict fail closed when no
  isolation mechanism is available.

When touching this code, add cases to `tests/test_sandbox.cpp` — the escape
tests there are the regression net for TOCTOU-style changes.

## Tool contract

`tools.cpp` implements the tool set advertised by `toolDefinitions()` in
`types.cpp` (name, description, JSON schema, access group): file tools,
`apply_patch` (multi-file unified diff, atomic — parsed by `parseUnifiedPatch`/
`applyPatchToContent` in `types.cpp`, shared with the ACP ingest path),
`bash`, web tools, the `lsp_*` family, `query_project_graph`, and the UI-state
tools `write_plan_file`, `suggest`, `question`, `todo_write`/`todo_read`,
`think`, `exit_plan_mode`, and `new_task`. Conventions:

- Tools return a `ToolResult` with `output` (formatted, clipped) and `rawOutput`.
- `clip()` truncates at 80 KB **keeping head and tail** — for a command whose
  error summary is its last line, a head-only clip would throw away the answer.
- `read_file` paginates; long results end with a `more line(s) not shown` note
  that tells the model the next `offset`.
- `multi_edit_file` applies a batch atomically and **rejects overlapping edits**:
  each edit must target text none of the earlier ones consumed or produced,
  because matches are found in the working string after those edits.
- `apply_patch` plans every file before writing, accepts the standard
  `@@ -0,0` new-file range, distinguishes an empty file from a deletion, and
  rolls back already-written files if a later write fails.
- Read-only disk tools (`list_dir`, `grep`, `glob`) are offloaded to a worker
  thread by `ToolRunner::runReadToolAsync` through a bridge that never touches
  KTextEditor documents, emitting `bashProgress`/`bashFinished`, so a long
  search streams instead of freezing the transcript. `read_file` stays
  synchronous because it must see the editor's unsaved buffer; tools that touch
  the project graph or the LSP provider stay synchronous too. A stop cancels
  the in-flight jobs and a turn generation marks late results stale.

## UI structure

- `ChatWidget` builds the transcript as a vertical list of widgets (user
  bubbles, assistant text, thinking blocks, plan checklists, tool cards,
  suggestion/question cards, sub-task cards) inside a `QScrollArea`.
- Tool cards (`ToolCallWidget`) are stateful: header summary, expand/collapse,
  diff preview, live output while running, copy button (full output, not the
  truncated view), and a file-path jump signal wired to `openFileAtLineRequested`
  so a click opens the file in Kate at that line.
- The **intent dock** above the composer holds anything awaiting the user
  (approval rows, question cards, live sub-agent cards); retired widgets move
  back into the transcript in place. A running sub-agent card streams its tool
  starts/finishes, streamed command output, progress text and latest step into
  the card's activity log as they arrive; complete output lines are coalesced
  on a short timer so a chatty child does not rebuild the document per chunk.
- `TurnStatus` is the busy strip: elapsed timer, verb + target of the current
  tool ("Reading src/agentloop.cpp"), running sub-agents, completed tool count.

Keyboard shortcuts (panel-local, `QShortcut` on ChatWidget):

| Shortcut            | Action                          |
| ------------------- | ------------------------------- |
| `Ctrl+Alt+E`        | Expand / collapse all tool cards |
| `Ctrl+Alt+Up/Down`  | Previous / next tool card       |
| `Ctrl+Alt+T`        | Expand / collapse thinking      |
| `Ctrl+Alt+P`        | Toggle plan mode (read-only)    |
| `Ctrl+Alt+Left/Right` | Previous / next model         |

## Tests

`tests/CMakeLists.txt` defines one QTest binary per component; core-level tests
need no GUI, widget tests add `../src/<widget>.cpp` and run `-platform offscreen`.
Some suites drive real subprocesses against `tests/mock_llm_server.py`,
`tests/mock_mcp_server.py` and `tests/mock_acp_agent.py`.

Build and run everything:

```sh
./build.sh          # cmake + build + ctest
```

Useful single suites:

- `test_tools` — every built-in tool, sandbox interaction, atomicity rules
- `test_sandbox` — path escapes, deny globs, command classification, namespace/
  mount construction, and live bubblewrap confinement checks where available
- `test_modes` — mode parsing, tool groups, per-mode sampling overrides
- `test_projectgraph` — scan limits, binary detection, symlink loops, round-trip
- `test_livecards` / `test_toolcard` / `test_overflow` — widget-level behaviour
- `test_subtaskparallel` / `test_multiagent` — sub-agent concurrency and locks
- `test_lsp` — diagnostics parsing (structured rows and mark fallback), symbol/hover lookups
- `test_debug` — debugger model parsers, stop classification, snapshot diffing, launch.json generation

## Persistence

- **Settings**: KConfig via `settings.cpp` (global, not per workspace).
- **Conversations**: SQLite through `SessionStore`; every turn autosaves, and the
  active conversation id is repaired to the newest transcript when stale.
- **Workspace state**: `.kateai/` — `KATEAI.md`, `modes/`, the MCP server
  config, and the cached `project_graph.json`.
