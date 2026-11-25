# Telemetry and cost

Kate AI records usage locally first. A turn's trace is derived from the
conversation's append-only event log; cost is derived from the provider's
reported token counts and the model profile. Nothing is sampled, and no
content leaves the machine unless OTLP export is enabled explicitly.

## Cost ledger

`CostUsage` carries `inputTokens`, `outputTokens`, `cachedInputTokens` and
`cacheWriteTokens`. `CostBreakdown::compute()` applies the model's per-million
prices:

```
inputUsd      = inputTokens       * inputPrice      / 1e6
outputUsd     = outputTokens      * outputPrice     / 1e6
cacheReadUsd  = cachedInputTokens * cacheReadPrice  / 1e6
cacheWriteUsd = cacheWriteTokens  * cacheWritePrice / 1e6
```

If the model profile has no prices, `priced` is false and the report says
`price unknown`; it does not invent a number. The ledger keys entries by
orchestration role (planner/executor) and records sub-agent usage separately.

The per-conversation budget (`CostBudgetUsd`, default `0`) is a soft budget:
crossing it emits one `notice` event and a visible notice, and the turn
continues. It never truncates a turn or silently changes permissions.

## Trace spans

`Telemetry::fromEvents()` builds a deterministic tree:

| Span | Parent | Attributes |
| --- | --- | --- |
| `turn` | — | mode, permission mode, sandbox, status, request/tool counts |
| `step` | turn | step number, role, provider, model |
| `request` | step | message count, estimated tokens |
| `chat` | step | text/tool-call counts |
| `tool/<name>` | step | tool call id, ok/cancelled, argument/output sizes |
| `permission` | step | tool, decision, rule id, reason |
| `policy` / `hook` | step | decision and rule |
| `verification` | step | passed, attempt, command count |
| `compaction` | step | before/after tokens |
| `subagent` | turn | task id, agent, isolation, status |

Span ids are sequence numbers, so replaying the same log produces the same
trace. The event payload is the source; span attributes intentionally contain
sizes and statuses, not message bodies or tool output.

`:kateai trace` prints this waterfall and exports:

- `kateai-trace-<conversation>.jsonl` — the raw event log;
- `kateai-spans-<conversation>.jsonl` — the derived spans.

## OTLP export

Enable it in Settings → Agent & Context:

- **OTLP export** — the `telemetry` harness flag must also be on;
- **OTLP endpoint** — for example `http://127.0.0.1:4318`;
- **Content capture** — off by default; when enabled, spans include truncated
  assistant text, user text, tool arguments and tool output for a collector
  that explicitly wants content.

The exporter posts to `<endpoint>/v1/traces` with
`Content-Type: application/json`, retries transient failures three times, and
emits `exportFinished`/a notice on failure. With export enabled, the finished
turn's spans are posted automatically; `:kateai trace` also posts the current
trace when it exports. A collector outage never changes a turn's outcome, and
the endpoint must be `http(s)`.

The payload follows the OTLP JSON shape:

```json
{
  "resourceSpans": [{
    "resource": {
      "attributes": [
        {"key": "service.name", "value": {"stringValue": "kateai"}},
        {"key": "service.version", "value": {"stringValue": "1.0.0"}}
      ]
    },
    "scopeSpans": [{
      "scope": {"name": "kateai.harness", "version": "1.0.0"},
      "spans": [{
        "traceId": "…32 hex…",
        "spanId": "…16 hex…",
        "name": "turn",
        "kind": 1,
        "startTimeUnixNano": "…",
        "endTimeUnixNano": "…",
        "attributes": []
      }]
    }]
  }]
}
```

## Privacy

- The event log is redacted before it is written (credentials, bearer tokens,
  provider key prefixes, secret environment assignments).
- Span attributes contain sizes and statuses by default; message/tool content is
  included only when `TelemetryContentCapture` is explicitly enabled.
- OTLP is off by default and has no remote endpoint default.
- Cost reports contain token counts and prices, not content.
