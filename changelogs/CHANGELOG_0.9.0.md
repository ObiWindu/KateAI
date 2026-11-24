# KateAI 0.9.0

0.9.0 is the extensibility/agents release. Skills package repeated procedures
as `SKILL.md` bundles with progressive disclosure and trust. Sub-agents get
declarative specifications, sidechain transcripts, summary-only returns,
forwarded questions, cost attribution, and optional git-worktree isolation.
A read-only Harness inspector makes the prompt, context, tools, memory, hooks,
trace, cost, and checkpoints visible without changing a turn.

## Skills v1

- Discovery reads project `.kateai/skills`, `.agents/skills`, and
  `.claude/skills`, user `$XDG_CONFIG_HOME/kateai/skills` and `.claude/skills`,
  managed `KATEAI_MANAGED_SKILLS`/`/etc/kateai/skills`, then bundled skills.
- Frontmatter supports `name`, `description`, `paths`, `allowed-tools`,
  `disable-model-invocation`, `context: inline|fork`, `model`, `effort`,
  `version`, and `license`.
- The prompt contains only a name/description catalog capped at 1500
  characters. The body loads through the `skill` tool, `:kateai skill <name>`,
  or `/<name>`.
- Path-scoped skills activate only when a touched path matches. User-only
  skills are hidden from the model catalog but remain user-invocable.
- The trust scan reports prompt-injection phrases, path traversal, network
  commands, executables, and files that resolve outside the skill directory.
  Project skills require `:kateai trust-skill <name>`; approval is tied to a
  hash of the body and supporting-file contents, so an edit revokes it.
- An untrusted project skill cannot shadow a bundled skill with the same id.
- `context: fork` runs the body in an isolated child loop and returns a
  summary.
- Three first-party skills ship in the binary: `diagnose-test-failure`,
  `review-diff`, and `prepare-release`.

## Subagents v2

- `AgentProfile`/AgentSpec adds `model`, `effort`, `permissionMode`, `tools`,
  `disallowedTools`, `skills`, `maxTurns`, `isolation`, and `memory`. Markdown
  frontmatter and the JSON roster round-trip the fields, and the roster text
  shown to the orchestrator includes the policy.
- The child applies the spec without widening the mode: tool allow/deny lists
  intersect the mode access, skill allow-lists filter the catalog and tool,
  and `maxTurns` maps to the model-request budget.
- Each run has a `SubagentRecord` (status, summary, error, isolation, patch,
  files changed, timestamps, token usage, cost, applied/discarded) and a
  redacted JSONL sidechain under
  `$XDG_DATA_HOME/kateai/subagents/<conversation>/<task>.jsonl`.
- Results stay summary-only by default. The parent gets a compact
  `[subagent taskId=… status=…]` footer and can page the sidechain with the new
  read-only `subagent_transcript` tool. `include_transcript` remains available.
- `subagent/start` and `subagent/end` events join the parent log.
- `:kateai usage` and the inspector show per-agent input/output/cached tokens
  and attributed cost.
- With `agentsV2`, a child question becomes a normal intent-dock question and
  its answer routes to the parked child. A child permission request is
  forwarded with a prefixed card id and resolved on the child. A completed or
  cancelled child clears its forwarded cards and mappings.
- `isolation: worktree` creates a detached git worktree in the cache
  directory. The child's sandbox works there while project modes, agents,
  rules, memory, and skills still come from the real project. On finish the
  patch and changed-file list are captured and nothing is merged. The card
  offers **Apply patch** / **Discard**; `:kateai apply-subtask <id>` and
  `discard-subtask <id>` do the same. Applying takes a safety checkpoint first
  when checkpoints are enabled. A worktree preflight failure falls back to
  in-process with a notice.
- Cancellation and parent abort release locks, remove pending worktrees, mark
  the record cancelled, and remove forwarded approval mappings.

## Inspector

- `:kateai inspector` opens a read-only seven-tab panel: Context, Tools,
  Rules & Memory, Hooks & Skills, Trace, Cost, and Rewind.
- It reuses `contextReport`, `promptReport`, `toolsReport`, `memoryReport`,
  `hooksReport`, `skillsReport`, `subagentsReport`, `traceReport`,
  `usageReport`, and the checkpoint list.
- Trace includes replay problems; Cost includes actual provider usage and
  sub-agent attribution; Rewind restores files only and states that
  conversation rewind is not in this release. `:kateai fork` copies the
  current conversation into a new active thread with a parent link.
- Refresh only re-reads state. The panel never runs checks, restores without
  an explicit click, or trusts a skill.

## Edit diagnostics and config

- A failed exact and canonical `edit_file` match now reports the closest
  matching block with line numbers.
- Configuration schema is v2. A v1 config is read as-is and rewritten with
  version 2 on the next save; no key moved.
- `skills`, `agentsV2`, and `inspector` default on. `telemetry` remains
  opt-in.

## Release engineering

- Version metadata updated to 0.9.0 across CMake, plugin metadata, README,
  ACP client identity, and packaging.
- New docs: `docs/HARNESS_0.9.md` and `docs/SKILLS.md`.
- Built-in tool count grew by `skill` and `subagent_transcript`; read-group
  parity tests were updated.

## Tests

New coverage: `test_harness_skills`, `test_skilltool`, `test_worktree`, and
`test_inspector`; AgentSpec parsing/round-trip in `test_multiagent`; sidechain
and structured-record assertions in `test_subtaskparallel`; config v1→v2
migration. The full suite is green on Linux.
