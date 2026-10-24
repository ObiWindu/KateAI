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
  checkpoint.cpp                  pre-turn shadow-git snapshots
  edittracker.cpp                 review queue for agent edits
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
- Commands are classified read-only / dangerous / always-denied, and wrapped in
  bubblewrap under Workspace/Strict profiles.

When touching this code, add cases to `tests/test_sandbox.cpp` — the escape
tests there are the regression net for TOCTOU-style changes.

## Tool contract

`tools.cpp` implements the tool set advertised by `toolDefinitions()` in
`types.cpp` (name, description, JSON schema, access group). Conventions:

- Tools return a `ToolResult` with `output` (formatted, clipped) and `rawOutput`.
- `clip()` truncates at 80 KB **keeping head and tail** — for a command whose
  error summary is its last line, a head-only clip would throw away the answer.
- `read_file` paginates; long results end with a `more line(s) not shown` note
  that tells the model the next `offset`.
- `multi_edit_file` applies a batch atomically and **rejects overlapping edits**:
  each edit must target text none of the earlier ones consumed or produced,
  because matches are found in the working string after those edits.

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
  back into the transcript in place.
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
- `test_sandbox` — path escapes, deny globs, command classification
- `test_modes` — mode parsing, tool groups, per-mode sampling overrides
- `test_projectgraph` — scan limits, binary detection, symlink loops, round-trip
- `test_livecards` / `test_toolcard` / `test_overflow` — widget-level behaviour
- `test_subtaskparallel` / `test_multiagent` — sub-agent concurrency and locks

## Persistence

- **Settings**: KConfig via `settings.cpp` (global, not per workspace).
- **Conversations**: SQLite through `SessionStore`; every turn autosaves, and the
  active conversation id is repaired to the newest transcript when stale.
- **Workspace state**: `.kateai/` — `KATEAI.md`, `modes/`, the MCP server
  config, and the cached `project_graph.json`.
