# Kate AI

<p align="center">
  <img src="docs/images/banner.jpg" alt="Kate AI — an agent panel inside the Kate text editor" width="100%">
</p>

<p align="center">
  <strong>An AI coding agent that lives inside <a href="https://kate-editor.org/">Kate</a></strong><br>
  Chat, project tools, sandboxed shell, and permission asks — Grok, OpenAI, OpenRouter, local models, and any ACP agent over stdio.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Kate-24.08%2B-1d99f3?logo=kde" alt="Kate 24.08+">
  <img src="https://img.shields.io/badge/Qt-6.5%2B-41cd52?logo=qt" alt="Qt 6.5+">
  <img src="https://img.shields.io/badge/KF-6-blue" alt="KDE Frameworks 6">
  <img src="https://img.shields.io/badge/license-LGPL--2.1--or--later-orange" alt="LGPL-2.1-or-later">
</p>

Kate AI is a [KTextEditor](https://invent.kde.org/frameworks/ktexteditor) plugin that turns Kate into an agentic coding environment. You stay in the editor you already use: the agent reads the workspace, edits open documents in place, runs sandboxed commands, and asks before anything destructive.

It is useful when you want a capable coding agent **without leaving Kate** — native Qt/KF6, per-user install, your API keys, your models (cloud or local), and a permission bar on every write.

---

## Why use it

Kate is a serious editor for C++, Qt, KDE, and systems work. Kate AI keeps that workflow intact and adds an agent that already knows the file you are looking at.

| You need | Kate AI does |
| --- | --- |
| Stay in Kate | Tool view on the right (`Ctrl+Alt+A`) in the same window. |
| Work on the current project | Reads the workspace, open tabs, cursor, and selection as context. |
| Edit with the buffer, not a fork | `read_file` / `write_file` / `edit_file` go through open Kate documents so unsaved work stays in sync. |
| Inspect before changing | **Plan mode** is read-only: the model can search and propose a plan, and cannot write or run shell. |
| See what the agent is doing | Streaming replies, collapsible reasoning, a live plan checklist, and tool cards with diffs. |
| Stay in control | Permission bar (Allow / Allow for session / Deny), sandbox profiles, deny globs for secrets. |
| Pick a model | Grok, OpenAI, OpenRouter, OpenAI-compatible (Ollama, vLLM, LocalAI), Claude-compatible, ACP agents (Grok Build, Claude Agent, Gemini CLI, …). |
| Teach the repo | Optional `KATEAI.md` at the workspace root is loaded as project instructions. |

Typical uses: explain a selection, fix a bug in the current file, refactor with a visible diff, add tests, explore an unfamiliar tree via the project graph, then apply focused patches after you approve them.

---

## Features

### Chat panel

- Tool view on the right: **View → Tool Views → Kate AI**, or **Ctrl+Alt+A**
- Multi-line prompt: **Enter** sends, **Shift+Enter** inserts a newline
- Auto-growing composer, prompt history (Up/Down on an empty first line)
- **@ mentions** for the active file, selection, workspace, open documents, and relative paths
- Markdown replies with a copy button
- Token estimate on the composer
- Stop the current turn at any time
- Scroll-to-bottom control when you leave the live stream
- Toolbar: provider, model (searchable menu), mode, permission mode, sandbox, MCP servers, checkpoints, thinking, reasoning effort, new chat, history, settings

### Modes

The agent works in one mode at a time, picked from the toolbar or the settings menu. Each mode has its own persona and its own set of tool groups, so a mode cannot quietly do something it was not given.

| Mode | Tool groups | What it is for |
| --- | --- | --- |
| **Code** | read, edit, command, mcp, web, orchestrate | The default: inspect, edit, and run commands. May delegate a self-contained piece of work to a sub-agent. |
| **Ask** | read, mcp, web | Answer questions about the code. Cannot change anything. |
| **Architect** | read, mcp, web | Design a solution and return a plan. Cannot change anything. |
| **Debug** | read, edit, command, mcp, web, orchestrate | Reproduce, form a hypothesis, then make the smallest fix. May delegate a self-contained piece of work. |
| **Orchestrator** | read, mcp, web, orchestrate | Split the work up and delegate every edit to a sub-agent. |

`orchestrate` is the group that carries `new_task`. Code and Debug get it so the agent can hand off when it judges it worthwhile; Ask and Architect do not, because they promise they cannot change anything and spawning a `coder` would break that promise. The tool schema itself is only sent to the model when the active mode actually grants it.

**Plan mode** stays orthogonal to the mode above it: it hard-restricts the model to read-only tools no matter which mode is active.

#### Custom modes

Drop a Markdown file in `<workspace>/.kateai/modes/`. The frontmatter sets the identity and the tool groups; the body is appended to the system prompt. `.kilocodemodes/` and `.roomodes/` are read too, so existing Kilo Code / Roo Code setups work unchanged.

```markdown
---
id: security-reviewer
name: Security Reviewer
description: Reviews changes for security defects
groups: [read, mcp]        # read | edit | command | mcp | orchestrate
roleDefinition: |
  You review code. You never edit it and never soften a finding.
customInstructions: |
  Cite the CWE for every issue and give a concrete fix.
---
Anything after the frontmatter counts as extra instructions.
```

Unknown tool groups are dropped rather than silently widening access, and a custom mode cannot shadow a built-in one.

### MCP servers

Kate AI speaks the [Model Context Protocol](https://modelcontextprotocol.io), so the agent can use any MCP server over **stdio** (a local process) or **Streamable HTTP**. Discovered tools appear as `mcp__<server>__<tool>` and respect the active mode's `mcp` group.

Servers are read from `<workspace>/.kateai/mcp.json` in the standard shape, and are edited under **Settings → Kate AI → MCP Servers**:

```json
{
  "mcpServers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/srv/project"],
      "alwaysAllow": ["read_file", "list_*"]
    },
    "docs": {
      "type": "http",
      "url": "https://example.com/mcp",
      "headers": { "Authorization": "Bearer …" },
      "alwaysAllow": ["*"]
    }
  }
}
```

- `alwaysAllow` auto-approves a server's tools; `*` and `prefix*` patterns are supported
- Servers that annotate a tool with `readOnlyHint` are treated as read-only and never prompt
- An MCP tool without an auto-approval always prompts, in **every** permission mode — including *Accept edits*, because an MCP tool can do anything its server offers
- Servers can be toggled per-entry from the toolbar menu; the status dot turns green when a server is ready
- A server that fails to start is reported and skipped; it never blocks a turn

### Checkpoints

Every turn snapshots the workspace into a private git repository in the cache directory **before** the agent changes anything, so any run can be reviewed or rolled back. Your own `.git` history is never touched.

- Toolbar → checkpoint button: create one manually, or pick any snapshot to diff or restore
- A restore overwrites files that changed after the snapshot and deletes files that were added afterwards; `.git` and the generated project graph are left alone
- Retention is configurable (default 20 snapshots) and old snapshots are garbage-collected
- Needs `git` on `PATH`; without it every checkpoint call becomes a no-op instead of an error

### Sub-agents and the agent team

`new_task` spawns a child agent with its own conversation and returns its answer to the parent turn. This is what makes **Orchestrator** mode work: it cannot edit files itself and must delegate.

**Parallel by default.** Independent subtasks requested in the same model response run at the same time — the turn does not wait for one to finish before starting the next. Concurrency is capped (default 3, *Checkpoints & Subtasks* tab); when the slots are full the model is told so rather than being left to retry.

#### The roster

The orchestrator picks a sub-agent by name, and each name comes with a mode and a description of what it is good at:

| Agent | Mode | Use it for |
| --- | --- | --- |
| **scout** | ask | Read-only reconnaissance: "where is X defined", "what calls this" |
| **architect** | architect | Designing a solution and returning a plan |
| **coder** | code | Implementing a change end to end |
| **debugger** | debug | Reproducing a failure and making the smallest fix |
| **reviewer** | ask | Checking another agent's work before accepting it |

Only **coder** can edit. Define your own under **Settings → Agent Team**, or share them with the project in `<workspace>/.kateai/agents/*.md`:

```markdown
---
id: db-migrator
name: DB Migrator
description: Writes and reviews database migrations
mode: code
---
Anything after the frontmatter is extra guidance for the agent.
```

#### What the team shares

- **One MCP connection.** Sub-agents borrow the parent's, instead of dialing every server again.
- **One edit-lock table.** Two agents editing one file would silently overwrite each other, so the second one is refused with a clear message until the first reports back. Give each agent its own files.
- **Separate budgets.** A sub-agent gets half the parent's tool-call and model-request budget so a runaway child cannot starve its siblings.
- **Auditable.** `new_task` takes `include_transcript: true`, which returns the sub-agent's tool log alongside its answer so the orchestrator can check how it got there.
- **Cancelable.** Each sub-agent can be stopped on its own from the chat card, or all at once from the team menu.

### Chat panel

Sub-agents get a card of their own rather than a generic tool row: agent name and mode, a live `running · 12s · 5 steps` status, a **Cancel** button, the answer when it lands, and an expandable activity log of everything it ran. The toolbar shows how many agents are in flight and opens a menu with the roster and *Cancel all*.

### Agent loop

The model can call tools, read the results, and continue until the task is done (bounded by max model requests, max tool calls, and a requests-per-minute cap).

- Streaming text and hidden reasoning (`reasoning_content` / `<thinking>`)
- Structured **implementation plan** rendered as a checklist that updates as steps complete
- Parallel tool calls when the provider supports them
- Self-critique and required **verification** after file mutations (read, test, build, or lint)
- Adaptive temperature (higher while exploring, lower while applying edits)
- Automatic retry on rate limits, 5xx, and network errors (exponential backoff)
- Repeat-call detection so the agent does not loop on the same failed tool
- Context compression and smart truncation so long sessions stay within the window

```text
You ──► LLM (Grok / OpenAI / OpenRouter / local) or any ACP agent over stdio
          │
          ├── thinking (collapsible) + plan checklist
          ├── stream the answer into the panel
          └── tool calls
                ├── read / search / graph   → auto
                ├── write / edit / bash     → permission bar + diff preview
                └── sandbox + deny globs
                      └── result back to the LLM
```

### Tools

| Tool | What it does |
| --- | --- |
| `read_file` | Read a UTF-8 file; `offset` / `limit` for large files |
| `write_file` | Create or overwrite a file (open Kate buffer if the file is already open) |
| `edit_file` | Exact-string replace; optional `replace_all` |
| `multi_edit_file` | Apply multiple non-contiguous edits to a file in a single tool call |
| `list_dir` | List a directory |
| `grep` | Regex search with optional glob, case-insensitivity, and context lines |
| `glob` | Find paths (`**/*.h`, `src/**/*.cpp`, …) |
| `bash` | Run a command in the workspace under the active sandbox |
| `query_project_graph` | Query indexed files, symbols, imports, dependents, and paths |
| `new_task` | Spawn a sub-agent by name or mode and return its answer |

Plus every tool discovered from the connected MCP servers, as `mcp__<server>__<tool>`.

In **Plan mode**, **Ask**, **Architect**, and **Orchestrator** the model only receives the read-only group (`read_file`, `list_dir`, `grep`, `glob`, `query_project_graph`) plus MCP tools. Tool availability is enforced twice — once in the request that advertises the tools, and again when a call arrives — so a model that hallucinates a write gets a clear rejection instead of a silent one.

### Tool cards and diffs

Each tool call appears as a compact card in the transcript (icon, name, shell-style summary). Expand it for arguments and output.

- `write_file` / `edit_file` / `multi_edit_file` show a **unified diff** in the card before you approve
- Other tools show a text preview (command, grep hits, graph summary)
- Older cards auto-collapse after a configurable count so long runs stay responsive

### Edit tracker

**Accept edits** mode lets the agent write without stopping to ask, then hands you the bill afterwards. Pending changes collect in a review queue under the transcript, one row per file:

- **Keep** accepts a file, **Reject** reverts it to the content it had before the turn started that file
- A file the agent created is labelled `new`, and rejecting it **deletes** the file rather than leaving a zero-byte one behind
- Several edits to the same file collapse into one row showing how many were made, with the counts totalled across all of them
- **Review** opens the combined diff for that file; Escape closes it without deciding anything
- **Undo** (or `Ctrl+Z` with the queue focused) reverses the last decision, restoring the file too
- **Keep All** / **Reject All** settle every pending file; the queue clears when you switch conversations, so a decision about one thread can never revert another thread's files

The composer shows context-window usage as a percentage, turning amber at 70% and red at 90%, so a turn that is about to compact its own history is visible before it happens.

### Editor integration

Kate AI is a first-class KTextEditor plugin, so it sees the same documents you do.

- Active file, cursor line, selection (up to 4k), and the list of open files are sent as editor context
- Reads and writes prefer the in-memory buffer over disk
- Right-click **Kate AI** submenu on any editor view:
  - **Ask Kate AI About This** — explain the selection (or current line)
  - **Fix Selection** — find problems, explain, apply
  - **Refactor Selection** — clarity and maintainability, then apply
  - **Add Tests for Selection** — propose coverage first
- Workspace root is the nearest parent with `.git` or `.kateproject`, otherwise the active file’s directory

### Thinking and plan mode

- **Thinking mode** captures model reasoning in a collapsible “Reasoning” block (markdown, full reasoning block expandable in chat)
- Auto-collapse when the visible answer starts; click to expand later
- **Plan mode** (toolbar): read-only tools only, for inspection and an implementation plan, on top of whichever mode is active
- Structured plans render as an interactive checklist; steps can be marked complete as the agent works
- Reasoning effort (`minimal` / `low` / `medium` / `high`) for models that expose it (e.g. Grok reasoning)

### Project rules

Alongside `KATEAI.md`, the agent picks up rules files the same way Kilo Code does, and folds them into the system prompt in this order:

1. **Global rules** from Settings → Kate AI → Modes & Tools
2. `<workspace>/.kateai/rules/*.md`
3. `<workspace>/.clinerules/*.md` and `.kilocoderules/*.md`
4. `<workspace>/AGENTS.md` and `CLAUDE.md`
5. The mode's own rule file last, so `<workspace>/.kateai/rules/<modeId>.md` wins

Each rule file is wrapped in a `<rules source="…">` block. Nothing outside those directories is ever read as instructions — `mcp.json` and other configuration are excluded.

### Permissions and sandbox

Nothing destructive runs silently.

| Permission mode | What runs without asking |
| --- | --- |
| **Ask** (default) | Read-only tools, read-only shell (`ls`, `git status`, …), and `new_task` |
| **Accept edits** | File writes too; shell still prompts; pending edits can be accepted or rejected as a batch |
| **Always approve** | Tools run without a prompt |

Delegation itself never prompts: spawning a sub-agent is consent to spend, not a mutation. Every tool the sub-agent actually runs is still judged by this policy, so allowing the spawn does not let it edit unattended.

On top of the mode, individual tools can be auto-approved so they never prompt — from **Settings → Modes & Tools** for the built-in tools, and from the settings menu for MCP tools (which is the same thing as a server's `alwaysAllow`). Auto-approval sits below *Always approve* and above everything else, but it still never overrides the hard-deny list.

Hard-denied commands (`rm -rf /`, `mkfs`, `dd` to devices, curl-piped-to-shell) are blocked in every mode.

| Sandbox | Reads | Writes | Child network |
| --- | --- | --- | --- |
| **Workspace** | Anywhere except deny globs | Workspace only | Allowed |
| **Read-only** | Anywhere except deny globs | Blocked | Blocked |
| **Strict** | Workspace only | Workspace only | Blocked |
| **Off** | Deny globs only | Deny globs only | Unsandboxed |

Shell isolation depends on the OS: [bubblewrap](https://github.com/containers/bubblewrap) on Linux, `sandbox-exec` on macOS, and workspace-cwd plus command policy on Windows. File tools still work if the OS sandbox helper is missing.

**Always blocked paths:** `.env`, `.env.*`, `*.pem`, `*.key`, `id_rsa` / `id_ecdsa` / `id_ed25519`, `.ssh/**`, AWS credentials, GnuPG, `.netrc`, `*.p12`, `*.pfx`. Extra deny globs can be added in settings.

### Providers and models

| Provider | Default endpoint | Default model |
| --- | --- | --- |
| **Grok (xAI)** | `https://api.x.ai/v1` | `grok-4.5` |
| **OpenAI** | `https://api.openai.com/v1` | `gpt-4.1` |
| **OpenRouter** | `https://openrouter.ai/api/v1` | `x-ai/grok-4` |
| **OpenAI compatible** | `http://localhost:11434/v1` | Ollama, LocalAI, vLLM, … |
| **Claude compatible** | `https://api.anthropic.com/v1` | Anthropic Messages API / Bedrock-style |
| **ACP** | stdio JSON-RPC (or HTTP) | Any [Agent Client Protocol](https://agentclientprotocol.com) agent |

ACP API formats (Settings → Kate AI → AI Providers → ACP Agent):

| Format | How it talks | Auth |
| --- | --- | --- |
| ACP native (default) | Spawns a stdio subprocess and speaks [ACP](https://agentclientprotocol.com) JSON-RPC | The agent's own CLI login; optional API key copied into a per-agent env var |
| OpenAI compatible | HTTP `POST /chat/completions` at the endpoint URL | `Bearer` |
| Anthropic / Claude compatible | HTTP `POST /v1/messages` | `x-api-key` |

Native ACP works with any stdio agent. Pick a preset (command/args are filled in) or choose **Custom** and type your own launch line:

| Agent | Default command | Default arguments |
| --- | --- | --- |
| Grok Build | `grok` | `agent stdio` |
| Claude Agent | `npx` | `-y @agentclientprotocol/claude-agent-acp` |
| Codex | `npx` | `-y @agentclientprotocol/codex-acp` |
| Gemini CLI | `gemini` | `--acp` |
| GitHub Copilot | `copilot` | `--acp` |
| goose | `goose` | `acp` |
| OpenCode | `opencode` | `acp` |
| Cline | `npx` | `-y cline --acp` |
| Qwen Code | `npx` | `-y @qwen-code/qwen-code --acp` |
| Auggie CLI | `npx` | `-y @augmentcode/auggie --acp` |
| Cursor | `cursor-agent` | `acp` |
| Custom | (yours) | (yours) |

Kate is the ACP client: `session/prompt`, `session/update`, `session/request_permission`, `fs/read_text_file`, `fs/write_text_file`, `terminal/*`. Always-approve is handled in Kate for every agent. Grok Build also receives `--model` / `--always-approve` and Grok `_meta` (`yoloMode`).

To use Grok Build: install the Grok CLI, run `grok` once to log in, then set the provider to **ACP** with format **ACP native** and agent **Grok Build**. Other agents: install their CLI (or have `npx` on PATH), complete that agent's login, and pick the matching preset.

The model menu fetches live catalogs when the provider supports it; you can still type a model id. Only the selected provider’s key is sent. Keys live in Kate’s config (`KateAI` group).

### Sessions and history

- The active conversation is restored when Kate restarts
- Conversation list in the panel: switch, delete, start new (`Ctrl+Alt+N`)
- Configurable cap on saved conversations (default 50)
- Extra system prompt and `KATEAI.md` (optional compression) are included as project context

### Project graph

On workspace load the plugin indexes files, symbols, imports, and relationships. The agent queries it with `query_project_graph` (`summary`, `nodes`, `edges`, `dependencies`, `dependents`, `find_related`, `find_path`, `dependency_chain`) so it can map structure before grepping blindly. Node/edge caps and content-preview length are configurable.

### Configuration surface

**Settings → Configure Kate → Kate AI**, or the gear in the panel. Tabs:

- **AI Providers** — keys, default models, custom endpoints, ACP format
- **Security & Permissions** — permission mode, sandbox, command timeout, extra deny globs, tool-card collapse
- **Agent & Context** — plan mode, thinking, budgets, `KATEAI.md`, extra instructions, compression, temperature / top-p / max tokens, reasoning effort, self-critique, parallel tools, verbosity, structured thinking/planning, verification, adaptive temperature, context window reserve, saved-conversation cap
- **Modes & Tools** — default mode, per-tool auto-approval, project rules toggle, global rules
- **MCP Servers** — server list editor, enable/auto-connect, tool timeout
- **Checkpoints & Subtasks** — checkpoint toggle and retention, subtask depth, parallel sub-agents, subtask timeout
- **Agent Team** — the built-in roster and a custom agent editor

### Workspace files

Everything a team shares lives next to the code:

```text
<workspace>/.kateai/
├── mcp.json          # MCP server definitions
├── modes/            # custom mode definitions (*.md)
├── agents/           # custom sub-agent definitions (*.md)
├── rules/            # project rules (*.md)
└── project_graph.json  # generated, excluded from checkpoints
```

---

## Install from git

Kate does not need a world-readable system plugin. The **default** install is per-user: the plugin lives in your home directory, owned by you, mode `700` on Unix. Other accounts on the machine cannot read it. No `sudo`.

```bash
git clone https://github.com/KateAI/kate-ai.git
cd kate-ai
./install.sh --deps   # distro / Homebrew packages
./install.sh          # same as ./install.sh --user
```

On Windows:

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

That installs (Linux):

| Path | Mode | Why |
| --- | --- | --- |
| `~/.local/lib/qt6/plugins/kf6/ktexteditor/kateai.so` | `700` | Only your user can load the plugin |
| `~/.config/plasma-workspace/env/kate-ai.sh` | `600` | KDE session prepends `QT_PLUGIN_PATH` |
| `~/.config/environment.d/50-kate-ai.conf` | `600` | systemd user session does the same |

Kate’s distro build only scans `/usr/lib/qt6/plugins`. The two env files teach **your** Kate to also look in `~/.local`. They do not replace the system plugin path.

Use it immediately in this terminal:

```bash
export QT_PLUGIN_PATH="$HOME/.local/lib/qt6/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
kate
```

Then **Settings → Configure Kate → Plugins → Kate AI**. From the application menu, log out and back in once so the session picks up `QT_PLUGIN_PATH`.

Uninstall:

```bash
./uninstall.sh
```

On Windows: `.\install.ps1 -Uninstall`.

### System-wide (every user on the machine)

This is the only case that needs world-readable `755` on Unix: the file is owned by root, and Kate runs as a normal user, so “other” must be able to `mmap` it.

```bash
./install.sh --system
```

### Distro packages

`./install.sh --package` builds a native package from this tree. Recipes live under `packaging/`.

| Family | How |
| --- | --- |
| Arch / CachyOS / Manjaro | `./install.sh --deps && ./install.sh --package` (or `cd packaging/arch && makepkg -si` after placing `kateai-<ver>.tar.gz` from `packaging/mk-source-tarball.sh`) |
| Fedora / Asahi Remix | `./install.sh --deps && ./install.sh --package` → RPM under `packaging/dist/` |
| Debian / Ubuntu 25.04+ | `./install.sh --deps && ./install.sh --package` → `.deb` under `packaging/dist/` |

Then install the artifact with `pacman -U`, `dnf install`, or `apt install ./kateai_*.deb`.

### macOS

Install Kate from Homebrew (`brew install --cask kate`) and the KF6/Qt build stack, then:

```bash
./install.sh --deps
./install.sh
```

The script copies `kateai.so` into `~/Library/Application Support/kate/lib/qt6/plugins/kf6/ktexteditor/` and, when the app bundle is writable, next to Kate.app’s other ktexteditor plugins. A LaunchAgent prepends `QT_PLUGIN_PATH` for GUI Kate.

A Homebrew-built plugin may not load in the Craft-built Kate.app. For a matching ABI, build with [KDE Craft](https://develop.kde.org/docs/getting-started/building/craft/) using `packaging/craft/kateai.py`.

### Windows

Kate for Windows is a Craft/MSVC build. The plugin has to use the same toolchain.

1. Install [KDE Craft](https://develop.kde.org/docs/getting-started/building/craft/) and Kate.
2. Copy `packaging/craft/kateai.py` into your Craft blueprints tree (for example `extragear/kateai/kateai.py`).
3. In the Craft shell: `craft kateai`
4. Or from a Craft shell in this repo: `powershell -ExecutionPolicy Bypass -File .\install.ps1`

`install.ps1` copies `kateai.dll` into Kate’s `kf6\ktexteditor` directory when it can write there, otherwise into `%LOCALAPPDATA%\KateAI\plugins` and sets the user `QT_PLUGIN_PATH`. The Microsoft Store Kate is not supported.

### Manual user install

Prefer `./install.sh`. CMake’s relative `PATH` cache can otherwise install into the source tree. The script copies the built plugin itself:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
mkdir -p ~/.local/lib/qt6/plugins/kf6/ktexteditor
install -m 700 build/bin/kf6/ktexteditor/kateai.so ~/.local/lib/qt6/plugins/kf6/ktexteditor/kateai.so
```

### Dependencies

| Platform | Packages |
| --- | --- |
| Arch / CachyOS / Manjaro | `kate extra-cmake-modules cmake ninja qt6-base ktexteditor kcoreaddons ki18n kxmlgui kconfigwidgets kconfig kwidgetsaddons bubblewrap` |
| Fedora / Asahi | `kate extra-cmake-modules cmake gcc-c++ ninja-build qt6-qtbase-devel kf6-ktexteditor-devel kf6-kcoreaddons-devel kf6-ki18n-devel kf6-kxmlgui-devel kf6-kconfigwidgets-devel bubblewrap` |
| Debian / Ubuntu (25.04+) | `kate cmake extra-cmake-modules ninja-build qt6-base-dev libkf6texteditor-dev libkf6coreaddons-dev libkf6i18n-dev libkf6xmlgui-dev libkf6configwidgets-dev libkf6config-dev libkf6widgetaddons-dev bubblewrap` |
| macOS | Homebrew: `cmake extra-cmake-modules ninja qtbase kcoreaddons ki18n kconfig kconfigwidgets kxmlgui kwidgetsaddons` and cask `kate`; Craft recommended to match Kate.app |
| Windows | KDE Craft + Kate (MSVC). See `packaging/windows/install.ps1` |

Needs **Kate ≥ 24.08** (KF6), **Qt ≥ 6.5**, **CMake ≥ 3.25**. Sandboxed shell uses `bwrap` on Linux and `sandbox-exec` on macOS.

---

## Usage

| Action | How |
| --- | --- |
| Open the panel | **Ctrl+Alt+A**, or **View → Tool Views → Kate AI**, or **Tools → Kate AI → Show Kate AI** |
| Send a prompt | **Enter** |
| New line in the prompt | **Shift+Enter** |
| Mention a file | Type `@` then pick from open documents / `active` / `selection` / `workspace` |
| Previous prompt | **Up** on an empty first line |
| New chat | **Ctrl+Alt+N** or the **New chat** button |
| Past conversations | History button in the panel |
| Stop a turn | **Stop** |
| Approve a tool | Permission bar: Allow / Allow for session / Deny |
| Ask about the selection | Right-click → **Kate AI → Ask Kate AI About This** |
| Fix / refactor / tests | Right-click → **Kate AI** submenu |

Paste an API key under **Settings → Configure Kate → Kate AI**, pick a provider and model in the panel toolbar, and ask about the current file.

### Plan mode and project instructions

Choose **Plan** in the chat toolbar when you want an implementation plan before any edits. In this mode Kate AI only receives read-only tools; it cannot run shell commands or change files. Switch back to **Agent** when you want it to apply the plan.

For repository-specific guidance, create a `KATEAI.md` at the workspace root. It can describe architecture, style, test commands, and contribution rules. Loading it is enabled by default and can be disabled from **Configure Kate AI**.

---

## Configuration

**Settings → Configure Kate → Kate AI**

API keys:

- [Grok / xAI](https://console.x.ai)
- [OpenAI](https://platform.openai.com/api-keys)
- [OpenRouter](https://openrouter.ai/keys)
- OpenAI-compatible — local server URL (Ollama default `http://localhost:11434/v1`)
- Claude-compatible — Anthropic or compatible gateway
- ACP — your ACP server endpoint and API key

Keys are stored in Kate’s config (`KateAI` group). Only the selected provider receives its key.

---

## Development

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Always configure out-of-source (`-B build`, not `cmake .`). Do not copy a `build/` directory between machines or users — it embeds absolute paths such as `/home/<someone>/…`. `./install.sh` and `./build.sh` throw away a cache that belongs to another source path, or they switch to `build-$USER` if `build/` is not writable.

| Path | Role |
| --- | --- |
| `src/plugin*.cpp` | Kate / KTextEditor glue, tool view, actions |
| `src/chatwidget.cpp` | Prompt window, transcript, thinking, plan checklist |
| `src/toolcallwidget.cpp` | Inline tool cards and diff preview |
| `src/promptedit.cpp` | Composer, @ completion, prompt history |
| `src/edittracker.cpp` | Accept / reject pending writes |
| `src/agentloop.cpp` | LLM ↔ tool loop, verification, budgets |
| `src/llmclient.cpp` | Streaming (`/v1/chat/completions`, Anthropic, HTTP ACP), retries |
| `src/acpclient.cpp` | Native ACP client (stdio JSON-RPC, any ACP agent) |
| `src/tools.cpp` | File, grep, glob, bash, project-graph tools |
| `src/graph/` | Project graph index and queries |
| `src/sandbox.cpp` | Path jail, deny globs, bubblewrap |
| `src/permissions.cpp` | Ask / accept-edits / always-approve |
| `src/sessionstore.cpp` | Conversation persistence |
| `src/documentbridge.cpp` | Open Kate buffers vs disk |
| `tests/` | Sandbox, permission, SSE parser, retry, session, prompt tests |

---

## Troubleshooting

| Problem | Fix |
| --- | --- |
| Plugin missing after **user** install | Start Kate from a shell with `export QT_PLUGIN_PATH="$HOME/.local/lib/qt6/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"`. For the app menu, log out/in once. Check `ls -l ~/.local/lib/qt6/plugins/kf6/ktexteditor/kateai.so` is `-rwx------`. On macOS, confirm the LaunchAgent and/or a copy inside Kate.app. On Windows, confirm `kateai.dll` next to Kate or `%LOCALAPPDATA%\KateAI\plugins`. |
| Plugin missing after **system** install | Fully quit Kate. Confirm `ls -l $(qtpaths --plugin-dir)/kf6/ktexteditor/kateai.so` is `-rwxr-xr-x`. If it is `--x`, run `sudo chmod 755` on that file. |
| Kate aborts at startup with `SIGABRT` and a `QGuiApplicationPrivate::createEventDispatcher` backtrace | **Not a plugin problem** — the abort happens inside the `QApplication` constructor in `main()`, before Kate loads any plugin. Qt could not initialize a QPA platform plugin, i.e. Kate cannot reach the display. Check `journalctl --user \| grep -i "platform plugin"`; `Failed to create wl_display (Permission denied)` plus `could not connect to display :N` means the running uid cannot access the session (e.g. a different user than the one who owns the graphical session, or `XAUTHORITY` is unset/unreadable). Grant X11 access (`xhost +SI:localuser:<user>` from inside the session) and force `QT_QPA_PLATFORM=xcb`, or launch Kate as the session owner. |
| `cmake` complains about a foreign `/home/…` path or `CMakeCache.txt` | Leftover `build/` from another user or machine. `rm -rf build` and run `./install.sh` again. |
| `cmake` *Operation not permitted* on `prefix.sh` | Stale files in `build/` from another user. `rm -rf build` and configure again. |
| Sandboxed `bash` fails | On Linux install `bubblewrap` (`bwrap`). On macOS `sandbox-exec` is part of the OS. File tools still work without OS isolation. |
| A request never finishes and the transcript just sits there | A connection that is accepted but then goes silent is now cut off after 3 minutes of no data and reported as a network error, then retried with backoff. A stream that keeps sending tokens is never interrupted. If retries keep failing, check for a proxy, VPN, or captive portal intercepting the provider endpoint. |
| "A model request is already in progress" after a failed retry | Retry state was stranded by a backoff delay that overflowed into a negative timer interval. Fixed in `LlmClient::calculateDelay`; restart Kate if you are on an older build. |
| No API key error | Open **Settings → Configure Kate → Kate AI** and paste a key for the selected provider. |

---

## License

[LGPL-2.1-or-later](LICENSE) — same family as Kate / KTextEditor plugins.
