# Kate AI 1.0.0

Released 2026-11-25. Phase 4 of the harness roadmap: observability, evaluation,
a headless client, and an ACP server.

## Highlights

1. Cost and traces are first-class: every provider-reported token is priced,
   and every turn has a deterministic span tree.
2. The harness is measurable: a scripted eval suite runs the real binary with
   fixtures and fails on regressions.
3. The harness is reusable: `kateai-headless run|replay|acp` drives the same
   core without the editor.

## Cost ledger

- `src/harness/cost.{h,cpp}` implements `CostUsage`, `CostBreakdown`, and
  `CostLedger`.
- Prices come from `ModelProfile`; a model without prices is reported as
  `price unknown`, never guessed.
- Cache reads and writes are separate line items.
- Entries are keyed by orchestrator role; sub-agents are recorded by task id.
- `CostBudgetUsd` is a soft per-conversation budget: one notice, no failure.
- `:kateai usage`, the inspector Cost tab, and headless `summary.json` share the
  same ledger.

## Telemetry

- `src/harness/telemetry.{h,cpp}` derives spans from `EventStore` events:
  `turn`, `step`, `request`, `chat`, `tool/<name>`, `permission`, `policy`,
  `hook`, `verification`, `compaction`, `checkpoint`, `subagent`.
- Missing results are closed at the last timestamp and marked unfinished.
- Trace and span ids are deterministic, so replays compare byte-for-byte.
- `:kateai trace` prints the waterfall and exports event and span JSONL.
- `src/harness/otlp.{h,cpp}` posts OTLP/HTTP JSON to `<endpoint>/v1/traces`,
  retries three times, and never blocks a turn. Export is off by default.
- Settings: `TelemetryEnabled`, `TelemetryEndpoint`, `TelemetryContentCapture`;
  harness flag `telemetry` (default off).

## Headless CLI

`src/headless/` adds:

- `args.{h,cpp}`: pure parsing, unknown options rejected.
- `session.{h,cpp}`: one `AgentLoop`, disk-only `DocumentBridge`, conversation
  binding, permission/question policy for non-interactive runs.
- `main.cpp`: `run`, `replay`, `acp`; JSONL event streaming; artifacts
  `events.jsonl`, `summary.json`, `trace.jsonl`, `patch.diff`.
- `acpsession.{h,cpp}`: adapts `HeadlessSession` to the ACP server.

Exit codes: 0 success, 1 usage/setup, 2 turn/verification failure, 3 budget,
4 permission required, 5 question required.

## ACP server

`src/harness/acpserver.{h,cpp}` implements ACP v1 over stdio:

- `initialize`, `authenticate`, `session/new`, `session/prompt`,
  `session/cancel`, `session/close`;
- `session/update`: message chunks, thought chunks, plans, tool calls and
  updates;
- `session/request_permission` with allow-once/allow-session/reject options;
- `_kateai/ask_user`, `_kateai/ping`, `_kateai/capabilities`;
- one in-flight prompt per session, unknown sessions rejected.

The server owns framing only; policy, sandbox, hooks, verification, and the
event log are the same objects the plugin uses.

## Evals

`tests/evals`:

- `run.py` — runner, assertions, JSON/Markdown reports;
- `scripted_provider.py` — deterministic SSE provider;
- `fixtures/python-calculator`, `fixtures/cpp-calculator`, `fixtures/empty`;
- `scenarios/smoke`: fix-with-verification, write-with-patch;
- `scenarios/injection`: write above the workspace, read `/etc/passwd`;
- `report.py` — regenerate Markdown from a report.

`harness_eval` custom target; `-DKATEAI_ENABLE_EVALS=ON` adds the smoke suite
to CTest.

## Configuration

`SettingsStore::configVersion()` is 4. v1–v3 configs are read as-is and
rewritten with the new version; the new keys are additive and the telemetry
flag is off by default. No setting is required for headless or ACP.

## Compatibility

- The plugin does not require the headless executable.
- Turning `telemetry` off disables export only; local cost and span reports
  remain.
- The event schema and conversation payload are unchanged.
- `kateai-headless` never starts from the plugin.

## Tests

New suites:

- `test_harness_cost`
- `test_harness_telemetry`
- `test_headless_args`
- `test_headless_cli`
- `test_acpserver`
- `harness_eval` (opt-in)

Existing settings tests now cover the telemetry/budget round-trip. The
verification nudge now respects `requireVerification=false`, so `--no-verify`
no longer triggers a prose-only extra turn.
