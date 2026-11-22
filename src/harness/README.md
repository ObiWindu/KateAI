# `src/harness`

The harness layer owns the agent's substrate: durable events, the turn state
machine, policy decisions, hooks, verification and replay. It links into
`kateai_core` and must not include Qt Widgets or KTextEditor headers, so tests
and a future headless host can drive it without the editor.

| File | Responsibility |
| --- | --- |
| `harnessflags.*` | Rollout flags (KConfig + `KATEAI_HARNESS_FLAGS`) |
| `events.*` | Event vocabulary, redaction, message projection |
| `eventstore.*` | Append-only SQLite store and legacy backfill |
| `replay.*` | Turn reconstruction and pairing validation |
| `turn.*` | `TurnContext`, `TurnController` phases |
| `middleware.*` | `TurnMiddleware` interface and chain |
| `checks.*` | Check discovery, sandboxed runner, verification middleware |
| `policy.*` | Layered deny-first rules and hardline |
| `hooks.*` | Hook config, trust, sandboxed execution |
| `audit.*` | Append-only JSONL permission trail |
| `toolresults.*` | Content store for oversized tool output |

Rules:

- No UI code, dialogs or widgets.
- Every decision that a user can be surprised by must emit an event or a
  notice.
- A new policy belongs in middleware or a policy object, not another branch in
  `AgentLoop`.
- Tests link against `kateai_core`; `tests/test_harness_*` is the suite.
