# Harness 1.0: telemetry, evals, headless and ACP

Kate AI 1.0 closes Phase 4 of `docs/HARNESS_ROADMAP.md` and EP-16 to EP-19 of
`docs/HARNESS_IMPLEMENTATION_PLAN.md`. The plugin and the new headless client
run the same `AgentLoop`, tools, policy engine, verification gate and event
log; nothing in this release forks the harness.

## What ships

| Feature | Behaviour | Flag / opt-in |
| --- | --- | --- |
| Cost ledger | Provider usage priced per model, cached reads/writes reported separately, per-conversation soft budget | always on; budget default off |
| Trace spans | A turn/step/tool/permission/verification waterfall derived from the event log, JSONL export | always readable; span export is local |
| OTLP export | Opt-in OTLP/HTTP JSON export with retry; a collector failure never fails a turn | `telemetry` flag + Settings `TelemetryEnabled` || Scenario evals | Scripted provider, fixture repos, injection corpus, JSON + Markdown reports | `tests/evals`, `harness_eval` target |
| `kateai-headless` | Run one prompt with JSONL events, patch/summary/trace artifacts and deterministic exit codes | executable |
| ACP server | `kateai-headless acp --stdio`, session mapping, streaming updates, permission and question round-trips | `--stdio` |
| Hardening | Config schema v4 (additive), docs, packaging version, release checks | migration |

## Cost accounting

`src/harness/cost.{h,cpp}` prices only what the provider reported:

- input, output, cache-read and cache-write tokens;
- per-model prices from `ModelProfile`;
- per-role entries (planner/executor) and per-sub-agent attribution;
- a soft per-conversation budget that raises one notice, never fails a turn;
- an explicit `price unknown` result instead of a guessed price.

`:kateai usage` and the inspector Cost tab show the ledger. The headless
`summary.json` embeds `costReportJson()`.

## Traces

`src/harness/telemetry.{h,cpp}` derives spans from the durable event log, so
the trace can never disagree with replay:

- `turn` (parent), `step`, `request`, `chat`, `tool/<name>`, `permission`,
  `policy`, `hook`, `verification`, `compaction`, `checkpoint`, `subagent`;
- durations from event timestamps; unmatched calls are closed at the last
  observed timestamp and marked unfinished;
- deterministic ids: the trace id is conversation/turn, span ids are sequence
  numbers;
- attributes are sizes, statuses, token counts and rule ids. Message bodies and
  tool output stay in the event log, which is already redacted.

`:kateai trace` now prints the waterfall and exports both the event JSONL and
the span JSONL. See `docs/TELEMETRY.md`.

## OTLP

`src/harness/otlp.{h,cpp}` maps spans to OTLP/HTTP JSON
(`resourceSpans → scopeSpans → spans`) with `service.name`/`service.version`.
Export is off by default. Enable `telemetry` and set an endpoint (for example
`http://127.0.0.1:4318`) in Settings → Agent & Context. Exports are batched per
trace request, retried three times, and only report failure through a notice.

## Evaluation harness

`tests/evals` contains:

- `run.py`: scenario runner with file/event/exit/cost/verification assertions;
- `scripted_provider.py`: deterministic OpenAI-compatible SSE provider;
- `fixtures/`: a failing Python calculator, a failing C++ CMake calculator, and
  an empty workspace;
- `scenarios/smoke` and `scenarios/injection`.

```sh
cmake --build build --target kateai-headless
cmake --build build --target harness_eval
# or a single suite:
python3 tests/evals/run.py --suite all --verbose
```

Enabling `-DKATEAI_ENABLE_EVALS=ON` adds `harness_eval` to CTest. The suite
uses no API keys and no network beyond loopback. See `docs/EVALS.md`.

## Headless CLI

`src/headless` builds `kateai-headless`:

```sh
kateai-headless run \
  --workspace ~/src/project \
  --prompt "Fix the failing test and verify it." \
  --provider openai-compatible --base-url http://127.0.0.1:8080/v1 \
  --model local --api-key local \
  --permission auto --sandbox workspace \
  --output-dir ./kateai-run
```

Artifacts: `events.jsonl`, `summary.json`, `trace.jsonl`, `patch.diff`.
Exit codes: `0` success, `1` usage/setup error, `2` turn/verification failure,
`3` budget exhausted, `4` permission required, `5` question required. See
`docs/HEADLESS.md`.

## ACP server

`kateai-headless acp --stdio` speaks ACP v1 over stdio. A host can create a
session, send a prompt, stream message/thought/tool/plan updates, answer
permission requests and questions, and cancel. Each session owns its own
conversation and event log. See `docs/ACP_SERVER.md`.

## Configuration

`SettingsStore::configVersion()` is 4. The new keys are additive:

- `[General] TelemetryEnabled`, `TelemetryEndpoint`, `TelemetryContentCapture`,
  `CostBudgetUsd`;
- the `telemetry` harness flag (default off) gates export, not accounting.

No new key is required; defaults keep the previous behaviour.

## Compatibility and rollback

- `telemetry` off: no OTLP export; cost and span reports still work locally.
- `costBudgetUsd = 0`: no budget notice.
- `kateai-headless` is an additive executable; the plugin does not depend on it.
- The ACP server is opt-in and never starts from the plugin.
- Eval fixtures and reports are outside the installed data directories.

## Tests

- `test_harness_cost`: pricing, cache prices, aggregation, budget, formatting.
- `test_harness_telemetry`: span tree, unmatched calls, JSONL, OTLP shape.
- `test_headless_args`: CLI parsing and rejection paths.
- `test_headless_cli`: real binary run, artifacts, exit 4, replay.
- `test_acpserver`: initialize, session/new, prompt, updates, permission
  round-trip, cancellation, unknown method.
- `harness_eval`: smoke and injection suites (opt-in for CTest).
