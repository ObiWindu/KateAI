# Hooks

Hooks are deterministic commands at lifecycle points. They run beside the
probabilistic prompt: the model can be asked not to touch a directory, but a
hook can stop the call.

## Files and trust

| Scope | Path | Trust |
| --- | --- | --- |
| Managed | `/etc/kateai/hooks.json` | Trusted |
| User | `~/.config/kateai/hooks.json` | Trusted |
| Project | `<workspace>/.kateai/hooks.json` | **Untrusted until approved** |

Approve a project's hooks with `:kateai trust-hooks`. The approval is stored
under a hash of the workspace path in the `[HarnessHooks]` config group.
Untrusted project hooks are listed but not executed.

`:kateai hooks` shows the configured hooks and the trust state.

## Schema

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "edit_file|multi_edit_file|apply_patch",
        "command": ["python3", ".kateai/hooks/protect_migrations.py"],
        "timeoutMs": 5000,
        "onFailure": "block"
      }
    ],
    "PostToolUse": [
      {
        "matcher": "edit_file",
        "command": ["python3", ".kateai/hooks/format_edited.py"],
        "timeoutMs": 10000
      }
    ],
    "Stop": [
      {
        "command": [".kateai/hooks/check-docs.sh"],
        "onFailure": "block"
      }
    ]
  }
}
```

- `command` is an argv array by default. A string is split with the platform
  command-line rules unless `shell: true` is set.
- `matcher` is a regular expression matched against the tool name (or the stop
  reason for `Stop`). An empty matcher matches everything.
- `onFailure` is `continue` (default) or `block` for crashes and timeouts.
- Commands run with a scrubbed environment and, when a sandbox is active,
  through the same sandbox wrapper as tools.

Supported events: `SessionStart`, `UserPromptSubmit`, `PreToolUse`,
`PostToolUse`, `PostToolUseFailure`, `Stop`, `PreCompact`, `SubagentStart`,
`SubagentStop`, `SessionEnd`. Version 0.7 wires `SessionStart`,
`UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `PostToolUseFailure` and
`Stop` into `AgentLoop`; the rest are reserved.

## Input

The hook receives one JSON object on stdin:

```json
{
  "event": "PreToolUse",
  "workspace": "/home/user/project",
  "tool": {
    "name": "edit_file",
    "arguments": { "path": "migrations/001.sql", "old_string": "…" }
  }
}
```

`PostToolUse` adds `result` (`ok`, `output`). `Stop` adds `reason`, `summary`
and `changedPaths`.

## Output and exit codes

Standard output may be a JSON object:

```json
{
  "decision": "allow",
  "reason": "checked",
  "updatedInput": { "path": "src/safe.cpp" },
  "additionalContext": "The formatter rewrote this file."
}
```

- `decision: "deny"` or `"block"` blocks the call and returns `reason`.
- `updatedInput` merges over the tool arguments before policy evaluation.
- `additionalContext` is appended to the tool result.
- Exit code `2` blocks even without JSON; stderr/stdout becomes the reason.
- Any other non-zero exit follows `onFailure`.
- A timeout kills the process and follows `onFailure`; output is capped at
  8 KiB.

A hook allow never overrides a policy deny. A hook deny always wins.

## Examples

Block edits outside a reviewable set:

```python
#!/usr/bin/env python3
import json, sys
event = json.load(sys.stdin)
path = event["tool"]["arguments"].get("path", "")
if path.startswith("migrations/"):
    print(json.dumps({"decision": "deny", "reason": "migrations need a human author"}))
    sys.exit(0)
```

Require the docs to build before the turn can finish:

```json
{
  "hooks": {
    "Stop": [
      { "command": ["make", "docs"], "onFailure": "block", "timeoutMs": 60000 }
    ]
  }
}
```
