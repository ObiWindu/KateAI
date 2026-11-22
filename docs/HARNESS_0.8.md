# Harness 0.8: prompt, context, memory, tools

Kate AI 0.8 makes long sessions and large tool catalogs viable. It implements
EP-7 to EP-10 and the prompt/request half of EP-14 from
`docs/HARNESS_IMPLEMENTATION_PLAN.md`.

## What ships

| Feature | Behaviour | Flag |
| --- | --- | --- |
| Prompt assembler v2 | Stable, ordered, budgeted sections; explicit static prefix; no timestamps or editor state in the system prompt | `promptV2` (on) |
| Model profiles | Family registry for windows, output limits, prices, cache semantics, prompt variant and edit dialect | always on |
| Context engine v2 | Tool-result paging, old-output pruning, old-turn snipping, incremental summary merge, rehydration, token calibration | `contextV2` (on) |
| Memory hierarchy v2 | Managed/user/project/nested/local scopes, `paths:` gating, dedup, budgets, provenance, `memory_write` | `memoryV2` (on) |
| Tool registry v2 | Declarative `ToolSpec`s, per-tool result budgets, `tool_search`/`tool_describe`/`tool_call`, MCP deferral | `toolSearch` (on) |
| Edit canonicalisation | Exact match first; on failure fold smart quotes, CRLF, trailing whitespace and whitespace runs, with ambiguous matches still rejected | always on |
| New reports | `:kateai prompt`, `:kateai memory`, `:kateai tools` | — |

All four 0.8 flags default on and can be disabled independently. The legacy
paths remain:

```sh
KATEAI_HARNESS_FLAGS=-promptV2,-contextV2,-memoryV2,-toolSearch
```

The `[Harness]` KConfig group accepts the same names.

## Prompt assembler

`src/harness/prompt.{h,cpp}` renders a `PromptRequest` into ordered sections.
Each section has an id, order, stability (`static`, `session`, `turn`), budget,
and content. Ordering is by `(order, id)`, never insertion order, so the same
inputs always produce the same bytes.

Section order: identity, active mode, execution protocol, model guidance,
thinking/planning, verification, self-critique, style, environment, plan mode,
project rules, memory, skills catalog, project graph, plan.

- The legacy duplicate reasoning/verification blocks are removed from the
  identity section; the assembler owns them once.
- Per-section budgets truncate with a visible marker. A total cap of 25% of the
  model window (floor 4096 tokens) sheds the largest non-core sections first.
- The static prefix hash is exposed by `:kateai prompt` and recorded in
  `:kateai context`. Changing editor state, git status, or rules changes only
  session/turn content, never the static hash.
- Verification prose is consolidated when the verification gate is on.

## Model profiles

`src/harness/models.{h,cpp}` resolves a provider and model id to a
`ModelProfile`: context window, max output, prices, cache style, prompt
variant, edit dialect, schema dialect, parallel-call and reasoning support.
The catalogue is family-based (`anthropic`, `openai`, `deepseek`, `gemini`,
`grok`, `qwen`, `glm`, `kimi`, `mistral`, `cohere`, `local`) so a new point
release of a known family inherits sane values. Unknown models get the
conservative 32k window, exact editing, and no cache assumptions. Profiles are
resolved by stable id rather than catalogue position.

The context manager's window table delegates to this registry, including the
provider-aware overload. Anthropic requests put a `cache_control` breakpoint
after the static system prompt and the last tool schema; `max_tokens` is capped
by the profile. OpenAI-compatible providers that support streaming usage are
asked for it, and Anthropic cache reads are included when calibrating the local
token estimate.

## Context engine

`src/harness/contextengine.{h,cpp}` compacts only when needed and stops at the
first shaper that fits:

1. **Tool-result budget** — output over the per-tool cap is written to the
   content store and replaced by a head/tail preview carrying a
   `tool_result id`.
2. **Prune old tool output** — output outside the recent window becomes one
   line. Failures and paged previews are preserved.
3. **Snip old turns** — the oldest span is dropped at a safe message boundary;
   the first request and the live user turn stay represented.
4. **Incremental summary** — the dropped span is merged into the previous
   summary with head/tail retention, so a second compaction still recalls an
   early decision.
5. **Full compaction** — the legacy `ContextManager::build` is the last
   resort.

Rehydration then adds a transient user message with the current plan, up to
five touched files, running sub-agent task descriptions, and optional root
instructions or invoked skills when the caller supplies them. Root instructions
normally remain in the system prompt; rehydration never duplicates them
implicitly. The message is capped to the room left in the context budget, not
just to a fixed character count, and nothing in it is persisted.

Provider-reported usage calibrates the local estimate through a per-model
factor clamped to 0.5–2.0. `:kateai usage` shows the factor and sample count.
When a summary drops a paged tool result, its id is listed in the summary so
`read_tool_result` still works.

