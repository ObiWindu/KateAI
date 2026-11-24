# KateAI 0.8.0

0.8.0 is the context/tools release. It rebuilds the prompt, context engine,
memory hierarchy and tool registry so long sessions stay coherent and a large
MCP catalog no longer consumes the window. It also adds native Gemini prompt
caching, preserves the full transcript when a conversation is reopened, and
lets a failed edit recover from smart-quote, CRLF and whitespace drift.

## Prompt assembler v2

- **Deterministic sections.** `PromptAssembler` renders an ordered section
  registry (`identity`, mode, execution, model guidance, reasoning,
  verification, self-critique, style, environment, plan mode, rules, memory,
  skills, project graph and plan). Sections sort by `(order, id)`, never by
  insertion order, so the same inputs produce the same bytes.
- **Stable static prefix.** Identity, execution, model guidance, verification
  and self-critique are static for the life of the conversation. Editor state,
  git status, workspace listings and reminders move to a transient
  turn-context message. `:kateai prompt` reports the SHA-1 of the static
  prefix; changing session content does not change it.
- **Explicit budgets.** Each section has a token budget and truncates with a
  visible marker. The total prompt is capped at 25% of the model window
  (4096-token floor) and sheds the largest non-core sections first. Tool-schema
  cost is reserved before any section is rendered, and truncation uses the same
  token estimator as the context engine, so non-ASCII text cannot exceed it.
- **One verification contract.** When the verification gate is on, the
  verification prose appears once; the legacy duplicate block is removed from
  the identity section.

## Model profiles and caching

- **Family registry.** Provider and model ids resolve to a `ModelProfile` with
  context window, output limit, prices, cache style, prompt variant, edit
  dialect, schema dialect and capability flags. Known model families inherit
  sane values across point releases; unknown models get a conservative 32k
  window, exact editing and no cache assumptions.
- **Provider-aware limits.** `max_tokens` is capped at the profile's real output
  limit, and OpenAI-compatible providers that support streaming usage are
  asked for it. Anthropic requests add `cache_control` breakpoints after the
  static system prompt and the last tool schema.
- **Native Gemini transport.** Gemini models configured through the
  OpenAI-compatible endpoint at `generativelanguage.googleapis.com` switch to
  the native API. Kate creates a `cachedContents` resource for the system
  instruction, tool declarations and first exchange, keyed by model, prompt and
  tools. Later requests send only the suffix with `cachedContent`; expired or
  changed caches are deleted and rebuilt. Text, thought, function-call and usage
  events flow through the existing stream handler.

## Context engine v2

`ContextEngine` compacts only when needed and stops at the first shaper that
fits:

1. **Tool-result budget** — output over the per-tool cap goes to the content
   store and is replaced by a head/tail preview with a `tool_result` id.
2. **Prune old output** — output outside the recent window becomes one line;
   failures and paged previews are preserved.
3. **Snip old turns** — the oldest span is dropped at a safe message boundary,
   keeping the first request and the live user turn represented.
4. **Incremental summary** — dropped spans merge into the previous summary with
   head/tail retention, so a second compaction still recalls an early decision.
5. **Full compaction** — the legacy `ContextManager::build` remains the last
   resort.

Rehydration adds one transient message with the current plan, up to five
touched files, running sub-agent tasks and optional root instructions or
invoked skills; it is capped to the room left in the context budget and is
never persisted. Provider-reported usage calibrates the local estimate through
a per-model factor clamped to 0.5–2.0, and `:kateai usage` shows the factor and
sample count. Summaries keep content-store ids so `read_tool_result` still
works after compaction. The content store prunes oldest-first at 200 MiB.

## Memory hierarchy v2

- **Five scopes.** Managed (`KATEAI_MANAGED_MEMORY` or
  `/etc/kateai/AGENTS.md`), user (`~/.config/kateai/`, `~/.claude/`), project
  (`AGENTS.override.md`, `AGENTS.md`, `CLAUDE.md`, `KATEAI.md`, Cline/Kilo
  rules, `.kateai/rules`, `.claude/rules`, `.agents/rules`), nested ancestor
  rules and local `*.local.md` files load in precedence order.
