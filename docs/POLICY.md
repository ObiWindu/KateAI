# Policy

Kate AI 0.7 evaluates tool calls against declarative, deny-first policy rules
before the legacy permission mode. The hardline floor is immutable: it also
applies in **Always approve**.

## Files

Loaded in this order (all optional):

| Scope | Path |
| --- | --- |
| Managed | `/etc/kateai/policy.json` |
| Project | `<workspace>/.kateai/policy.json` |
| User | `~/.config/kateai/policy.json` |

A missing file is ignored. A malformed file produces a warning and its rules
are ignored; it never bricks the agent. The effective rules and warnings are
shown by `:kateai policy`.

## Schema

```json
{
  "rules": [
    {
      "id": "protect-migrations",
      "reason": "Migrations require a human author.",
      "match": {
        "tool": "edit_file|multi_edit_file|apply_patch",
        "path": "migrations/**"
      },
      "decision": "deny"
    },
    {
      "id": "allow-git-status",
      "match": { "tool": "bash", "commandPrefix": "git status" },
      "decision": "allow"
    },
    {
      "id": "ask-force-push",
      "match": {
        "tool": "bash",
        "commandPrefix": ["git push"],
        "commandRegex": "--force"
      },
      "decision": "ask"
    }
  ]
}
```

`decision` is `allow`, `ask`, `deny` or `review` (`review` currently prompts,
because no reviewer model is configured yet).

`match` fields:

| Field | Meaning |
| --- | --- |
| `tool` | Exact tool name, `*` wildcard, or a `\|`-separated list |
| `path` | Glob matched against `path`, `paths`, `files` and command operands |
| `commandPrefix` | String or array; matched from the start of a `bash` command |
| `commandRegex` | Regular expression over the raw `bash` command |
| `risk` | `read`, `write` or `execute` |
| `mcpServer` | MCP server segment of `mcp__server__tool` |
| `mcpTool` | MCP tool segment of `mcp__server__tool` |

Paths are resolved against the workspace and symlinks are followed before
matching, so `src/link/secret.txt` cannot sidestep a rule for
`src/real/secret.txt`.

## Evaluation order

1. Immutable hardline
2. A `deny` in any layer
3. An `ask`/`review` rule
4. An `allow` rule
5. The legacy permission mode

A deny anywhere beats an allow anywhere. A policy `allow` can skip the legacy
"ask" step; it cannot skip a hardline deny or the sandbox.

## Hardline

These are always denied, including in **Always approve**:

- `rm -rf /`, `mkfs`, `dd of=/dev/…`, fork bombs, shutdown/reboot/poweroff.
- Force-pushing protected `main` / `master` / `release/*` refs.
- Writes or commands naming `.env`, `.env.*`, private keys, `/etc/shadow`,
  `/etc/sudoers`, `.ssh`, `.gnupg`, AWS credentials, Kubernetes and Docker
  config.

The hardline list is code, not configuration, so a project policy cannot
weaken it.

## Audit

Every permission decision is appended to the conversation event log and to
`~/.local/share/kateai/audit.jsonl`. Credentials are redacted before either
write. `:kateai trace` exports the event log as JSONL for inspection.
