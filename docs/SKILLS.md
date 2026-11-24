# Skills

Kate AI skills are `SKILL.md` bundles: a short description that is always
visible to the model, and a procedure that is loaded only when it is needed.
They are the 0.9 progressive-disclosure mechanism for repeated team work such
as reviewing a diff, diagnosing a failing test, or preparing a release.

## Discovery

Skills are discovered, in precedence order, from:

1. managed: `KATEAI_MANAGED_SKILLS` or `/etc/kateai/skills`;
2. user: `$XDG_CONFIG_HOME/kateai/skills` and `$XDG_CONFIG_HOME/.claude/skills`;
3. project: `<workspace>/.kateai/skills`, `.agents/skills`, `.claude/skills`;
4. bundled first-party skills: `diagnose-test-failure`, `review-diff`,
   `prepare-release`.

The first skill with an id wins. An untrusted project skill cannot hide a
trusted bundled skill with the same id.

A skill directory must contain `SKILL.md`. Supporting files one level deep are
listed for the model but never loaded automatically; scripts are listed, not
executed.

## Frontmatter

```markdown
---
name: diagnose-test-failure
description: Diagnose a failing test and prove the fix.
paths: ["tests/**", "src/**"]
allowed-tools: [read_file, grep, bash, edit_file]
disable-model-invocation: false
context: inline
model: inherit
effort: low
version: 1.0.0
license: MIT
---
Reproduce the failure, form one hypothesis, make the smallest fix, and re-run
the narrow command before the suite.
```

- `name` and `description` are required. The id is the slugified name.
- `paths:` globs use the same rules as memory rules. A path-scoped skill is
  absent from the catalog until a tool touches a matching path. Explicit user
  invocation is still available.
- `disable-model-invocation: true` hides the skill from the model catalog; it
  can still be invoked by the user.
- `context: fork` runs the body in an isolated child context and returns only
  its summary. `context: inline` (the default) returns the body as a tool
  result in the current turn.
- `allowed-tools` is recorded and shown; the active mode, AgentSpec tool lists,
  sandbox, policy, and permissions still apply.
- Unknown keys are ignored, so a file can carry tool-specific metadata.

## Catalog and invocation

The prompt receives only a catalog of `- id: description` lines, capped at
1500 characters. The body is loaded by:

- the model: `skill(name)`;
- the user: `:kateai skill <name>` or `/<name>` in the composer;
- the report: `:kateai skills`.

An inline body enters the conversation as a tool result. A fork body runs as a
sub-agent and returns a summary; the same `subagent_transcript` tool can audit
its sidechain.

## Trust

Bundled and managed skills are trusted. User skills are trusted after a scan
unless the scan finds a critical problem. Project skills always require
explicit approval, keyed by a hash of the skill body and supporting files:

```sh
:kateai trust-skill diagnose-test-failure
```

A changed file has a different hash and becomes untrusted again. The scan
reports:

- prompt-injection phrases;
- path traversal;
- network commands;
- executable/script files in the skill directory;
- supporting files that resolve outside the skill directory.

An untrusted project skill is refused by the `skill` tool with the trust
command in the error, so the model cannot acquire its instructions by naming
it.

## Rollback

The `skills` harness flag defaults on. Turn it off to remove the catalog, the
`skill` tool, and skill rehydration:

```sh
KATEAI_HARNESS_FLAGS=-skills kate
```

Discovery still runs so a later enable does not require a restart, but no
catalog section, tool schema, or rehydration reaches the model when the flag is
off.
