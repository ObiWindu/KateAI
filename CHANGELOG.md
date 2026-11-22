# Changelog

## Unreleased — 0.8.0

- **Prompt assembler v2.** The system prompt is now a deterministic, ordered
  section registry with per-section and total budgets and a stable static
  prefix. Editor state, git status, and workspace listings stay in a transient
  turn message, verification prose is consolidated, and `:kateai prompt` dumps
  the exact prompt with its token manifest. See `docs/PROMPT.md` and
  `docs/HARNESS_0.8.md`.
- **Model profiles.** A family registry now supplies context windows, output
  limits, prices, cache semantics, prompt variant, and edit dialect for every
  provider. Unknown models get a conservative 32k fallback and a visible
  notice. Anthropic requests use explicit cache breakpoints.
- **Gemini explicit cache resources.** Gemini models configured through the
  OpenAI-compatible provider on Google's Generative Language API now use the
  native transport. Kate creates a `cachedContents` resource for the system
  instruction, tool declarations, and first exchange, then sends only the
  suffix with `cachedContent`; changed or expired caches are rebuilt.
- **Context engine v2.** Graduated shapers page oversized tool output, prune
  old output, snip old turns, merge an incremental summary, and fall back to
  full compaction. Rehydration re-attaches the plan, touched files, running
  tasks, and skills. Provider usage calibrates the local token estimate;
  summaries preserve content-store ids so paged output stays reachable.
- **Memory hierarchy v2.** Managed, user, project, nested, and local scopes;
  `AGENTS.md`/`CLAUDE.md`/`KATEAI.md` and rules directories; `paths:` globs,
  priorities, deduplication by path and hash, and 32 KiB/128 KiB/32-file
  budgets. The new `memory_write` tool proposes a diff and writes only to the
  memory directory after approval. See `:kateai memory`.
- **Tool registry v2.** Declarative `ToolSpec` metadata, byte-identical
  schemas, per-tool result budgets, and a BM25 `tool_search` /
  `tool_describe` / `tool_call` bridge. MCP tools defer by default when search
  is on; a 100-tool catalog now advertises only the core set plus the bridge.
  See `:kateai tools`.
- **Edit canonicalisation.** `edit_file` retries a failed exact match against
  folded smart quotes, CRLF, trailing whitespace, and whitespace runs, applying
  the replacement at mapped original offsets. Ambiguous canonical matches still
  fail; no line-number edit path was added.
- **Reports.** `:kateai context` and `:kateai usage` now include prompt
  sections, context shapers, profile pricing, and token calibration.
  `:kateai prompt`, `memory`, and `tools` are new.
- All four 0.8 flags default on (`promptV2`, `contextV2`, `memoryV2`,
  `toolSearch`) and degrade to the legacy path when disabled.
- **0.8 hardening.** Prompt truncation is token-accurate for non-ASCII text and
  reserves tool-schema cost; context summaries from both engines are
  interchangeable and persisted once; memory discovery keeps Cline/Kilo and
  mode-specific rules, scopes root config directories correctly, and enforces
  byte budgets; globstar matches zero directories; usage parsing accepts both
  streaming shapes and calibrates Anthropic cache hits; reports use the real
  prompt/request; and `memory_write` honours sandbox read-only/deny rules and
  writes through the document bridge.

## 0.7.0 — 2026-11-21

- **Session event log.** Conversations are now recorded as an append-only
  `session_event` log in the existing SQLite database, with a rebuildable
  message projection, redaction before write, one-time legacy backfill, and
  JSONL export via `:kateai trace`. See `docs/HARNESS_0.7.md`.
- **Verification gate.** A mutating turn cannot finish while the discovered
  check plan fails: failures are fed back with a retry cap, passes become the
  cited evidence, and `:kateai verify` runs the plan on demand.
- **Policy engine.** Managed/project/user rules are deny-first with
  symlink-aware path matching and an immutable hardline floor that survives
  always-approve. Decisions are audited in the event log and
  `~/.local/share/kateai/audit.jsonl`. See `docs/POLICY.md`.
