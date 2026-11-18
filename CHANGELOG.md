# Changelog

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
