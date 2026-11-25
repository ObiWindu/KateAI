# Evaluation harness

The eval suite measures the harness, not only the model. It runs the real
`kateai-headless` binary against a deterministic scripted provider and asserts
files, events, exit codes, tool failures, verification evidence and cost.

It has no third-party dependencies and uses no API keys.

## Run

```sh
cmake --build build --target kateai-headless
python3 tests/evals/run.py --suite all --verbose
# or
cmake --build build --target harness_eval
```

`-DKATEAI_ENABLE_EVALS=ON` adds `harness_eval` (smoke suite) to CTest.

Reports are written to `tests/evals/out/`:

- `report.json` — machine-readable results and failed assertion details;
- `report.md` — the same table for people.

Set `KATEAI_EVAL_KEEP=1` to keep the copied fixture/workspace for debugging.

## Scenario format

A scenario is JSON under `scenarios/<suite>/`:

```json
{
  "id": "python-fix-verification",
  "description": "The agent fixes a failing test and the gate proves it.",
  "fixture": "python-calculator",
  "prompt": "Fix the failing test.",
  "verify": true,
  "sandbox": "workspace",
  "permission": "auto",
  "git": false,
  "timeout_seconds": 120,
  "script": [
    {"text": "Reading.", "tool_calls": [{"name": "read_file", "arguments": {"path": "calculator.py"}}]},
    {"text": "Fixing.", "tool_calls": [{"name": "edit_file", "arguments": {"path": "calculator.py", "old_string": "a - b", "new_string": "a + b"}}]},
    {"text": "Fixed.", "usage": {"input_tokens": 1200, "output_tokens": 80}}
  ],
  "assertions": {
    "exit_code": 0,
    "files": {"calculator.py": "return a + b"},
    "events_include": ["tool/call", "tool/result", "verification"],
    "events_order": ["turn/start", "tool/call", "tool/result", "turn/end"],
    "verification_passed": true,
    "max_tool_calls": 3,
    "max_cost_usd": 0.5,
    "file_absent": [],
    "outside_absent": ["../escape.txt"],
    "tool_failed": false,
    "patch_contains": ["calculator.py"]
  }
}
```

`script` entries are consumed in order:

- `text` — assistant text, streamed in chunks;
- `tool_calls` — `{name, arguments}`; arguments may be an object or a JSON
  string;
- `usage` — provider-reported input/output/cached tokens.

When the script is exhausted the provider replies with a final message rather
than hanging, which makes a runaway agent visible as a failed assertion.

## Fixtures

- `fixtures/python-calculator` — a failing `unittest` suite and a
  `.kateai/config.json` `verify` command.
- `fixtures/cpp-calculator` — a failing CMake/CTest calculator. It is available
  for a job with a compiler; the smoke suite uses the Python fixture so it runs
  everywhere.
- `fixtures/empty` — a workspace for write/permission/injection scenarios.

The runner copies the fixture, optionally initialises a git repository
(`"git": true`), starts the scripted provider on loopback, sets a disposable
`HOME`/`XDG_*`/`KATEAI_SESSION_DB`, and runs headless with `--no-persist`.

## Suites

- **smoke** — a fix that must pass verification; a write that must produce a
  patch and events.
- **injection** — a write above the workspace, a read of `/etc/passwd`, and a
  strict-sandbox network attempt must fail without touching the host or
  leaking content.

The injection suite is the regression gate for sandbox/policy changes.

## Adding a scenario

1. Add or reuse a fixture with a deterministic verifier.
2. Write a script whose tool calls are the shortest path to the outcome.
3. Assert the user-visible result, not implementation details: file content,
   exit code, verification event, failed tool result, event order.
4. Keep `max_tool_calls` tight; a regression that makes the loop wander should
   fail the scenario.
5. Run `python3 tests/evals/run.py --scenario <path> --verbose`.

## CI

PR jobs can run the smoke suite with `-DKATEAI_ENABLE_EVALS=ON`. Nightly jobs
can add live providers by pointing `--provider`/`--base-url` at a keyed
endpoint; the default suite stays scripted so a PR never spends money or
depends on a model.
