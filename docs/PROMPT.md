# Prompt assembly

Kate AI builds the system prompt with `PromptAssembler`
(`src/harness/prompt.{h,cpp}`) when `promptV2` is on. The legacy
`AgentLoop::systemPrompt()` remains as the rollback path.

## Sections

| Order | Id | Stability | Default budget (tokens) |
| --- | --- | --- | --- |
| 10 | `identity` | static | 6000 |
| 20 | `mode` | session | 2000 |
| 30 | `execution` | static | 2000 |
| 40 | `model-guidance` | static | 800 |
| 50 | `reasoning` | session | 2000 |
| 60 | `verification` | static | 800 |
| 70 | `self-critique` | static | 400 |
| 80 | `style` | session | 2000 |
| 90 | `environment` | session | 300 |
| 95 | `plan-mode` | session | 500 |
| 100 | `rules` | session | 16000 |
| 110 | `memory` | session | 12000 |
| 120 | `skills` | session | 2000 |
| 130 | `project-graph` | session | 6000 |
| 140 | `plan` | session | 6000 |

Sections are sorted by `(order, id)`. Optional empty sections are omitted.
There are no timestamps, unordered containers, or locale-formatted numbers in
the rendered text.

## Static prefix and caching

`identity`, `execution`, `model-guidance`, `verification`, and
`self-critique` are static: they do not change while the conversation lives.
Editor context, git status, workspace listings, and reminders are a transient
user message inserted after the system prompt (`turnContextMessage()`), never
part of the system prompt.

`PromptResult::staticHash` is the SHA-1 of the rendered static sections.
`:kateai prompt` reports it; `:kateai context` records it. A test asserts that
changing session sections leaves the hash unchanged.

For Anthropic-shaped transports, `LlmClient` adds one `cache_control:
ephemeral` breakpoint after the static system prompt and one on the last tool
schema. Other providers get a byte-stable prefix and their automatic cache.

## Budgets

- Per-section budget: truncate the head and append a visible
  `[… section truncated: ~N tokens omitted …]` marker.
- Total budget: 25% of the context window, with a 4096-token floor that never
  exceeds the window itself. While the total is over, the largest non-core
  section is shrunk first; `identity`, `mode`, and `execution` are core. If
  only core remains, the largest core section is shrunk too, and sections that
  cannot shrink below their marker are dropped.
- Tool schema tokens are reserved from the same allowance before sections are
  rendered. Truncation uses the same token estimator as the context engine, so
  non-ASCII content cannot exceed its budget. The manifest records both the
  total budget and the section budget.

## Profiles

`ModelProfiles::variantFor()` selects `local`, `anthropic`, `openai`,
`deepseek`, `gemini`, or `grok`. The variant adds a short guidance section
(edit dialect, plan style) and may omit plan prose. It does not change the
security or verification contract.

## Verification gate

When `verificationGate` is on, the verification section is one short contract:
the harness runs the discovered checks and failing output is the next task.
The legacy "VERIFICATION REQUIREMENT" and reasoning duplication is stripped
from the identity section so the model does not receive two conflicting
instructions.

## Commands

- `:kateai prompt` dumps the exact system prompt and manifest. It never
  includes conversation messages or credentials.
- `:kateai context` shows the prompt-section token table, the static hash, the
  context-engine shapers, summary size, and calibration factor.
