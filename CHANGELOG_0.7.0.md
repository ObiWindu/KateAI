# KateAI 0.7.0

0.7.0 is the harness release. A turn is now recorded as an append-only event
log, can be replayed, cannot quietly claim success after a failed check, and
is governed by declarative policy rules and hooks instead of prompt requests
alone. The editor integration, tools, sessions and checkpoints are unchanged.

## Session event log

- **Append-only event log.** A new `session_event` table in the existing
  session database records `turn/start`, `step/start`, `request/headers`,
  `assistant/message`, `tool/call`, `tool/result`, `permission/decision`,
  `hook/decision`, `verification`, `compaction` and `turn/end` events. One
  transaction appends the events, advances the conversation head and updates
  the message projection, so a crash cannot split them.
- **Legacy conversations are backfilled** once from the stored payload and
  marked `imported`. The payload remains the fast-load path and the downgrade
  safety net; the event tables are additive.
- **Redaction before write.** Bearer tokens, provider key prefixes,
  secret-shaped environment assignments and secret-named JSON fields never
  reach the event table.
- **Replay and export.** `:kateai trace` writes the redacted log as JSONL;
  `Replay::analyze` checks that every tool call has exactly one result and that
  sequence numbers are monotonic.

## Verification gate

- **Checks, not prose.** After a mutating turn, Kate AI discovers checks from
  `.kateai/config.json` (`verify`), a `## Commands` section in
  `AGENTS.md`/`KATEAI.md`/`CLAUDE.md`, the build system (CTest, Cargo,
  npm/pnpm/yarn, pytest, Make), or `git diff --check`.
- **The turn does not end on a failure.** Failure output is fed back once as a
  controller message; the model retries. Passing checks are appended as
  evidence and the final summary must cite them. `maxVerificationAttempts`
  caps the loop and leaves a visible notice.
- **Sandboxed and bounded.** Checks run through the active sandbox and
  scrubbed environment, at most three commands and ten minutes per
  verification. `:kateai verify` runs the plan on demand.

## Policy engine

- **Layered rules.** Managed (`/etc/kateai/policy.json`), project
  (`.kateai/policy.json`) and user (`~/.config/kateai/policy.json`) rules match
  tools, paths, command prefixes/regexes, risk and MCP server/tool.
- **Deny-first.** A deny in any layer beats an allow in any layer. Paths are
  resolved and symlinks followed before matching.
- **Immutable hardline.** `rm -rf /`, `mkfs`, raw device writes, force-pushes
  to protected refs, `.env`, private keys, `/etc/shadow`, `.ssh`, `.gnupg`,
  cloud credentials and container configs are denied even in always-approve.
- **Audit.** Every decision is recorded in the event log and in
  `~/.local/share/kateai/audit.jsonl` with credentials redacted. `:kateai
  policy` shows the effective rules.

## Hooks v1

- **Lifecycle commands.** Trusted user/managed hooks and approved project
  hooks run at `SessionStart`, `UserPromptSubmit`, `PreToolUse`,
  `PostToolUse`, `PostToolUseFailure` and `Stop`.
- **Deterministic decisions.** A hook can deny (JSON or exit code 2), merge
  `updatedInput` into the tool arguments, or add context. Timeouts, crashes and
  output caps follow `onFailure`; processes are killed, not orphaned.
- **Trust.** Project hooks are listed but do not run until `:kateai
  trust-hooks`. `:kateai hooks` shows the state. See `docs/HOOKS.md`.

## Slash commands

The ACP command button is gone. When the active agent advertises commands, the
composer placeholder mentions `/`, and typing `/` as the first character opens
a filtered list above the cursor. Each row shows the command and its
description; typing filters both, and Enter/Tab inserts the selected command.

## Middleware and turn identity

- **Turn phases.** `TurnContext` and `TurnController` give events, middleware
  and tests one vocabulary (`Starting → ModelWait → ToolWait → Verifying →
  Stopping → terminal`).
- **Composable policies.** `TurnMiddleware`/`MiddlewareChain` run pre-hooks in
  order and post-hooks in reverse, short-circuiting on deny/stop/retry. The
  verification gate is the first built-in middleware; budgets are available as
  a standalone middleware.

## Commands

- `:kateai verify`, `:kateai policy`, `:kateai hooks`, `:kateai trace` and
  `:kateai trust-hooks` join `context` and `usage`. `usage` now reports the
  harness flags, policy rule count, hook count and event-log conversation.

## Flags

All 0.7 features default on and can be disabled individually:

```
KATEAI_HARNESS_FLAGS=-verificationGate,-hooks,+eventLog
```

The flags are `eventLog`, `middleware`, `verificationGate`, `policyV2` and
`hooks`. They can also be set in the `[Harness]` config group. Turning one off
restores the legacy path without deleting the event tables.

## Tests

Nine new suites cover redaction, append/read ordering, projection rebuild,
legacy backfill, replay pairing, turn transitions, middleware ordering,
check discovery and execution, timeout killing, deny-first policy, symlink
resolution, audit redaction, hook trust/exit-2/timeout/updatedInput, and a
full `AgentLoop` run where a failing check blocks completion until a later
edit passes it (including the exported trace).
