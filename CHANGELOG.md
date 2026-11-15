# Changelog

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