- **Path-aware rules.** Frontmatter supports `paths`, `priority`,
  `alwaysApply` and `description`. A `paths:` file loads only when a touched
  path matches; a nested `AGENTS.md` without `paths:` applies to its subtree.
  Mode-named rule files are restricted to that mode.
- **Deduplication and budgets.** Files are deduplicated by canonical path and
  content hash, so a symlinked `CLAUDE.md` loads once. Budgets are 32 KiB per
  file, 128 KiB total and 32 files, measured in UTF-8 bytes and visible in the
  provenance. Globstar matches zero directories, so `**/*.cpp` includes root
  files.
- **Safe writes.** `memory_write` writes only to `.kateai/memory/<topic>.md` or
  the user memory directory. The topic is validated as a slug, project writes
  stay inside the workspace, user writes obey read-only and deny rules, and the
  normal Write card shows the diff before approval.

## Tool registry v2

- **Declarative tools.** Every built-in tool has a `ToolSpec` with title,
  description, group, risk, concurrency safety, idempotence, prior-read
  requirement, always-loaded/deferred flags, result budget, schema and aliases.
  Schemas are byte-identical to `toolDefinitions()`.
- **Deferral.** When more than 15 tools are accessible, or the schema cost
  exceeds 10% of the window, only the core set plus three bridge tools are
  advertised: `tool_search` (BM25 over name, title, description and aliases),
  `tool_describe` (full schema) and `tool_call` (invoke with normal permission,
  access, hook and result-budget handling). MCP tools defer by default when
  `toolSearch` is on.
- **Deferral is not a wall.** Direct calls to deferred tools still execute, and
  `tool_call` is unwrapped before hooks and policy so the target's risk is the
  one enforced. Discovered tools stay advertised for the conversation.
- **Per-tool result budgets** replace the single 40K threshold. Output over the
  budget is stored with the budget's head/tail split.

## Edit canonicalisation

`edit_file` still tries the exact unique match first. Only when that fails does
a second pass fold smart quotes and dashes to ASCII, read CRLF as LF, strip
trailing whitespace and collapse runs of spaces/tabs. A character map applies
the replacement at the original offsets, so the rest of the file keeps its
bytes. Ambiguous canonical matches still fail; `multi_edit_file` and
`apply_patch` keep their exact-match and atomic contracts.

## Transcripts and ACP

- **Reopened conversations keep their whole history.** The display transcript
  is stored separately from the model history, so context compaction no longer
  shrinks what a reopened chat shows. The rebuild iterates a snapshot and
  prunes the display copy while leaving the model history intact. Older
  payloads without a transcript fall back to their stored messages.
- **ACP prompts lead with the user's message.** Editor context and the
  once-per-session rules/client-method notes follow the message and carry an
  assistant-only ACP audience annotation, so agents no longer answer the
  editor context while ignoring the request.

## Reports and flags

- `:kateai prompt` dumps the exact system prompt, variant, static hash and
  per-section token manifest.
- `:kateai memory` lists loaded files, scopes, bytes, hashes, truncation and
  path globs.
- `:kateai tools` shows the effective catalog, advertised/deferred state,
  schema cost, risk and result budget.
- `:kateai context` adds prompt sections, context shapers, summary size and the
  calibration factor; `:kateai usage` adds the model profile, estimated input
  cost and calibration samples.

All four 0.8 flags default on (`promptV2`, `contextV2`, `memoryV2`,
`toolSearch`) and degrade independently to the legacy path:

```sh
KATEAI_HARNESS_FLAGS=-promptV2,-contextV2,-memoryV2,-toolSearch kate
```

The `[Harness]` config group accepts the same names.

## Release engineering

The Windows plugin CI job now installs only KTextEditor and the KF6 packages
the plugin links against. `craft kate` also builds Kate, Konsole and
Markdownpart from source because KDE's Windows binary cache does not contain
them, which made the Windows job sit for hours. The release bundle still passes
`-WithKate`. Craft runs in CI mode so a retry prompt cannot block on stdin, and
superseded PR runs are cancelled.

## Tests

New suites cover model profiles, prompt ordering and budgets, the static-prefix
hash, context shapers and rehydration, memory scopes and budgets, registry
deferral and bridge calls, `memory_write`, edit canonicalisation, transcript
pruning and history restore, incremental project-graph imports, and the
Gemini/Anthropic stream and usage shapes. `ctest` is green on Linux.
