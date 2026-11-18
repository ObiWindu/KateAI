# KateAI 0.5.0

0.5.0 focuses on correctness under real agent traffic: ACP launch and protocol
handling, sandbox and permission boundaries, checkpoint and patch safety, and
restoring the conversation exactly as it looked when Kate was closed. Existing
settings, sessions, and workflows keep working; no manual migration is needed.

## Highlights

### Reliable conversation restore

- Per-window conversation ids are retained even when Kate destroys its windows
  before writing the session config, so reopening the editor no longer opens a
  blank panel.
- A stale per-window id falls back to valid history instead of an empty chat.
- An unreadable stored transcript is left untouched and a fresh chat is started
  so autosave cannot overwrite the only recoverable copy.
- Switching conversations marks the loading state before aborting the live
  turn, preventing late stream events from being saved under the new chat.

### Conversation cards

- Restored tool calls are keyed by occurrence rather than by id, so providers
  and sub-agents that reuse ids (`call_0`, retried delegations) come back as
  separate cards with their own output.
- ACP `fs/read_text_file` results restore as reads instead of edits.
- Cancelled tool and sub-agent results restore as **Cancelled**, not as green
  successes or red failures.
- Question cards keep the question next to the answer, and suggestion cards
  keep the advice, including alias tool names and `_kateai/*` client methods.
- JSON arguments and results are re-indented and labelled; command output and
  diffs remain verbatim.

### Sub-agent cards

- The header shows agent name, readable mode, state, elapsed time, and real
  tool/error counts.
- The live line names the tool in flight, colour-coded by tool kind, with
  `Doing` / `Done` / `Failed` prefixes. A fresh run shows `Starting…` instead
  of an empty card.
- Finished answers offer **Copy** and keep cancelled runs visually distinct.
- Concurrent cards share the bounded tab strip, and tabs render plain text
  instead of literal HTML.

### ACP agents

- Registry installs support raw-binary distributions, archive detection by URL
  and magic bytes, staged downloads, musl target selection, and safe updates.
- Registry env maps (object or array) are read, tool managers in nvm, Volta,
  fnm, Bun, pnpm, asdf, Homebrew, and `~/.local/bin` are discovered, and the
  resulting `PATH` is exported to the agent.
- Package agents stay pinned (`npx --yes <package>@<version>`,
  `uvx <package>==<version>`), and uninstall no longer removes arbitrary paths.
- Launch arguments with whitespace are quoted; unresolved relative commands
  fail closed where a staged install is expected.
- `session/update` and requests are scoped by session id, replayed history on
  `session/load` is suppressed, and session setup is no longer cut off by the
  30-second request timeout.
- Auth methods prefer env/key flows, `session/new` retries once after an auth
  failure, and advertised capabilities are schema-valid.
- MCP servers are filtered by `mcpCapabilities`; stdio commands are
  absolutized.
- `fs/read_text_file` returns exact content (bounded in size) without injected
  pagination prose, and terminal output honours `outputByteLimit`.
- Approval grants are turn-scoped, parked methods are bounded by count and
  bytes, and cancellation closes parked cards.
- Streamed tool output is capped and rate-limited; update-only tool calls and
  terminal/tool link ownership are tracked correctly.

### Sandbox and permissions

- Newline-separated commands are treated as chains and `sudo`, `doas`, and
  `pkexec` are never auto-approved.
- Read-only shell reads are confined to the workspace; outside paths require
  approval, and truncated glob expansion fails closed.
- Deny-glob scanning handles backslash escapes, subshells, and nested
  `sh -c` scripts.
- Write-capable flag spellings (`sort --output=`, `tail --follow=`, `rg --pre`,
  …) and mutating git subcommands are rejected.
- Root spellings such as `rm -rf //` and `$HOME` targets are blocked.
- Deny globs match case-insensitively on Windows/macOS, and macOS path-case
  differences no longer count as inside the workspace.
- Tool names are canonicalized before policy evaluation and execution, so
  aliases cannot skip hard-deny checks.