The content store prunes oldest-first at a 200 MiB cap.

## Memory hierarchy

`src/harness/memory.{h,cpp}` loads, in precedence order:

1. managed: `KATEAI_MANAGED_MEMORY` or `/etc/kateai/AGENTS.md`;
2. user: `$XDG_CONFIG_HOME/kateai/AGENTS.md`, `~/.config/kateai/memory/`,
   `~/.claude/CLAUDE.md`;
3. project root: `AGENTS.override.md`, `AGENTS.md`, `CLAUDE.md`, `KATEAI.md`,
   `.clinerules`, `.kilocoderules`, `.kateai/rules`, `.claude/rules`,
   `.agents/rules`, `.kateai/memory`;
4. nested: the nearest ancestor directories of a path a tool touched;
5. local: `*.local.md` files.

Frontmatter supports `paths`, `priority`, `alwaysApply`, and `description`.
A file with `paths:` is lazy and loads only when a touched path matches; a
nested `AGENTS.md` without `paths:` applies to its subtree. Files are
deduplicated by canonical path and content hash, so a `CLAUDE.md` symlink or
copy of `AGENTS.md` loads once. Rule files named after a mode are restricted to
that mode; the active mode wins over the generic rules in the same scope.
`.kateai`, `.claude`, and `.agents` files directly under the workspace are
project-scoped, not nested.

Budgets are 32 KiB per file, 128 KiB total, and 32 files. Truncation is
measured in UTF-8 bytes and is visible in the rendered provenance. Globstar
patterns (`**/`) match zero or more directories, so `**/*.cpp` includes a
root-level file.

`memory_write` writes only to `.kateai/memory/<topic>.md` or the user memory
directory. The topic is validated as a slug, so the model cannot traverse to
`AGENTS.md` or an arbitrary path. Project writes use the sandbox's workspace
confinement; user writes still obey read-only and deny rules. The write goes
through the document bridge when one is available, and the normal Write
permission card shows the diff and asks for approval.

## Tool registry

`src/harness/registry.{h,cpp}` gives every built-in tool a `ToolSpec`: title,
description, group, risk, concurrency safety, idempotence, prior-read
requirement, always-loaded/deferred flags, result budget, schema, and aliases.
Schemas are byte-identical to `toolDefinitions()`.

Deferral activates when more than 15 tools are accessible or the schema cost
exceeds 10% of the window. MCP tools defer by default whenever `toolSearch` is
on. The advertised set is the always-loaded core plus the three bridge tools:

- `tool_search(query)` — BM25 over name, title, description, and aliases;
- `tool_describe(name)` — full schema, and marks the tool discovered;
- `tool_call(name, arguments)` — invokes the target through its normal
  permission, access, hook, and result-budget path.

Direct calls to deferred tools still execute. `tool_call` is unwrapped before
hooks and policy so the target's risk is the one enforced. Discovered tools
stay advertised for the conversation.

Per-tool result budgets replace the single 40K threshold; output over the
budget goes to the content store with the budget's head/tail split.

## Edit canonicalisation

`edit_file` still tries the exact unique match first. Only when it fails does a
second pass compare a canonical form: smart quotes and dashes folded to ASCII,
CRLF read as LF, trailing whitespace removed, and runs of spaces/tabs collapsed.
A character map applies the replacement at the original offsets, so the rest of
the file keeps its bytes. A canonical match that is ambiguous is still an
error; the model must add context or set `replace_all`. `multi_edit_file` and
`apply_patch` keep their exact-match/atomic contracts.

## Reports
- `:kateai prompt` — exact system prompt, variant, static hash, and the
  per-section token manifest. No conversation content or credentials.
- `:kateai memory` — loaded files, scope, bytes, hash/truncation, path globs.
- `:kateai tools` — effective tools, advertised/deferred state, schema cost,
  risk, and result budget.
- `:kateai context` now includes prompt sections, the context-engine shapers,
  summary size, and the calibration factor.
- `:kateai usage` now includes the model profile, estimated input cost when
  pricing is known, and calibration samples.

## Compatibility and rollback

- No persisted conversation schema changed. The incremental summary is part of
  the normal message projection; the event log is unchanged.
- `memory_write` is a new tool name; older clients simply never call it.
- Turning a flag off restores the legacy path for that subsystem:
  `promptV2` → `systemPrompt()`, `contextV2` → `ContextManager::build`,
  `memoryV2` → `RulesLoader`, `toolSearch` → the full built-in schema list.
- The model registry replaces only the hard-coded window table; an explicit
  `contextWindow` setting still wins.

## Tests

`tests/test_harness_models`, `test_harness_prompt`, `test_harness_context`,
`test_harness_memory`, `test_harness_registry`, the `memory_write` case in
`test_tools`, and the updated `test_harness_foundation` cover the new paths.
`ctest` is green on Linux.
