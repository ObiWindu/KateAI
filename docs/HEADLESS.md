# Headless CLI

`kateai-headless` drives the same harness as the editor plugin without a UI.
It links `kateai_core` and uses `AgentLoop`, `DiskDocumentBridge`, the event
log, policy, hooks and the verification gate. LSP, debugger and ecosystem
providers are absent; those tools report themselves as unavailable.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target kateai-headless
```

The binary installs to `KDE_INSTALL_BINDIR` next to the plugin. It is not a
runtime dependency of the plugin.

## Run

```sh
kateai-headless run \
  --workspace ~/src/project \
  --prompt "Fix the failing test and verify it." \
  --provider openai-compatible \
  --base-url http://127.0.0.1:8080/v1 \
  --model local-model \
  --api-key local \
  --permission auto \
  --sandbox workspace \
  --output-dir ./kateai-run
```

Options:

| Option | Meaning |
| --- | --- |
| `-p, --prompt <text>` | Prompt for the run (required) |
| `-C, --workspace <dir>` | Workspace root; defaults to the current directory |
| `-o, --output-dir <dir>` | Artifact directory |
| `--provider <id>` | `grok`, `openai`, `openrouter`, `deepseek`, `openai-compatible`, `claude-compatible`, `kilo`, `opencode` |
| `--base-url <url>` | Override the provider base URL |
| `-m, --model <id>` | Model id |
| `--api-key <key>` | Key for this run; never written to settings |
| `--permission <mode>` | `ask`, `accept-edits`, `auto`, `bypass` |
| `--sandbox <profile>` | `off`, `workspace`, `read-only`, `strict` |
| `--mode <id>` | Agent mode (default `code`) |
| `--max-turns <n>` | Model-request ceiling; `0` = unlimited |
| `--max-cost <usd>` | Hard per-run cost budget; exits 3 when a priced turn crosses it |
| `--no-verify` | Disable the verification gate |
| `--conversation <id>` | Use an explicit conversation id |
| `--session-db <path>` | Session/event SQLite path (`KATEAI_SESSION_DB`) |
| `--no-persist` | Do not create a conversation row; events still use the log |
| `--answer <text>` | Answer the first question automatically |
| `--jsonl` | Stream harness events to stdout as JSONL |
| `--quiet` | Do not stream assistant text to stdout |

`--permission ask` is deliberately non-interactive: the first action that needs
a decision aborts with exit code 4. `accept-edits` grants file edits but still
fails closed (exit 4) for a command that needs approval. `auto` and `bypass`
grant the policy's asks for the turn; the policy engine and hardline floor still
apply before that point, so a denied action is never allowed by the mode.

## Artifacts

With `--output-dir`:

| File | Contents |
| --- | --- |
| `events.jsonl` | Redacted harness events, one JSON object per line |
| `summary.json` | Status, exit code, conversation id, usage, cost, changed paths, spans |
| `trace.jsonl` | Derived spans (same format as `:kateai trace`) |
| `patch.diff` | `git diff` plus untracked-file diffs when the workspace is a git tree |

Without `--output-dir`, the run still streams text and optional JSONL events.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | The turn completed |
| `1` | Usage or setup error (bad option, missing workspace, provider error before a turn) |
| `2` | The turn failed or verification did not pass |
| `3` | A model-turn or cost budget was exhausted |
| `4` | A permission decision was required in `ask` mode |
| `5` | The agent asked a question with no `--answer` |

## Replay

```sh
kateai-headless replay --conversation <id> [--session-db <path>] [--jsonl]
```

Replay reads the event log without calling a model, validates call/result
pairing and prints a JSON report. Exit `0` means the recorded turn is
consistent.

## ACP

```sh
kateai-headless acp --stdio \
  --workspace ~/src/project \
  --provider openai-compatible --base-url http://127.0.0.1:8080/v1 \
  --model local --api-key local
```

See `docs/ACP_SERVER.md`. In ACP mode the server owns the conversation and the
host answers permission/question requests.

## Security notes

- The sandbox, deny globs, hardline floor and policy engine are the same as the
  plugin's.
- Credentials passed with `--api-key` are held in memory for the process.
- The session database and event log are local files; point `--session-db` at
  a disposable path in CI.
- `patch.diff` can contain source changes; treat the output directory as
  sensitive.

## Limits

- No editor providers: LSP, debugger and ecosystem tools report themselves as
  unavailable. File, shell, graph, web and sub-agent tools still use the same
  `ToolRunner`.
- Workspace MCP servers are not auto-connected in a non-interactive process,
  because starting a project-supplied command requires the trust card. Use the
  plugin or an explicit future `--mcp` opt-in for that.
- The CLI is one prompt per process; use the ACP mode for a long-lived session.