- **Hooks v1.** Trusted user/managed hooks and approved project hooks run at
  session, prompt, tool and stop events with timeouts, exit-code-2 blocking,
  `updatedInput` and `onFailure`. See `docs/HOOKS.md`.
- **Middleware.** Explicit turn phases plus a composable middleware chain;
  verification is the first built-in.
- **Commands.** `:kateai verify`, `policy`, `hooks`, `trust-hooks` and `trace`
  join `context` and `usage`.
- **Slash commands in the composer.** The old ACP **cmds** button is gone.
  With an ACP agent selected the placeholder mentions `/` from startup, and
  typing `/` as the first character opens a filtered list above the cursor
  with each command's description. The list is cached per agent, so reopening
  Kate shows it before a new session starts; with no cached list, `/` explains
  that the agent advertises its commands when a session starts.
- All six harness flags default on (`eventLog`, `middleware`,
  `verificationGate`, `policyV2`, `hooks`) and can be disabled with
  `KATEAI_HARNESS_FLAGS` or `[Harness]`.

## Unreleased — 0.6.1

- **Context visibility.** `:kateai context` reports the model window, the
  request estimate, the system/ambient/tool-schema/conversation breakdown,
  rules and graph sizes, and the free space. `:kateai usage` reports provider,
  model, mode, turn state, request/tool counters and the estimated request.
- **Stable prompt prefix.** Editor context, the workspace listing and git
  status moved out of the system prompt into a transient turn-context message.
  The system prompt and tool schemas are now stable across turns, which lets
  providers reuse their prompt cache; ambient state is never persisted.
- **Tool-result budgets.** Tool output over 40K characters is stored outside the
  conversation and replaced with a head/tail preview. The new
  `read_tool_result` tool pages it back by character offset. MCP and ACP output
  is clipped before it enters history instead of arriving unbounded.
- **Write conflict detection.** `write_file` remembers files read during the
  turn and refuses to overwrite one that changed afterwards; the model is told
  to re-read and retry. Files never read keep the create/overwrite behaviour.
- **ACP slash commands.** When an agent advertises commands, the composer shows
  a **Commands** button that inserts the selected `/command` into the prompt.
- **Sub-agents fail fast instead of hanging.** A sub-agent's question is
  answered with an instruction to proceed without the user, and a permission
  ask is denied, with an activity line explaining why. Sub-agents cannot prompt
  the user.
- **Removed dead surface.** The orphaned `goal_report` tool, the
  `run-workflow` skill that documented a non-existent tool, and eight settings
  that were persisted and editable but unused.
- Version metadata is unchanged in this entry; the release commit bumps it.

## 0.6.0 — 2026-11-21

- **Live debugger integration.** Kate AI reads Kate's debugger plugin
  (`kategdbplugin`, GDB/DAP) through a model-and-signal bridge with an
  auto-detected `Q_INVOKABLE` upgrade path. The agent sees the session state,
  paused frame, call stack, locals, and breakpoints; that context is injected
  into the prompt, a stopped or crashed session raises a banner, and debugger
  state can be snapshotted and diffed.
- **Twelve debugger tools**: `debug_status`, `debug_stack`, `debug_locals`,
  `debug_breakpoints`, `debug_select_frame`, `debug_select_thread`,
  `debug_snapshot`, `debug_set_breakpoint`, `debug_step`, `debug_eval`,
  `debug_session`, and `debug_console`, plus `build_targets`, `build_run`, and
  `terminal_run`. They run through the tool runner, agent loop, mode tool
  groups, and the ACP bridge with the same risk classification and sandbox
  gates as `bash`.
- **Rich LSP diagnostics.** The bridge resolves the `lspclientplugin` plugin
  id and reads structured rows from Kate's diagnostics model, falling back to
  document marks. `lsp_diagnostics` now reports severity, source, code, and
  range instead of flattened mark text, including diagnostics for unopened
  workspace files.