- ACP filesystem and terminal paths are resolved before use, mode tool access
  is enforced, and approval grants are scoped to the request that was shown.
- Credentials embedded in URL values are stripped from the sandbox
  environment, and the preload shim is only loaded from installation or system
  directories.

### Checkpoints, edits, and patches

- Checkpoints no longer depend on `/dev/null` for `GIT_CONFIG_GLOBAL`, so they
  work on Windows.
- Git is probed again after a failure instead of caching “unavailable” for the
  session, and an interrupted shadow repository is rebuilt.
- `AgentLoop::createCheckpoint` reports its failure reason; manual checkpoint
  actions show it, and sub-agent checkpoints are rejected explicitly.
- Checkpoint restore is symlink-safe, fails closed on protected-path removal
  and staging errors, and bases pruning on activity rather than creation time.
- Rejecting an agent edit can be undone, while reverts that would discard newer
  user edits are refused.
- `apply_patch` deletes are atomic, hunk counts and line numbers are validated,
  and `--- content` lines stay inside hunks.
- Moving-cursor ownership is respected, and cursors from destroyed documents
  are not reused.

### Session storage and data integrity

- A corrupted session database is quarantined with its WAL/SHM sidecars rather
  than losing the newest committed transactions.
- Database files are tightened to owner-only permissions, and 0 = Unlimited
  remains unlimited.
- Payloads that fail to load are not overwritten by an empty transcript.
- Stored sessions are compacted only when compaction saves a material amount,
  the undo stack is capped, MCP timeout timers are deleted, and conversations
  are pruned only when a new one is added.

### LLM and web transports

- Claude-compatible endpoints no longer duplicate `/v1`.
- Retry budgeting is bounded, permanent 4xx/billing errors are not retried, and
  huge retry delays are clamped.
- `error: null`, string/null tool indexes, partial SSE events, and late
  transport errors after a finished stream are handled.
- `reasoning_content` is not replayed, and Anthropic extended thinking is not
  requested on tool-use turns whose blocks cannot be replayed.
- ACP prompt ids are not clobbered; timers, stale approval cards, duplicate
  failures, parked-method completion, tool-call status, session-open races, and
  mid-turn stop handling are fixed.
- MCP headers/env secrets stay in the credential store, config loads and HTTP
  bodies are bounded, multi-line SSE and batches parse, and auto-approve
  changes are persisted before use.
- MCP tool names resolve to their original server spelling.
- Trailing-dot internal hosts are blocked, blocked redirects abort, and direct
  result URLs are decoded.
- SearXNG endpoint queries are preserved and DuckDuckGo snippets no longer
  shift onto the wrong result.

### Project graph

- Files over the old 64 KB scan limit are indexed up to 1 MB, so the largest
  source files have real line counts and symbols.
- Dependency lists are sorted, making the committed cache reproducible.
- An older cache schema is rebuilt, directory timestamps are considered, and
  the committed cache no longer embeds full file contents.
- The graph summary and `query_project_graph` return workspace-relative paths,
  real language/line metrics, and resolved dependency edges.
- Compaction uses model-family context windows and preserves recent tool
  failures and project-graph related files in the summary.

### UI and long sessions

- Reasoning that arrives after an answer starts is buffered and flushed when the
  card freezes, so reasoning and answers render sequentially.
- A stopped turn freezes its live card; the next turn starts in a fresh card.
- Token estimation has an ASCII fast path.
- Long-lived sub-agent activity and streamed output remain bounded.

## Packaging

- Version metadata is consistent at 0.5.0 across CMake, `src/kateai.json`, the
  README badge, the ACP client identity, and all packaging manifests.
- The release workflow comment and source-tarball defaults are updated for
  `v0.5.0`.

## Validation

- All 54 CTest suites pass locally.
- Regression coverage was added for hard-deny bypasses, patch parsing and
  rollback, checkpoint symlink restore, MCP secret redaction, LLM stream/retry
  edge cases, unlimited history, web host blocking, environment scrubbing,
  restored sub-agent cards, repeated tool-call ids, cancelled results, and
  conversation restore across widget teardown.
