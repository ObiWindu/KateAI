# KateAI 0.6.0

0.6.0 connects Kate AI to the rest of Kate. The agent can now read the live
debugger, query real structured diagnostics from the language servers, drive
the build plugin and the embedded terminal, and open review diffs and the
project graph in native editor tabs. Existing settings, sessions, checkpoints,
and on-disk formats are unchanged; upgrading is in place.

## Live debugger

- **Bridge to Kate's debugger.** `KateDebugBridge` (katedebugbridge.cpp)
  connects to the `kategdbplugin` plugin (GDB, and DAP through the same
  plugin). It uses a Tier-1 bridge over the plugin's public model and signals,
  which works on stock Kate 25.x/26.x, and automatically upgrades to a Tier-2
  path when the plugin view exposes the richer `Q_INVOKABLE` surface. When the
  plugin is missing, the debugger tools say so and suggest `bash` instead of
  failing.
- **Live debug context.** While a session is paused, the current stop reason,
  frame, source location, and nearby variables are folded into the prompt and
  the editor context, so the agent starts from what the debugger knows rather
  than from a description the user has to type.
- **Crash and exception banner.** A stop caused by a crash, signal, or
  exception raises a banner on the stopped file with **Diagnose Stop** and
  **Explain** actions. The transcript starts with the stack and locals already
  in hand.
- **Snapshots and diffing.** Debugger state can be captured as a snapshot and
  compared with a later one (`debug_snapshot`), which makes "what changed
  between these two stops?" a single tool call.
- **`.vscode/launch.json`.** `:kateai launch-config` (and the menu action)
  derives launch targets from the project's build system and asks before
  overwriting an existing file.

## Debugger and build tools

Twelve debugger tools join the tool set:

| Tool | What it does |
| --- | --- |
| `debug_status` | Session state, stop reason, current frame, and thread list |
| `debug_stack` | Backtrace for the selected thread |
| `debug_locals` | Locals for a frame, optionally filtered |
| `debug_breakpoints` | Breakpoints with conditions and hit counts |
| `debug_select_frame` / `debug_select_thread` | Change the active frame or thread |
| `debug_snapshot` | Capture or diff debugger state |
| `debug_set_breakpoint` | Set, condition, or remove a breakpoint |
| `debug_step` | Continue, step over/into/out, or run to a line |
| `debug_eval` | Evaluate an expression in the paused frame |
| `debug_session` | Start, stop, or attach a debug session |
| `debug_console` | Run a debugger console command |

`build_targets` lists what the project can build; `build_run` builds a target
through Kate's Build plugin; `terminal_run` runs a command in Kate's embedded
Konsole terminal. All of them are available to native and ACP agents through
the same tool runner, with mode tool groups controlling who may use them
(`ToolGroup::debug()`).

## Diagnostics

- **Correct plugin lookup.** The bridge now probes the real
  `lspclientplugin` id, so LSP discovery works on current Kate builds.
- **Structured diagnostics.** `lsp_diagnostics` reads the diagnostics model
  behind Kate's DiagnosticsView, which carries severity, source, code, and an
  exact range. It covers workspace files that are not open in the editor
  (LSP, build plugin, and other producers), with the previous document-mark
  reader kept as a fallback for environments without that model.
- **Graceful degradation.** When no language server or diagnostics model is
  present, the tools report that and the agent falls back to the project graph
  and text search.

## Ecosystem

- **Build plugin** (`katebuildplugin`): target discovery and `build_run`.
- **Terminal plugin** (`katekonsoleplugin`): `terminal_run`, plus
  `:kateai terminal` to explain the current selection or command.
- **Project plugin** (`kateprojectplugin`): project-tree context and menu
  actions.
- **Backtrace browser** (`katebacktracebrowserplugin`): `:kateai backtrace`
  explains a crash dump opened there.
- **Native tabs**: review diffs (with **Accept All** / **Reject All**), debug
  diffs, and the project graph open as Kate tabs instead of only inside the
  panel.

## Editor assistance

- Inline variable notes on the paused line, and hover values for variables
  under the cursor while debugging.
- A toggleable `KTextEditor::AnnotationModel` gutter marking AI edits
  (`:kateai annotations`).
- A clickable status-bar pill for the current debugger or agent state.
- Menu and context-menu actions for the debugger, build errors, backtraces,
  terminal, git blame, commit message drafting, and the native tabs.
- New `:kateai` subcommands: `debug`, `stack`, `break`, `snapshot`,
  `fix-errors`, `build-errors`, `launch-config`, `backtrace`, `terminal`,
  `blame`, `commit`, `diff-tab`, `graph-tab`, `annotations`.

## Safety

- Read-only debugger tools (`debug_status`, `debug_stack`, `debug_locals`,
  `debug_breakpoints`, `debug_select_frame`, `debug_select_thread`,
  `debug_snapshot`, `build_targets`) are classified as reads, so they need no
  prompt and are allowed in plan, ask, architect, and orchestrator modes.
- `debug_set_breakpoint` is a write (the breakpoint's file path is resolved
  through the sandbox), and `debug_step`, `debug_eval`, `debug_session`,
  `debug_console`, `build_run`, and `terminal_run` are executes, so they use
  the normal permission bar and sandbox checks.
- ACP agents receive the same tools through the ACP client bridge and are
  gated the same way.

## Packaging

- The Fedora RPM now owns `%{_libdir}/kateai/libpage4k.so` (the 4K-page
  sandbox shim) and its debuginfo, and the stale changelog date was corrected.
  `rpmbuild` passes on x86_64 and aarch64.
- ACP agent installation fixes from the integration branch are included:
  archive/raw-binary handling, staged swaps, and shim installation.
- Version metadata is consistent at 0.6.0 across CMake, `kateai.json`, the ACP
  client identity, README, and every package recipe.

## Compatibility

- No settings, session, checkpoint, or workspace format changes; 0.5.x data
  opens as-is.
- The `lsp_*` tools and existing editor commands keep their names and
  arguments.
- The new debugger and build tools appear in the default mode tool groups; a
  mode that does not list them simply never advertises them.
