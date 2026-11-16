# Changelog

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
  packaging. See `CHANGELOG_0.4.0.md` and
  `docs/RELEASE_0.4.0_AUDIT.md`.

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