- **Kate ecosystem bridge.** Build-plugin targets, the Konsole terminal's
  selection and current command, project-tree actions, and the Backtrace
  Browser are reachable from the agent and the menus. Review diffs, debug
  diffs, and the project graph open in native Kate tabs (review diffs keep
  **Accept All** / **Reject All**).
- **Editor assistance while paused.** Inline variable notes on the paused
  line, hover values, a toggleable annotation gutter for AI edits, and a
  clickable status-bar pill that jumps back to the panel.
- **New `:kateai` subcommands**: `debug`, `stack`, `break`, `snapshot`,
  `fix-errors`, `build-errors`, `launch-config`, `backtrace`, `terminal`,
  `blame`, `commit`, `diff-tab`, `graph-tab`, and `annotations`.
  `:kateai launch-config` generates a `.vscode/launch.json` from the project's
  build targets; `:kateai backtrace` explains a crash dump.
- Fixed ACP agent installation edge cases and the page-size shim install path;
  the Fedora RPM now packages the shim and passes rpmbuild on x86_64 and
  aarch64.
- Version metadata updated to 0.6.0 across CMake, plugin metadata, README, ACP
  client identity, and packaging. See `CHANGELOG_0.6.0.md`.

## 0.5.1 — 2026-11-18

- Fixed `test_sandbox` on Fedora aarch64: the bubblewrap runtime probe now
  exercises the actual wrapper for each sandbox profile, so hosts that cannot
  create the network or cgroup namespace required by Strict/Read-only skip
  those runtime assertions instead of failing. The probe also no longer
  assumes a particular host shell path.
- No user-facing behavior changes; the 0.5.0 features and on-disk formats are
  unchanged.

## 0.5.0 — 2026-11-18

- Hardened ACP agent installation and launch: raw-binary distributions,
  archive type detection, staged swaps, registry env maps, musl targets,
  tool-manager discovery, pinned `npx`/`uvx` lines, and safe uninstall paths.
- ACP protocol correctness: session-scoped updates and requests, no replayed
  history on `session/load`, no forced 30-second session setup timeout,
  auth-method fallback, capability-filtered MCP servers, bounded file reads,
  terminal output limits, scoped approval grants, and bounded parked methods.
- Closed sandbox and permission bypasses around newline command chains,
  `sudo`/`doas`/`pkexec`, glob expansion before deny scanning, write-capable
  flags, mutating git subcommands, path case rules, and tool aliases. ACP paths
  are resolved before policy is applied.
- Made checkpoint handling more robust across Windows and interrupted
  repositories, added failure reasons to the checkpoint API, and made restore
  symlink-safe with fail-closed staging.
- Improved data safety: WAL/SHM sidecars are kept when a session database is
  quarantined, unreadable payloads are never overwritten, unlimited history
  stays unlimited, deletes in `apply_patch` are atomic, and undo/reject paths
  refuse to discard newer edits.
- Hardened LLM transports and retries: no duplicated `/v1`, bounded retries,
  permanent HTTP errors are not retried, and partial SSE events, late
  transport errors, null tool indexes, and non-replayable reasoning are handled.
- MCP and web-search fixes: secrets stay in the credential store, config/HTTP
  loads are bounded, multi-line SSE and batches parse, internal hosts and
  blocked redirects are rejected, and search snippets no longer shift between
  results.
- Project graph improvements: files up to 1 MB are indexed, dependency lists
  are stable, cache generation is reproducible, and JSON arguments/results are
  formatted in cards while prose is left untouched.
- Conversation history now restores reliably after closing and reopening Kate,
  including per-window conversation ids, stale ids, repeated tool-call ids, ACP
  read cards, and cancelled results.
- Reworked sub-agent cards: readable mode, live tool and state, elapsed time,
  tool/error counts, a starting indicator, cancelled-state handling, and a
  copy-answer action. Concurrent cards keep their bounded tab strip, with
  plain-text tabs.
