# Kate AI

<p align="center">
  <img src="docs/images/banner.jpg" alt="Kate AI — an agent panel inside the Kate text editor" width="100%">
</p>

<p align="center">
  <strong>An AI coding agent for <a href="https://kate-editor.org/">Kate</a></strong><br>
  Chat, editor-aware context, sandboxed tools, planning, and permission asks — Grok, OpenAI, and OpenRouter.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Kate-24.08%2B-1d99f3?logo=kde" alt="Kate 24.08+">
  <img src="https://img.shields.io/badge/Qt-6.5%2B-41cd52?logo=qt" alt="Qt 6.5+">
  <img src="https://img.shields.io/badge/KF-6-blue" alt="KDE Frameworks 6">
  <img src="https://img.shields.io/badge/license-LGPL--2.1--or--later-orange" alt="LGPL-2.1-or-later">
</p>

Kate AI adds an AI coding assistant to the Kate text editor: a modern chat tool view, an agent that can inspect and modify your project, editor-aware context, sandboxed tools, and a permission bar before anything destructive happens.

---

## Highlights

### Modern chat experience

- **Sticky auto-scroll** — the transcript follows streaming output only when you are already near the bottom (about 40 px). Scrolling up to read older content no longer causes the view to snap away from you.
- **Jump to latest** — when new output arrives while you are reading above the latest message, a floating `↓ Jump to latest` pill appears. Clicking it returns to the latest output and re-engages sticky scrolling.
- **Prompt history** — press **Up / Down** at the prompt boundary to cycle through your submitted prompts. The current draft is preserved so you can return to it after browsing history. History is bounded to 100 entries.
- **Escape to stop** — pressing **Escape** while an agent turn is running aborts the active turn, matching the stop control.
- **`@` mention completion** — type `@` followed by text to complete against special context tokens, open documents, and a bounded source-oriented workspace file index.
- **One-click copy** — user and assistant message cards expose a discrete **Copy** action with `✓ Copied` feedback. Code blocks remain horizontally scrollable when lines are wider than the panel.
- **Unified transcript cards** — live streaming and session-restored messages share the same card/header treatment, including consistent copy controls.
- **Quick-start actions** — an empty chat offers `Explain active file`, `Find bugs in selection`, and `Generate tests` chips that prefill the prompt and run immediately.
- **Fresh editor context** — the active document, cursor, open-file list, and current selection are refreshed immediately before a prompt is submitted, so a newly changed selection is not stale.

### Agent capabilities

