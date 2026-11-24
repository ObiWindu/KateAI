# Harness 0.9: skills, agents, inspector

Kate AI 0.9 makes the harness extensible and parallel. It implements EP-11 to
EP-15 from `docs/HARNESS_IMPLEMENTATION_PLAN.md` and finishes the request-shaping
work that began in 0.8.

## What ships

| Feature | Behaviour | Flag |
| --- | --- | --- |
| Skills v1 | `SKILL.md` discovery, catalog-only prompt cost, on-demand body, trust scan, hash approval, `context: fork` | `skills` (on) |
| Subagents v2 | AgentSpec fields, JSONL sidechains, summary-only returns, forwarded questions/approvals, cost attribution, worktree isolation, patch Apply/Discard | `agentsV2` (on) |
| Inspector | Read-only Context, Tools, Rules & Memory, Hooks & Skills, Trace, Cost, Rewind panel | `inspector` (on) |
| Fork | `:kateai fork` copies the current conversation into a new active thread with a parent link | always on |
| Edit diagnostics | Closest-block line-number hint after a failed exact/canonical match | always on |
| Config schema v2 | v1 configs are read as-is and rewritten with version 2 | migration |

The three 0.9 flags default on and can be disabled independently:

```sh
KATEAI_HARNESS_FLAGS=-skills,-agentsV2,-inspector kate
```

The `[Harness]` KConfig group accepts the same names.

## Skills

`src/harness/skills.{h,cpp}` discovers managed, user, project, and bundled
skills. The prompt gets a name+description catalog capped at 1500 characters;
`ToolRunner::skill()` loads the body on demand and enforces trust, user-only
visibility, path gating, and an AgentSpec skill allow-list.

- Project roots: `.kateai/skills`, `.agents/skills`, `.claude/skills`.
- User roots: `$XDG_CONFIG_HOME/kateai/skills` and `.claude/skills`.
- Managed: `KATEAI_MANAGED_SKILLS` or `/etc/kateai/skills`.
- Bundled: `diagnose-test-failure`, `review-diff`, `prepare-release`.

Trust is a SHA-1 over the body and supporting-file contents, stored in
`$XDG_DATA_HOME/kateai/skill-trust.json` (override with
`KATEAI_SKILL_TRUST_FILE`). Project skills require
`:kateai trust-skill <name>`; user skills are auto-trusted after a clean scan;
a critical finding (injection, traversal, executable, escaping file) requires
approval. See `docs/SKILLS.md`.

`context: fork` is intercepted before normal tool execution and runs the body
in an isolated child loop. It returns a summary and records a sidechain, just
like `new_task`.

## Subagents v2

`AgentProfile` now carries the AgentSpec fields: `model`, `effort`,
`permissionMode`, `tools`, `disallowedTools`, `skills`, `maxTurns`,
`isolation`, and `memory`. Markdown frontmatter and the JSON roster both parse
and round-trip them. Built-in agents keep their existing ids and modes.

Each native `new_task` run:

- receives a `SubagentRecord` with status, summary, isolation, patch, timestamps,
  token usage, and cost;
- appends a JSONL sidechain under
  `$XDG_DATA_HOME/kateai/subagents/<conversation>/<task>.jsonl`;
- returns summary prose plus a compact `[subagent taskId=… status=…]` footer;
- can be audited with `subagent_transcript(task_id, offset, max_chars)`;
- emits `subagent/start` and `subagent/end` events into the parent log;
- reports per-agent tokens and cost in `:kateai usage` and the inspector.

### Question and permission forwarding

With `agentsV2` on, a child's `question` becomes a normal question card in the
parent's intent dock. The answer is routed back to the parked child. A child
permission request uses a prefixed card id and is routed through
`AgentLoop::resolvePermission` to the child. A finished or cancelled child
removes its forwarded cards and mappings.

### Worktree isolation

`isolation: worktree` (or the `new_task` `isolation` argument) creates a
detached `git worktree` under the cache directory. The child's sandbox and
working root point at the worktree; its mode, agent, rules, memory, and skills
discovery stay on the real project so a gitignored `.kateai` directory is not
lost. The parent tree is never touched.

On completion the child's staged diff and changed-file list become a patch
artifact. The parent tool result says nothing was merged. The UI offers
**Apply patch** and **Discard** on the sub-agent card; `:kateai
apply-subtask <id>` and `:kateai discard-subtask <id>` do the same. Applying
takes a safety checkpoint first when checkpoints are enabled. A worktree
preflight failure falls back to in-process with a visible notice.

## Inspector

`:kateai inspector` opens a read-only panel with seven tabs:

- **Context** — context report plus the exact prompt and section manifest.
- **Tools** — effective/deferred set, schema cost, risk, result budget.
- **Rules & Memory** — loaded files, scope, bytes, paths, truncation.
- **Hooks & Skills** — hook trust, skill discovery/trust/scan findings, and
  sub-agent records.
- **Trace** — the conversation event log with replay problems.
- **Cost** — usage, calibration, actual provider tokens, sub-agent attribution.
- **Rewind** — checkpoints and files-only restore.

It reuses the same report methods as the `:kateai` commands. Refresh and
`turnFinished` only re-read state; the panel never runs verification, changes a
turn, or trusts a skill. Conversation rewind is explicitly not part of this
release; the tab says so. `:kateai fork` copies the current conversation into a
new active thread with a parent link. `:kateai rewind [id]` and `/rewind`
restore checkpoint files, and `/fork`, `/inspector`, `/skills`, and `/compact`
are built-in composer commands.

## Edit robustness

`edit_file` keeps exact matching as the primary path and the 0.8 canonical
fallback for smart quotes, CRLF, trailing whitespace, and whitespace runs. When
both fail, the error now includes the closest matching block with line numbers
so the model can copy the real text instead of re-reading the whole file.
Ambiguous matches still fail, and `apply_patch` remains atomic and
byte-faithful.

## Configuration

`SettingsStore::configVersion()` is now 2. A v1 configuration is read as-is and
rewritten with version 2 on the next save; no key moved. The version boundary
is where future `[Harness]`/`[Skills]` key migrations belong.

## Compatibility and rollback

- The session payload version is unchanged; sidechains and the inspector are
  additive.
- `subagent_transcript` and `skill` are new tool names; older clients never
  call them.
- Turning `skills` off removes the catalog, the tool schema, and skill
  rehydration.
- Turning `agentsV2` off restores the 0.8 fail-fast child question/permission
  behavior and ignores AgentSpec application (isolation, model, effort, tools,
  skills, max turns).
- Turning `inspector` off hides the panel; the report commands still work.
- Worktrees live in the cache directory and never enter the project graph.

## Tests

`test_harness_skills`, `test_skilltool`, `test_worktree`, `test_inspector`,
the AgentSpec additions in `test_multiagent`, the sidechain/record assertions in
`test_subtaskparallel`, the config v1→v2 migration, and the updated built-in
tool counts cover the new surface. `ctest` is green on Linux.
