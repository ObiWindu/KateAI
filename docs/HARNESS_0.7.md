# Harness 0.7: substrate, verification, policy

Kate AI 0.7 makes a turn replayable and makes "done" mean "the check passed".
The work follows `docs/HARNESS_IMPLEMENTATION_PLAN.md` (EP-2 to EP-6) and is
backed by the tests in `tests/test_harness_*`.

## What ships

| Feature | Behaviour | Flag |
| --- | --- | --- |
| Session event log | Append-only `session_event` table in the session database; message projection; JSONL export | `eventLog` (on) |
| Replay | Every recorded tool call must pair with exactly one result; `:kateai trace` exports the log | `eventLog` (on) |
| Turn controller + middleware | Explicit turn phases and a composable policy chain; verification is the first built-in | `middleware` (on) |
| Verification gate | Discovers checks, runs them sandboxed after mutations, feeds failures back, caps retries | `verificationGate` (on) |
| Policy engine | Layered deny-first rules, immutable hardline, symlink-aware matching, audit trail | `policyV2` (on) |
| Hooks v1 | Trusted user/managed hooks and approved project hooks at tool/stop/session events | `hooks` (on) |

Flags are loaded from `[Harness]` in the Kate config and overridable with
`KATEAI_HARNESS_FLAGS` (for example `-verificationGate,+eventLog`). All six
default on in 0.7 and degrade to the legacy path when their configuration is
absent.

## Event log

The log lives in the same SQLite database as conversations
(`~/.local/share/kateai/sessions.sqlite`, or `KATEAI_SESSION_DB`). It is
append-only:

```sql
session_event(conversation_id, seq, ts, turn_id, step_id, type, payload, imported)
event_conversation_head(conversation_id, head_seq)
event_message_projection(conversation_id, seq, role, content, …)
```

Event types: `turn/start`, `turn/end`, `step/start`, `request/headers`,
`assistant/message`, `tool/call`, `tool/result`, `permission/decision`,
`policy/decision`, `hook/decision`, `verification`, `compaction`, `notice`,
`checkpoint`, `message`.

Properties:

- One transaction appends events and advances the head. A crash cannot leave a
  committed event without its projection.
- Payloads are redacted before writing: bearer tokens, provider key prefixes,
  secret-shaped environment assignments, and secret-named JSON keys.
- Opening a legacy conversation backfills its payload once, marked
  `imported: true`. The payload remains the fast-load path and a downgrade
  safety net.
- The message projection can be rebuilt from the log at any time.
- `Replay::analyze` validates call/result pairing and sequence ordering.

Export with `:kateai trace`. The JSONL file is redacted.

## Verification gate

Discovery order:

1. `.kateai/config.json` `verify` array (strings or
   `{ "command", "paths", "timeoutMs" }`).
2. `## Commands` / `## Checks` in `AGENTS.md`, `KATEAI.md`, `CLAUDE.md`
   (backticked commands or `- command` lines).
3. Build-system detection: CTest/CMake, Cargo, npm/pnpm/yarn, pytest, Make.
4. `git diff --check` in a git repository.

At most three commands run per verification, with a ten-minute total budget
and per-command timeouts. They run through the active sandbox and scrubbed
environment. A check whose explicit `paths` do not overlap the changed files
is skipped.

On a mutating turn's stop:

- Failure output is appended once as a controller message; the model gets a
  targeted retry.
- Success is appended as evidence and the model is asked for a final summary
  that cites it.
- After `maxVerificationAttempts` failures the turn completes with a visible
  notice; it never loops.

`:kateai verify` runs the plan on demand.

## Policy and hooks

See `docs/POLICY.md` and `docs/HOOKS.md`.

## Commands

| Command | Purpose |
| --- | --- |
| `:kateai verify` | Run the discovered check plan now |
| `:kateai policy` | Effective policy files, rules and warnings |
| `:kateai hooks` | Configured hooks and trust state |
| `:kateai trust-hooks` | Approve the current project's hook file |
| `:kateai trace` | Export the conversation event log as JSONL |
| `:kateai usage` | Counters plus harness/policy/hook status |

## Compatibility and rollback

- The session payload version stays at 3; event tables are additive.
- Turning `eventLog` off stops writes and reads the payload as before; it does
  not drop the tables.
- Turning the verification gate off restores the one-turn prose nudge.
- Turning policy/hooks off restores the legacy permission policy and ignores
  hook files.
- No new runtime dependencies: Qt Core/Sql plus the existing sandbox.

## Not in 0.7

Prompt assembler v2, context engine v2, deferred tool search, skills,
subagent v2, the inspector UI, telemetry/OTLP, and the headless CLI remain on
the roadmap for 0.8–1.0.