- **Chat panel** on the right — multi-line prompt, Enter to send, Shift+Enter for a new line.
- **Plan mode** — read-only project inspection and planning without file changes or shell mutations.
- **Structured thinking and planning** — optional thinking blocks, structured plans, automatic thinking collapse, and interactive plan checklists.
- **Streaming replies** and an agent loop (model → tools → model) with bounded tool iterations.
- **Providers via API keys:** Grok (xAI), OpenAI, and OpenRouter.
- **OpenAI-compatible streaming** through `/v1/chat/completions`.
- **Tools:** `read_file`, `write_file`, `edit_file`, `list_dir`, `grep`, `glob`, and `bash`.
- **Permission asks** before writes and non-read-only commands — Allow / Allow for session / Deny.
- **Sandbox profiles** — workspace, read-only, strict, or off; shell commands can run under [bubblewrap](https://github.com/containers/bubblewrap).
- **Secret-file protection** — sensitive paths such as `.env`, `*.pem`, `*.key`, `.ssh`, AWS credentials, and GnuPG data are denied by default.
- **Editor-aware file access** — reads and writes can go through open Kate documents so unsaved buffers stay in sync.
- **Selection actions** — explain, fix, refactor, or generate tests for the current selection.
- **Project instructions** — optional `KATEAI.md` at the workspace root can provide repository-specific instructions.
- **Automatic retry** for transient API/network/server failures with configurable backoff.
- **Verification loop** — optional verification after file mutations with configurable retry attempts.
- **Optional parallel tool calls** for independent operations supported by the agent protocol.

---

## Reliability, memory, and performance

The current implementation also includes a reliability/performance pass aimed at keeping Kate responsive during long agent runs and high-volume streaming.

### Crash and lifetime hardening

- Workspace/project-graph initialization is **lazy**: opening Kate or restoring the Kate AI panel no longer recursively scans the workspace. The graph is loaded/generated only when the first agent turn actually needs it.
- Kate with no local document no longer guesses `$PWD`/`$HOME` as an AI workspace, avoiding accidental scans of the user's home tree during startup.
- Persisted chat history is bounded during restore (newest messages are kept, with oversized message/reasoning bodies truncated), preventing a pathological session from allocating an unbounded transcript when Kate launches.
- The editor-backed `DiskDocumentBridge` is owned by `ChatWidget`, so it remains alive for the complete lifetime of agent/tool execution in the tool view.
- Vulnerable widget references use guarded Qt pointer semantics where appropriate, reducing use-after-free risk during Kate view teardown.
- Deferred UI callbacks capture guarded object references rather than assuming the widget still exists when the event fires.
- Project-graph node state is initialized explicitly instead of relying on uninitialized scalar fields.

### Non-blocking tool execution

- Agent-triggered shell commands use an asynchronous `QProcess` path, avoiding `waitForFinished()` on Kate's GUI execution path.
- Tool results are associated with their tool-call ID, so a late process signal cannot resume a newer agent turn.
- Cancelling/stopping an agent also cancels a running asynchronous shell command.
- Shell timeouts remain enforced, with partial output preserved for diagnostics.
- The legacy synchronous shell helper remains available where it is explicitly used by non-agent code/tests; the live agent path uses the asynchronous runner.

### Bounded memory growth

- Streaming transcript updates are coalesced instead of scheduling a timer/render operation for every token.
- Markdown rendering of the active streaming message is coalesced with the UI update path, avoiding repeated expensive reparsing at token frequency.
- Live shell output is drained continuously and capped at **32 KiB** while a process runs; excess output is discarded with an explicit truncation marker rather than growing without a bound.
- Prompt history is bounded to **100** entries and de-duplicates repeated prompts by moving the newest occurrence to the end.
- Project-graph context is bounded by configurable node/edge limits and can omit file-content previews or compress graph information.
- Editor context, project instructions, system prompts, and older chat messages can be compressed/truncated to stay within the model context budget.

### UI update efficiency

- Sticky scrolling prevents unnecessary scrollbar movement when the user is reading older messages.
- Workspace `@` completion uses a bounded scan (up to 500 indexed workspace files / 600 total completion entries), ignoring common generated/vendor trees such as `.git`, `build`, `node_modules`, `.cache`, and `dist`.
- Workspace completion refresh is kept separate from the immediate editor-context refresh performed at Send time, so submitting a prompt does not force a large filesystem scan.
- Project/workspace refresh work is skipped when the detected workspace has not changed.
- Reusable UI objects are preferred for the floating scroll control and related effects rather than repeatedly allocating animation/effect objects.

These changes preserve existing API compatibility, agent execution protocols, and session serialization formats.

---

## Install from git

Kate does not need a world-readable system plugin. The **default** install is per-user: the `.so` lives in your home directory, owned by you, mode `700`. Other accounts on the machine cannot read it. No `sudo`.

```bash
git clone https://github.com/<you>/kate-ai.git
cd kate-ai
./install.sh          # same as ./install.sh --user
```

That installs:

| Path | Mode | Why |
| --- | --- | --- |
| `~/.local/lib/qt6/plugins/kf6/ktexteditor/kateai.so` | `700` | Only your user can load the plugin |
| `~/.config/plasma-workspace/env/kate-ai.sh` | `600` | KDE session prepends `QT_PLUGIN_PATH` |
| `~/.config/environment.d/50-kate-ai.conf` | `600` | systemd user session does the same |

Kate’s distro build normally scans `/usr/lib/qt6/plugins`. The two env files teach **your** Kate to also look in `~/.local`. They do not replace the system plugin path.

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

### System-wide (every user on the machine)

This is the only case that needs world-readable `755`: the file is owned by root, and Kate runs as a normal user, so “other” must be able to `mmap` it.

```bash
./install.sh --system
```

### Manual user install

Prefer `./install.sh`. CMake’s relative `PATH` cache can otherwise install into the source tree. The script copies the built `.so` itself:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
mkdir -p ~/.local/lib/qt6/plugins/kf6/ktexteditor
install -m 700 build/bin/kf6/ktexteditor/kateai.so ~/.local/lib/qt6/plugins/kf6/ktexteditor/kateai.so
```

### Dependencies

| Distro | Packages |
| --- | --- |
| Arch / CachyOS / Manjaro | `kate extra-cmake-modules qt6-base kf6-ktexteditor kf6-kcoreaddons kf6-ki18n kf6-kxmlgui kf6-kconfigwidgets bubblewrap` |
| Fedora | `kate extra-cmake-modules qt6-qtbase-devel kf6-ktexteditor-devel kf6-kcoreaddons-devel kf6-ki18n-devel bubblewrap` |
| Debian / Ubuntu (25.04+) | `kate cmake extra-cmake-modules qt6-base-dev libkf6texteditor-dev libkf6coreaddons-dev libkf6i18n-dev libkf6xmlgui-dev libkf6configwidgets-dev bubblewrap` |

Needs **Kate ≥ 24.08** (KF6), **Qt ≥ 6.5**, **CMake ≥ 3.25**, and `bwrap` for sandboxed shell execution.

---

## Usage

| Action | How |
| --- | --- |
| Open the panel | **Ctrl+Alt+A**, or **View → Tool Views → Kate AI** |
| Send a prompt | **Enter** |
| New line in the prompt | **Shift+Enter** |
| Recall previous prompt | **Up / Down** at the beginning/end of the prompt, or with an empty prompt |
| Cancel/stop an agent turn | **Escape** or the **Stop** button |
| Mention workspace/editor context | Type **`@`**, then choose a completion |
| Jump to newest output | Click **↓ Jump to latest** when it appears |
| Copy a message | Click **Copy** in the message header |
| New chat | **Ctrl+Alt+N** or the **New chat** button |
| Ask about the selection | Right-click in the editor → **Ask Kate AI About This** |

The prompt accepts multi-line content. **Enter** submits; **Shift+Enter** inserts a new line. The `@` completer recognizes the special tokens `@active`, `@selection`, and `@workspace`, plus matching open-file names and a bounded set of workspace source/text files.

The toolbar at the top of the panel switches provider, model, permission mode, sandbox, and Agent/Plan mode without opening settings. Use the gear button for the full configuration window.

### Sticky scrolling behavior

The transcript follows new streaming content while the viewport is already close to the bottom. Once you scroll far enough away from the bottom, your reading position is preserved. New output is still rendered normally, but it no longer pushes the scrollbar back down. The floating **Jump to latest** button appears when unseen content is below you.

Sending a new prompt always returns the transcript to the latest message and restores sticky scrolling.

### Prompt history behavior

Prompt history is kept in the current chat widget session. Up/Down navigation is intentionally boundary-aware so those keys still work as ordinary text navigation inside a multi-line prompt. Recalling history stores the current draft; moving forward beyond the newest history item restores that draft.

### `@` mentions

Completion sources are refreshed from the current editor/workspace state and include:

- `@active` — current active document
- `@selection` — current editor selection
- `@workspace` — current workspace
- open document names and workspace-relative paths
- a bounded source-oriented index of workspace files

Generated/vendor directories are excluded from the workspace scan to keep completion fast on large repositories.

### Plan mode and project instructions

Choose **Plan** in the chat toolbar when you want an implementation plan before any edits. In this mode Kate AI only receives read-oriented project tools; it cannot change files or execute mutating shell commands.

For repository-specific guidance, create a `KATEAI.md` at the workspace root. It can describe architecture, style, test commands, and contribution rules. Loading it is enabled by default and can be disabled from **Configure Kate AI**.

---

## Configuration

**Settings → Configure Kate → Kate AI**

### Providers

| Provider | Default base URL | Default model |
| --- | --- | --- |
| Grok (xAI) | `https://api.x.ai/v1` | `grok-4.5` |
| OpenAI | `https://api.openai.com/v1` | `gpt-4.1` |
| OpenRouter | `https://openrouter.ai/api/v1` | `x-ai/grok-4` |

API keys:

- [Grok / xAI](https://console.x.ai)
- [OpenAI](https://platform.openai.com/api-keys)
- [OpenRouter](https://openrouter.ai/keys)

Keys are stored in Kate’s config (`KateAI` group). Only the selected provider receives its key.

### Permission modes

| Mode | What runs without asking |
| --- | --- |
| **Ask** (default) | Read-only tools and read-only shell (`ls`, `git status`, …) |
| **Accept edits** | File writes too; shell still prompts |
| **Always approve** | Tools run without a prompt |

Hard-denied commands (`rm -rf /`, `mkfs`, `dd` to devices, curl-piped-to-shell) are blocked in every mode.

### Sandbox profiles

| Profile | Reads | Writes | Child network |
| --- | --- | --- | --- |
| **Workspace** | Anywhere except deny globs | Workspace only | Allowed |
| **Read-only** | Anywhere except deny globs | Blocked | Blocked |
| **Strict** | Workspace only | Workspace only | Blocked |
| **Off** | Deny globs only | Deny globs only | Unsandboxed |

The workspace is the nearest parent with `.git` or `.kateproject`, otherwise the active file’s directory.

### Agent and context controls

The **Agent & Context** settings expose tuning for both quality and resource usage. The exact controls can evolve with supported providers, but the current implementation includes:

| Area | Controls |
| --- | --- |
| Context compression | Compression level; project-graph size; file-content preview length; editor-context length; project-instruction length; system-prompt length |
| Generation | Temperature; top-p; max tokens; reasoning effort; verbosity |
| Agent behavior | Self-critique; parallel tool calls; verification after mutations; maximum verification attempts |
| Structured workflow | Structured thinking; structured planning; auto-collapse thinking; plan checklist; thinking-token cap; plan-step cap |
| Adaptive generation | Exploration/exploitation temperatures and optional phase-based temperature adjustment |
| Context budget | Smart context truncation; reserved response tokens; compression of older messages; compression threshold |
| Progress | Plan updates and narrative progress narration |

The defaults are chosen to balance coding quality with bounded context growth. For very large repositories, reducing project-graph limits and enabling stronger context compression can reduce request size and UI work.

### Retry behavior

Transient API/network/server failures can be retried automatically. Retry settings include:

- enable/disable automatic retry
- maximum retry attempts
- exponential or fixed strategy
- base delay
- maximum single-retry delay cap

---

## Context and workspace model

Kate AI builds its prompt from several sources as appropriate:

1. **Current user prompt**
2. **Editor context** — active file, cursor line, current selection, and open files
3. **Workspace information** — detected project root and project graph summaries
4. **`KATEAI.md`** — optional repository-specific instructions
5. **Conversation history** — subject to context-window and compression settings

The editor context is refreshed immediately before sending a prompt. This is important when a user changes a selection without switching tabs first.

The project graph is bounded and can be compressed. File previews, editor context, system prompts, and historical messages can also be limited so long-running chats do not grow indefinitely.

---

## Architecture

```text
You
 │
 ▼
ChatWidget ──► AgentLoop ──► LlmClient ──► Grok / OpenAI / OpenRouter
 │                │
 │                ├── tools ──► files / grep / glob / async bash
 │                │                  │
 │                │                  └── Sandbox + permissions
 │                │
 │                └── bounded context / retries / verification
 │
 ├── PromptEdit
 │     ├── history
 │     └── @ completion
 │
 └── transcript
       ├── sticky scrolling
       ├── copy actions
       └── streaming Markdown
```

The live chat path is event-driven. Network streaming and agent/UI state transitions use Qt's event loop; agent-triggered shell work uses `QProcess` asynchronously so long-running commands do not block the Kate UI.

---

## Development

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

For a headless UI/test environment:

```bash
XDG_CONFIG_HOME=/tmp XDG_DATA_HOME=/tmp XDG_RUNTIME_DIR=/tmp \
QT_QPA_PLATFORM=offscreen \
ctest --test-dir build --output-on-failure
```

Always configure out-of-source (`-B build`, not `cmake .`). Do not copy a `build/` directory between machines or users — it embeds absolute paths such as `/home/<someone>/…`. `./install.sh` and `./build.sh` throw away a cache that belongs to another source path, or they switch to `build-$USER` if `build/` is not writable.

### Recommended debugging for crashes and leaks

For native lifetime/memory regressions, build dedicated sanitizer configurations on a machine with matching Qt/KDE development packages:

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure
```

ThreadSanitizer can be used separately when investigating actual cross-thread races. Kate AI's current live shell path avoids creating a worker-thread dependency by using asynchronous Qt process events instead.

### Source layout

| Path | Role |
| --- | --- |
| `src/plugin*.cpp` | Kate / KTextEditor glue, tool view, actions |
| `src/chatwidget.cpp` | Prompt UI, transcript, streaming rendering, copy actions, sticky scrolling, quick-start UI |
| `src/promptedit.cpp` | Prompt input, history, Escape handling, `@` completion |
| `src/agentloop.cpp` | LLM ↔ tool loop, context budgeting, retries, verification, async tool coordination |
| `src/llmclient.cpp` | OpenAI-compatible network streaming (`/v1/chat/completions`) |
| `src/tools.cpp` | File and shell tools, including asynchronous agent `bash` execution |
| `src/sandbox.cpp` | Path jail, deny globs, bubblewrap |
| `src/permissions.cpp` | Ask / accept-edits / always-approve |
| `src/graph/projectgraph.h` | Bounded project graph state and node/edge metadata |
| `src/types.cpp` | Settings, provider helpers, serialization models |
| `tests/` | Prompt input, sandbox, permissions, SSE parser, retry, and session tests |

### UI behavior notes for contributors

- Avoid unconditional `scrollToBottom()` from high-frequency streaming callbacks. Use sticky-scroll state and coalesced updates.
- Prefer parent ownership or Qt smart/guarded pointers for widgets involved in deferred callbacks.
- Keep expensive filesystem indexing out of the prompt submission path.
- Keep agent-triggered shell execution asynchronous on the UI path.
- Bound buffers that can be fed by untrusted or user-generated command output.
- Preserve `toolCallId` checks when adding asynchronous tool completions.

---

## Troubleshooting

| Problem | Fix |
| --- | --- |
| Plugin missing after **user** install | Start Kate from a shell with `export QT_PLUGIN_PATH="$HOME/.local/lib/qt6/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"`. For the app menu, log out/in once. Check `ls -l ~/.local/lib/qt6/plugins/kf6/ktexteditor/kateai.so` is `-rwx------`. |
| Plugin missing after **system** install | Fully quit Kate. Confirm `ls -l $(qtpaths --plugin-dir)/kf6/ktexteditor/kateai.so` is `-rwxr-xr-x`. If it is `--x`, run `sudo chmod 755` on that file. |
| `cmake` complains about a foreign `/home/…` path or `CMakeCache.txt` | Leftover `build/` from another user or machine. `rm -rf build` and run `./install.sh` again. |
| `cmake` *Operation not permitted* on `prefix.sh` | Stale files in `build/` from another user. `rm -rf build` and configure again. |
| Sandboxed `bash` fails | Install `bubblewrap` (`bwrap`). File tools still work without it. |
| The prompt jumps while I read old output | This should not happen during normal streaming; sticky scrolling only follows the bottom when you are within the near-bottom threshold. Use **Jump to latest** to intentionally return to live output. |
| `@` completion is empty | Ensure the plugin can see the active workspace and open Kate documents. Workspace completion intentionally ignores generated/vendor directories and is bounded for performance. |
| Escape does not stop a turn | Confirm the prompt has focus and that no completion popup is consuming Escape. The active agent turn can also be stopped with the panel stop button. |
| No API key error | Open **Settings → Configure Kate → Kate AI** and paste a key for the selected provider. |
| Long-running shell commands make Kate freeze | The live agent path uses asynchronous `QProcess`; rebuild from the current source and ensure you are not running an older plugin binary from another install path. |

---

## Compatibility notes

- **API compatibility:** provider interfaces and the OpenAI-compatible request shape remain unchanged.
- **Agent protocol:** existing tool-call flow and agent turn/session behavior remain compatible.
- **Session serialization:** existing session data remains readable and writable; UI-only state such as prompt history is not required for session restoration.
- **Qt/KF6:** the plugin remains a Qt 6 / KDE Frameworks 6 Kate plugin.

---

## License

[LGPL-2.1-or-later](LICENSE) — same family as Kate / KTextEditor plugins.