- Question and suggestion cards keep their prompt/advice payload alongside the
  answer, including alias tool names and ACP client methods.
- Reasoning and answers render sequentially, completed cards freeze cleanly,
  and long-running sessions compact without growing stored history unbounded.
- Version metadata updated to 0.5.0 across CMake, plugin metadata, README, ACP
  client identity, and packaging. See `CHANGELOG_0.5.0.md`.

## 0.4.0 — 2026-11-16

- API keys move from the plaintext KConfig file into `CredentialStore`:
  KDE Wallet when available, otherwise an atomic owner-only
  `credentials.json`. Legacy keys and `credentials.conf` migrate automatically.
- A corrupted SQLite session database is verified with `quick_check`, moved to
  `sessions.sqlite.corrupt-<timestamp>`, and replaced, instead of silently
  showing an empty history. Session payloads and the schema are versioned.
- Checkpoint restore saves a `Before restoring <id>` safety snapshot first and
  fails closed if the snapshot cannot be written.
- `edit_file`, `multi_edit_file`, and `apply_patch` reject writes when the file
  changed after it was read, so newer user or external edits are not clobbered.
  New-file patch hunks use exclusive creation and refuse a file that appeared
  after planning.
- Disk writes preserve existing file permissions.
- Stdio MCP servers and ACP `terminal/create` commands receive a sanitized
  environment instead of every ambient provider credential.
- Sandboxed shell and ACP terminal commands now run under OS-level isolation:
  bubblewrap namespaces, mount isolation, private `/tmp`, capability drop,
  network namespaces for Read-only/Strict, masked credential stores, and a
  scrubbed session environment. Read-only and Strict fail closed where no
  isolation mechanism exists.
- Question and suggestion cards now read the field names and nested shapes
  providers actually send, render their body with a theme-aware document, and
  size themselves after layout instead of leaving a blank, oversized card.
- Context settings now map to real behavior: dead knobs were removed, defaults
  for graph/editor/instruction budgets were raised, the project graph is saved
  on first index and resolves imports to real file relationships, and
  compaction uses model-aware windows plus graph/failure-aware summaries.
- Added an explicit per-turn `TurnState` lifecycle and persisted turn ids; the
  status strip now shows starting, model-wait, and stopping phases from it.
- Fixed a crash when an ACP parked `fs/write_text_file` / `terminal/create`
  permission was answered synchronously.
- Fixed assignment-shaped `api_key=...` redaction.
- Version metadata updated to 0.4.0 across CMake, plugin metadata, README, and
  packaging. See `CHANGELOG_0.4.0.md`.

## 0.3.0 — 2026-11-15

- ACP question and suggestion cards now work across agent naming schemes:
  `_kateai/*` client methods, elicitation, answer-shaped permission requests,
  and agents' own `question`/`suggest`/`ask_user` tool calls.
- Sub-agent cards stream tool starts/finishes, progress text and live command
  output into their activity log.
- `apply_patch` supports new-file hunks, keeps an emptied file instead of
  deleting it, rejects duplicate file sections, and rolls back partial writes.
- Replaced asynchronous `read_file` with a synchronous read for unsaved
  buffers; search/listing work now uses a genuinely disk-only worker.
- `todo_write`, `todo_read`, `think` and `exit_plan_mode` are treated as
  read-only UI tools and no longer prompt or take checkpoints.
- ACP slash commands are read from setup responses and `session/update`
  notifications; `fs/read_text_file` pagination no longer invents a trailing
  line; numeric string positions are accepted throughout the LSP tools.
- `web_search.site`, `web_fetch.timeout_ms` and `web_fetch.max_bytes` are
  honored.
- Restored history keeps `apply_patch` diffs, and git branch context no longer
  blocks the UI on cursor movement.
- Improved tool schemas and documentation; expanded regression coverage.

## 0.1.0 — 2026-09-30

- Initial release.
