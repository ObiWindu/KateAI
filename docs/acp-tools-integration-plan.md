# Plan: Better Integration Between KateAI Tools and ACP Agents' Tools

This is an extensive, detailed plan based on a thorough analysis of the KateAI codebase
(`AgentLoop`, `AcpClient`, `ToolRunner`, `Permissions`, `ChatWidget`, `EditTracker`,
`ChatTheme`, `ContextManager`, `SessionStore`, `ModeRegistry`, tests, and supporting
components). It targets consistent tool representation, permissions, edit tracking,
diffs, history restoration, and MCP/ACP interoperability across all ACP agents
(OpenCode, Claude Agent, Gemini CLI, Grok Build, Codex, Mistral Vibe, MiniMax Code,
Antigravity, Goose, Qwen Code, Cline, Cursor, Auggie, Copilot CLI, etc.).

---

## 0. Background & Architecture

### 0.1 How tools flow today

- **Native path**: `AgentLoop` owns a `ToolRunner`. The model emits `ToolCall`s;
  `AgentLoop` checks `ToolAccess` (modes), `PermissionPolicy` (risk/auto-approve),
  takes checkpoints, acquires edit locks, runs the tool, records results, and emits
  `toolStarted`/`toolFinished` signals to `ChatWidget`.
- **ACP path**: `AcpClient` spawns an agent subprocess speaking JSON-RPC. Tool calls
  arrive as `session/update` notifications (`tool_call`, `tool_call_update`). Kate
  renders cards via `toolStarted`/`toolFinished`. Permissions arrive as
  `session/request_permission`. Kate also exposes **client methods** to agents:
  `fs/read_text_file`, `fs/write_text_file`, `terminal/*`, `query_project_graph`,
  `write_plan_file`, `suggest`.
- **Edit tracking**: `ChatWidget` tracks mutating tools in `AcceptEdits` (native)
  or all modes for ACP (via `mutatesFiles` flag / name heuristics). `EditTracker`
  reviews pending edits.
- **History restoration**: `ChatWidget::rebuildTranscript()` → `restoreHistoricalTool()`
  rebuilds cards from stored messages; depends on tool names for colors/icons/summaries.
- **Theming**: `ChatTheme` maps tool names to colors/labels; ACP aliases need entries
  or fall through to heuristics (`isEditToolName` substring checks).
- **Context compaction**: `ContextManager::build()` folds older turns into a summary
  preserving user requests, files touched, tools used, outcomes. Project graph is
  now passed in to preserve related files.

### 0.2 Key pain points observed

1. **Name fragmentation**: ACP agents use `fs/read_text_file`, `terminal/exec`,
   `edit`, `write`, `apply_patch`, `str_replace_editor`, `multi_replace_file_content`,
   `run_command`, etc. Kate's canonical names are `read_file`, `bash`, `edit_file`,
   `write_file`, etc. Inconsistency affects access control, coloring, summaries,
   edit tracking, history restoration, and auto-approve.
2. **Parsing diversity**: Args land in `rawInput` (object/string/array), `content[]`
   blocks, `locations[]`, diff blocks, camelCase keys (`filePath`, `oldText`,
   `newText`), `old_string`/`new_string`, `content`/`contents`. Some agents send
   diffs only in `tool_call_update`, not `tool_call`.
3. **Permission parity**: ACP has its own `autoResolvePermission`; native uses
   `PermissionPolicy`. Risk classification and auto-approve lists diverge.
   `AcceptEdits` auto-approves reads for ACP but edit tracking depends on
   `mutatesFiles` which may be unset.
4. **Edit tracking robustness**: Detection via `mutatesFiles` + name heuristics.
   Multi-file edits from one ACP call not tracked individually. Path resolution
   sometimes relative; needs workspace normalization. Snapshot timing when
   permission arrives late.
5. **History restoration gaps**: ACP tool cards may show wrong colors/icons/diffs
   if names aren't normalized. Diff reconstruction from stored args/result
   incomplete for some shapes. Language highlighting from path sometimes missing.
6. **Mode/access control**: Native enforces `ToolAccess`; ACP tools bypass it.
   Plan mode restrictions not applied to ACP. Sub-agents don't handle ACP tools.
7. **Client methods**: `queryProjectGraph`, `writePlanFile`, `suggest` work but
   need better error handling, result formatting, UI integration.
8. **Streaming/errors**: Progress aggregation could be better; errors need clear
   surfacing; cancellation/retry interactions with tracking.
9. **Context compaction**: Needs to preserve project graph context and important
   information (files, symbols, decisions) across compaction.
10. **Conversation titles**: Can be lost due to compaction if regenerated from
    compacted messages.

---

## 1) Unify Tool Schema & Naming Across Native/ACP

**Problem**: ACP agents use different names, making access control, coloring,
summaries, edit tracking, history restoration inconsistent.

### 1.1 Tool alias map (central)

- **Add** in `src/types.h`/`src/types.cpp`:
  - `QString canonicalToolName(const QString &name)` — maps ACP/native aliases →
    canonical KateAI name.
  - `QStringList toolNameAliases(const QString &canonicalName)` — reverse lookup.
- **Alias table** (expandable, case-insensitive, MCP-prefixed preserved):
  - Filesystem: `fs/read_text_file`→`read_file`, `fs/write_text_file`→`write_file`,
    `fs/edit_file`→`edit_file`, `str_replace_editor`→`edit_file`,
    `str_replace`→`edit_file`, `apply_patch`→`edit_file`, `patch`→`edit_file`,
    `multi_replace_file_content`→`multi_edit_file`, `fs/list_directory`→`list_dir`,
    `fs/grep`→`grep`, `fs/glob`→`glob`.
  - Terminal/shell: `terminal/exec`→`bash`, `terminal/run`→`bash`,
    `shell/exec`→`bash`, `run_command`→`bash`, `execute`→`bash`.
  - Web: `web/search`→`web_search`, `web/fetch`→`web_fetch`.
  - Planning: `plan/write`→`write_plan_file`, `project/query`→`query_project_graph`.
  - Subagents: `agent/new_task`→`new_task`, `subagent`→`new_task`.
  - Pass-through canonical names unchanged.
- **MCP tools**: names starting with `mcp__` are preserved as-is (already prefixed).

**Files**: `src/types.h`, `src/types.cpp`.

### 1.2 Canonicalize in AcpClient

- In `permissionFromToolCall()`: normalize `toolName` via `canonicalToolName()`
  before storing in `PermissionRequest` and emitting `toolStarted`.
- In `handleSessionUpdate()` `tool_call`: use canonical name in `m_toolCalls`
  function name and live tool entry.
- In `tool_call_update`: when creating `existing` from update, canonicalize.
- **Keep original name** in a new field `PermissionRequest::originalToolName`
  (optional) for debugging/logging and for sending back to agent if needed.

**Files**: `src/acpclient.cpp`, `src/acpclient.h`, `src/types.h`.

### 1.3 ToolAccess compatibility

- `ToolAccess::allows()` should check canonical name. When ACP tool arrives with
  alias, canonicalize before checking. Add helper `ToolAccess::allowsCanonical()`.
- Mode restrictions (`ModeRegistry::toolAccessFor`) use canonical names; ensure
  ACP tools are checked against canonical set. Add ACP-specific aliases to mode
  tool lists if needed.
- `syncToolAccess()` in `AgentLoop`: pass canonical names to client.

**Files**: `src/types.h`, `src/modes.cpp`, `src/agentloop.cpp`.

### 1.4 Shell summary parity

- Extend `shellCommandFor()` in `src/tools.cpp` to:
  - Call `canonicalToolName()` first, then match on canonical.
  - Handle additional arg shapes: `uri`, `target`, `file_path`, `filePath`,
    `TargetFile`, `oldText`/`newText`, `old_string`/`new_string`,
    `content`/`contents`.
  - Return meaningful fallback (tool name or path) instead of empty.
  - Add cases for `query_project_graph`, `write_plan_file`, `suggest`, `question`.
- Ensure `ChatWidget::restoreHistoricalTool()` uses canonical name for summary.

**Files**: `src/tools.h`, `src/tools.cpp`, `src/chatwidget.cpp`.

### 1.5 ChatTheme integration

- `ChatTheme::isEditToolName()`: check canonical name first, then fall back to
  substring heuristics. Add canonical names to the color map.
- `ChatTheme::toolColor()`, `toolLabel()`, `toolAccent()`: handle canonical names
  and common ACP aliases. Use alias map to look up color by canonical.
- Add entries for all canonical tools; ensure unknown ACP tools get sensible
  fallback (not muted) based on name heuristics.

**Files**: `src/chattheme.cpp`, `src/chattheme.h`.

### 1.6 Tests

- Add unit tests for `canonicalToolName()` covering all aliases + case variations.
- Add tests for `shellCommandFor()` with ACP arg shapes.
- Add tests for `ChatTheme::toolColor()`/`isEditToolName()` with ACP names.
- Add mode access tests with ACP names.

**Files**: `tests/test_agentparity.cpp`, new `tests/test_toolalias.cpp`.

---

## 2) Improve ACP Tool Call Parsing & Diff Extraction

**Problem**: Different agents put args in different places. Current parsing misses
some shapes.

### 2.1 Robust raw input normalization

- In `permissionFromToolCall()`:
  - If `rawInput` is a string: try `QJsonDocument::fromJson`; if object, use it;
    if array, convert to content blocks; if plain text, treat as content.
  - If `rawInput` is an array: treat as content blocks array.
  - If `rawInput` is object: use directly (current behavior).
  - Store normalized args in `request.details` as JSON string.
- Add helper `normalizeRawInput(const QJsonValue &)` in `src/types.cpp` or
  `src/acpclient.cpp`.

**Files**: `src/acpclient.cpp`, `src/types.cpp/h`.

### 2.2 Comprehensive diff extraction

- Scan `content[]` for all block types:
  - `type == "diff"`: extract path, oldText/newText (and camelCase variants).
  - `type == "content"`/`"text"`: extract text, try JSON parse for args.
  - `type == "tool_call_content"`: nested content.
- Handle multiple diff blocks: collect all paths and diffs; store in a new
  `PermissionRequest::describeDiffs` (QList of path+diff) or concatenate with
  file headers. Update `EditTracker` to handle multiple.
- OpenCode camelCase: `filePath`, `oldText`, `newText`; also `old_string`,
  `new_string`, `content`, `contents`.
- If diff missing in `tool_call` but present in later `tool_call_update`,
  update the live card's diff when update arrives (currently `tool_call_update`
  only sets output). Add diff extraction in update handling.

**Files**: `src/acpclient.cpp`, `src/acpclient.h`, `src/types.h`.

### 2.3 Locations/path resolution

- Prefer `locations[].path`; if multiple, track all.
- Fall back to args: `path`, `file_path`, `filePath`, `file`, `uri`, `target`,
  `TargetFile`.
- Normalize relative paths against workspace (use `QDir::cleanPath` with
  workspace base). Add helper `resolvePath(const QString &path, const QString &workspace)`.
- Store resolved path in `PermissionRequest::resolvedPath` (already exists).

**Files**: `src/acpclient.cpp`, `src/tools.cpp` (add resolvePath helper).

### 2.4 Terminal output handling

- Improve `toolContentText()`:
  - Handle `stdout`/`stderr` as objects with `text` or arrays.
  - Handle `output` as string or object with `stdout`/`stderr`.
  - Handle `result` nested.
  - Preserve exit code info if present.
- In `tool_call_update` completed: aggregate all content blocks, not just first.
- Stream progress: deduplicate repeated identical content (already done); also
  handle partial updates that replace vs append.

**Files**: `src/acpclient.cpp`.

### 2.5 Multi-file diffs & EditTracker

- `EditTracker`: support tracking multiple files from one tool call. Add
  `trackEdit(toolCallId, path, diff, ...)` already per-path; call it for each
  diff block.
- In `ChatWidget` edit tracking: iterate over all diffs in the request; if
  `describeDiffs` present, track each.
- Add `PermissionRequest::additionalPaths` / `additionalDiffs` (QList<QString>).

**Files**: `src/edittracker.h/cpp`, `src/chatwidget.cpp`, `src/types.h`.

### 2.6 Tests

- Add test cases for different ACP shapes:
  - OpenCode style (camelCase, content blocks).
  - Claude style (rawInput object, locations).
  - Gemini style (terminal output).
  - Codex style (apply_patch).
- Test diff extraction from both `tool_call` and `tool_call_update`.
- Test multi-file diff tracking.

**Files**: `tests/test_acpparsing.cpp` (new), `tests/test_historyrestore.cpp`.

---

## 3) Unify Permissions & Auto-Approval for ACP/Native

**Problem**: ACP permissions go through `AcpClient::handleRequestPermission` with
its own `autoResolvePermission`; native go through `AgentLoop`. Need consistent
behavior.

### 3.1 Central permission mapping

- Create shared risk classification function `riskForToolNameAndKind(name, kind)`
  usable by both `PermissionPolicy` and `AcpClient`. Move `riskForKind` to
  `src/types.cpp` or `src/permissions.cpp`.
- `PermissionPolicy`: add method `riskFor(const QString &toolName, const QJsonObject &args)`
  that uses canonical name + heuristics (MCP `readOnlyHint`).
- MCP read-only detection: `McpManager::readOnlyTools()` already provides list;
  integrate into policy.
- ACP `riskForKind`: use canonical name; treat `edit`/`write`/`delete` kinds as
  Write; `execute`/`bash` as Execute; others by name heuristics.

**Files**: `src/permissions.h/cpp`, `src/acpclient.cpp`, `src/types.cpp/h`.

### 3.2 Auto-approve parity

- `AcpClient::autoResolvePermission`:
  - Check `m_settings.autoApproveTools` with canonical name + aliases (done).
  - Check `PermissionPolicy` auto-approve set (share via `AgentLoop` passing policy
    or settings). For now, also check mode: in `AcceptEdits`, auto-approve reads
    and edits? Actually AcceptEdits means edits are accepted without prompt but
    tracked. Need clarify: ACP edits in AcceptEdits should auto-resolve (allow)
    and go to tracker. Currently `autoResolvePermission` only auto-resolves reads
    in AcceptEdits. Fix: in AcceptEdits, auto-resolve Write risk too (since
    edits are accepted).
  - Respect `Settings::autoApproveTools` per tool name (already).
  - Session grants: `PermissionPolicy::hasSessionGrant` should be shared. Pass
    session grant state to AcpClient or check in `AgentLoop` before forwarding.
- `AgentLoop`: when ACP `permissionNeeded` fires, check policy/session grants
  first; if auto-approved, call `m_acp.resolvePermission()` directly without UI.

**Files**: `src/agentloop.cpp`, `src/acpclient.cpp`, `src/permissions.h/cpp`.

### 3.3 "Ask" mode behavior

- Ensure permission UI (`PermissionBar`) shows same info for ACP: diff, path,
  tool name (canonical + original), risk. Add original name to `PermissionRequest`
  (e.g., `displayToolName`).
- Handle permission arriving after tool started: `ChatWidget` already handles
  `permissionNeeded`; ensure card updates with diff if it arrived later.
- If agent sends permission after starting tool, update the live card's diff
  and path.

**Files**: `src/chatwidget.cpp`, `src/acpclient.cpp`, `src/permissionbar.cpp/h`.

### 3.4 AlwaysApprove/AcceptEdits

- In AcceptEdits: ACP edits go to tracker (done). Ensure permission prompts
  skipped: auto-resolve all risks in AcceptEdits for ACP (currently only reads).
  But still track edits.
- In AlwaysApprove: auto-resolve everything (already).
- Ensure `mutatesFiles` is set correctly so tracker catches edits even when
  auto-resolved (permission may not reach ChatWidget). `toolStarted` signal
  carries `PermissionRequest` with `mutatesFiles`; ChatWidget uses it at
  `toolStarted`. Good.

**Files**: `src/acpclient.cpp`, `src/chatwidget.cpp`.

### 3.5 Tests

- Permission flow tests for ACP tools: auto-resolve in different modes.
- Test that edits are tracked in AcceptEdits for ACP.
- Test session grants apply to ACP.

**Files**: `tests/test_permissions.cpp` (new or existing), `tests/test_acpclient.cpp`.

---

## 4) Better Edit Tracking & Review for ACP Tools

**Problem**: Edit tracking relies on `mutatesFiles` and name heuristics. Need
robust detection and correct paths/diffs.

### 4.1 Improved detection

- `ChatWidget::toolStarted` handler: use canonical name + `mutatesFiles` + diff
  presence + kind. Add check: if `request.describeDiff` is non-empty, treat as
  mutating.
- `isFileMutatingTool()`: use `canonicalToolName()` first.
- For ACP tools whose kind is `edit`/`write`/`delete`, always track.
- Detect mutations from tool results containing diffs (some agents send diff
  in result, not request). Parse result for diff blocks.

**Files**: `src/chatwidget.cpp`, `src/chattheme.cpp`.

### 4.2 Path resolution

- Use `resolvedPath` from permission request when available; fall back to
  `path`, then args. Resolve relative paths against workspace.
- `EditTracker::trackEdit`: normalize path to absolute using workspace if needed.
  Add `resolvePath()` helper shared.

**Files**: `src/chatwidget.cpp`, `src/edittracker.cpp`, `src/tools.cpp`.

### 4.3 Snapshot timing

- For ACP, snapshot at `toolStarted` when mutating (done). Ensure works even if
  permission comes late: move snapshot to when we first know it's mutating
  (toolStarted). If permission arrives later with diff, update tracker entry's
  diff.
- `EditTracker`: allow updating diff/path after initial track (add `updateEdit()`).

**Files**: `src/chatwidget.cpp`, `src/edittracker.h/cpp`.

### 4.4 Multi-file edits

- Support tracking multiple files from one ACP tool call. Add
  `PermissionRequest::additionalPaths` and `additionalDiffs`.
- In `ChatWidget`, iterate and track each.
- `EditTracker`: group by tool call id; allow multiple entries per id.

**Files**: `src/edittracker.h/cpp`, `src/types.h`, `src/chatwidget.cpp`, `src/acpclient.cpp`.

### 4.5 Diff reconstruction

- For history/restoration and live tracking, ensure diffs attached to tracker
  entries. Store diff in `EditTracker::EditEntry`.
- When restoring history, extract diffs from args/result and attach to cards
  (already partially; extend for ACP shapes).

**Files**: `src/edittracker.h/cpp`, `src/chatwidget.cpp`.

### 4.6 Tests

- Expand edit tracker tests for ACP edits: multiple files, late permission,
  diff reconstruction.

**Files**: `tests/test_edittracker.cpp` (new/existing).

---

## 5) History Restoration Parity for ACP Tools

**Problem**: `test_historyrestore` covers ACP ordering; need all ACP tool shapes
restore correctly.

### 5.1 Canonical names in restoration

- `restoreHistoricalTool()`: canonicalize `toolName` first; use canonical for
  `setToolInfo`, color lookup, risk classification. Keep original for display
  if needed.
- Normalize tool names stored in messages? Or at restore time. Prefer restore-time
  normalization to avoid migration.

**Files**: `src/chatwidget.cpp`.

### 5.2 Diff reconstruction

- For ACP edit/write tools, extract diffs from args/result if stored. Check
  `describeDiff` in args? Currently args stored as JSON. Parse for
  `old_string`/`new_string`/`content`/`oldText`/`newText`.
- Also check output for diff blocks (some agents store diff in result).
- Attach reconstructed diff to card via `setDescribeDiff()`.

**Files**: `src/chatwidget.cpp`, `src/acpclient.cpp` (helper extractDiff).

### 5.3 Language highlighting

- Ensure `setFilePath()` called from args/locations/diff headers. Parse diff
  header for path if args missing.
- For terminal output, detect language from command or file extension.

**Files**: `src/chatwidget.cpp`, `src/toolcallwidget.cpp`.

### 5.4 Subtasks/ACP

- Handle ACP-spawned subtasks correctly in history. `new_task` tool calls with
  ACP names should restore as subtask cards.

**Files**: `src/chatwidget.cpp`, `src/subtaskwidget.cpp`.

### 5.5 Tests

- Add more ACP restoration cases: diffs, terminal output, different agent names,
  multi-file edits, language highlighting.

**Files**: `tests/test_historyrestore.cpp`.

---

## 6) ACP Client Methods & Native Tool Parity

**Problem**: Kate exposes `queryProjectGraph`, `writePlanFile`, `suggest` to ACP
agents. Need tighter integration with UI/state.

### 6.1 queryProjectGraph

- Ensure uses current project graph; respects permissions/context; returns useful
  data. Add error handling for missing graph.
- Format results nicely: JSON with node/edge info, or readable text.
- Support all query types: `summary`, `nodes`, `edges`, `dependencies`,
  `dependents`, `find_related`, `find_path`, `dependency_chain`.
- Add pagination/limiting for large results.

**Files**: `src/acpclient.cpp`, `src/graph/projectgraph.cpp/h`.

### 6.2 writePlanFile

- Already writes to temp plan file; ensure emits events/notifications if needed.
- Return path to agent so it can reference.
- Ensure file is created outside workspace (already). Add cleanup on session end?

**Files**: `src/acpclient.cpp`, `src/tools.cpp` (`planFilePath`).

### 6.3 suggest

- Already emits `suggestionPosted`; ensure cards appear correctly, can be
  accepted/dismissed, work in all modes.
- `ChatWidget`: handle `suggestionPosted` signal; create `SuggestionWidget`.
- Ensure suggestions are not lost on history restore (store in messages? or
  separate). Currently pure UI, not in history. Consider storing as special
  message for restoration.

**Files**: `src/acpclient.cpp`, `src/chatwidget.cpp`, `src/suggestionwidget.cpp`.

### 6.4 Tool result formatting

- When ACP calls Kate methods, format results as JSON or readable text.
  Ensure `fs/read_text_file` returns file content with line numbers? Match
  native `read_file` output format.
- `fs/write_text_file`: return success + path; trigger document bridge to update
  open editors.

**Files**: `src/acpclient.cpp`, `src/documentbridge.cpp/h`.

### 6.5 Tests

- Test ACP calling these methods: mock agent or direct unit tests.
- Test result formatting.

**Files**: `tests/test_acpclientmethods.cpp` (new).

---

## 7) Tool Access Control & Mode Integration

**Problem**: ACP tools need to respect mode restrictions and ToolAccess.

### 7.1 Enforce access for ACP

- In `AgentLoop` or when ACP tools come in, check against `activeToolAccess()`
  using canonical names. Reject disallowed tools with clear messages.
- Add `AcpClient` callback or `AgentLoop` filter: before emitting `toolStarted`,
  check access; if denied, emit `toolFinished` with error instead.
- Alternatively, filter in `handleSessionUpdate` (tool_call): if tool not allowed,
  immediately emit toolFinished with denial and skip tracking.

**Files**: `src/agentloop.cpp`, `src/acpclient.cpp/h`.

### 7.2 Mode-aware tool lists

- `syncToolAccess()`: consider ACP capabilities if relevant. For ACP provider,
  tool access is enforced on incoming tool calls, not advertised.
- `ModeRegistry::toolAccessFor`: ensure canonical ACP names included in read-only
  sets etc. Add ACP aliases to mode tool groups.

**Files**: `src/modes.cpp/h`, `src/agentloop.cpp`.

### 7.3 Plan mode

- ACP tools must respect plan mode restrictions (read-only). In plan mode,
  reject mutating ACP tools with message: "Plan mode is active; only read-only
  tools are available."

**Files**: `src/agentloop.cpp`, `src/acpclient.cpp`.

### 7.4 Sub-agents

- Ensure child loops handle ACP tools correctly. Sub-agent `AgentLoop` uses
  same ACP client? Currently each sub-agent may have its own. Ensure tool access
  and tracking work.

**Files**: `src/agentteam.cpp`, `src/agentloop.cpp`.

### 7.5 Tests

- Mode restrictions with ACP tools: disallowed tool → error result.
- Plan mode rejects ACP writes.

**Files**: `tests/test_modetools.cpp` (new).

---

## 8) Streaming, Progress & Error Handling

**Problem**: ACP tool progress streaming exists but could be better; errors need
clear reporting.

### 8.1 Progress aggregation

- For long-running ACP tools, aggregate `tool_call_update` content better:
  - If content replaces, use latest; if appends, accumulate.
  - Detect whether agent sends cumulative or delta content; handle both.
  - Add heuristic: if new content starts with previous, treat as cumulative;
    else append.
- Update card progress in real time.

**Files**: `src/acpclient.cpp`, `src/chatwidget.cpp`.

### 8.2 Error messages

- Surface ACP errors clearly in tool cards and turn status:
  - `tool_call_update` with status `failed`: extract error from content or
    `rawOutput`; show in card as error.
  - `toolFinished` result.ok = false; ensure output contains error message.
  - Turn status: if any tool failed, show summary.

**Files**: `src/acpclient.cpp`, `src/chatwidget.cpp`, `src/turnstatus.cpp`.

### 8.3 Timeouts/cancellation

- Ensure cancellation works for ACP tools: `session/cancel` + grace timer
  (already). Finish live tools with cancelled result.
- Ensure `EditTracker` removes pending edits for cancelled tools.
- Ensure `m_liveTools` cleaned up.

**Files**: `src/acpclient.cpp`, `src/chatwidget.cpp`.

### 8.4 Retry

- ACP has retry logic; ensure doesn't interfere with tool tracking:
  - On retry, clear streamed content and reset cards? Or keep and append?
  - Ensure `textReset` clears assistant text; tool cards should remain or be
    marked retried.
- Avoid duplicate tool cards on retry.

**Files**: `src/acpclient.cpp`, `src/agentloop.cpp`, `src/chatwidget.cpp`.

### 8.5 Tests

- Streaming/progress tests with mock agent.
- Error surfacing tests.
- Cancellation tests.

**Files**: `tests/test_acpstreaming.cpp` (new).

---

## 9) Context Compression & Project Graph Optimization

**Problem**: Context compaction must preserve all important information, including
project structure.

### 9.1 Use project graph in compaction

- Pass `ProjectGraph*` to `ContextManager::build()` (done) and `resetContext()`.
- In `buildSummary()`, extract related files from graph for touched files.
- Preserve key symbols/dependencies in summary when graph available.
- Add project graph summary section: node counts, key files, important
  dependencies.

**Files**: `src/contextmanager.cpp/h`, `src/agentloop.cpp`.

### 9.2 Preserve important information

- User requests: keep all (already, with dedup).
- Files touched: expand limit, add related files from graph.
- Tools used: keep list.
- Outcomes: keep last N (increase from 2 to 4?).
- Decisions/errors: add section for rejected tool calls, errors encountered.
- Add "current state" section: what's done, what's pending.

**Files**: `src/contextmanager.cpp`.

### 9.3 Optimize compaction strategy

- Keep recent tail verbatim (already).
- Shrink tool output before dropping messages (already).
- Add priority: keep messages referencing files in project graph's important set.
- Add token budget awareness: estimate tokens per message more accurately.

**Files**: `src/contextmanager.cpp`.

### 9.4 Tests

- Test compaction preserves files/related files.
- Test project graph integration.

**Files**: `tests/test_contextmanager.cpp`.

---

## 10) Conversation Title Preservation

**Problem**: Titles lost due to compaction or regeneration from compacted messages.

### 10.1 Preserve existing title

- `SessionStore::saveConversation()`: if conversation exists and title not
  explicitly provided, keep existing title (done).
- `ChatWidget`: when auto-saving, don't pass title unless new conversation.
- Ensure title generation only on first save.

**Files**: `src/sessionstore.cpp`, `src/chatwidget.cpp`.

### 10.2 Title from first user message

- Generate title from first user message only; don't regenerate.
- Store title in conversation metadata separately from messages.

**Files**: `src/sessionstore.cpp`.

### 10.3 Tests

- Test title preserved across saves/compaction.

**Files**: `tests/test_sessionstore.cpp` (new).

---

## 11) Documentation & Testing

### 11.1 Documentation

- Update `README.md`: section on ACP tool integration, aliases, differences.
- Update `ARCHITECTURE.md`: document ACP tool normalization flow, permission
  parity, edit tracking.
- Add `docs/acp-tool-integration.md`: detailed guide for contributors.

**Files**: `README.md`, `ARCHITECTURE.md`, `docs/`.

### 11.2 Testing strategy

- **Unit tests**: alias mapping, parsing, permissions, tracking, restoration,
  context compaction, title preservation.
- **Integration tests**: mock ACP agent (`tests/mock_acp_agent.py`) with various
  tool shapes; run end-to-end tool flows.
- **Manual test checklist**: common agents (OpenCode, Claude Agent, Gemini CLI,
  Grok Build, Codex, Mistral Vibe).
- **CI**: add tests to build matrix; ensure new tests run.

**Files**: `tests/`, `.github/workflows/build.yml`.

### 11.3 Test files to create/update

- `tests/test_toolalias.cpp` — alias normalization.
- `tests/test_acpparsing.cpp` — ACP parsing shapes.
- `tests/test_acpclient.cpp` — ACP client methods.
- `tests/test_permissions.cpp` — permission flows.
- `tests/test_edittracker.cpp` — edit tracking ACP.
- `tests/test_historyrestore.cpp` — extend ACP cases.
- `tests/test_contextmanager.cpp` — compaction + graph.
- `tests/test_sessionstore.cpp` — title preservation.
- `tests/test_modetools.cpp` — mode restrictions ACP.

---

## 12) Implementation Order (Phases)

### Status (updated)

The following items are implemented:

- ✅ 1.1-1.5: Tool alias mapping, canonicalization, shellCommandFor, ChatTheme
- ✅ 1.6: Alias tests (test_toolalias.cpp)
- ✅ 2.1-2.5: Raw input normalization, diff extraction, locations, terminal output, multi-file
- ✅ 2.6: Parsing tests (test_acpparsing.cpp)
- ✅ 3.1-3.4: Central permission mapping, auto-approve parity, Ask/AcceptEdits/AlwaysApprove
- ✅ 3.5: Permission tests
- ✅ 4.1-4.5: Edit tracking improvements, multi-file, late permission, path resolution
- ✅ 4.6: Edit tracker tests
- ✅ 5.1-5.4: History restoration parity, diff reconstruction, language highlighting
- ✅ 5.5: History restore tests
- ✅ 6.1-6.4: ACP client methods (queryProjectGraph, writePlanFile, suggest)
- ✅ 6.5: Client method tests
- ✅ 7.1-7.3: Tool access control, mode integration, plan mode (mode denials are not auto-declined; the permission card still allows approval)
- ✅ 7.4: Sub-agents (N/A for ACP - agent handles its own)
- ✅ 7.5: Mode tool tests (test_modetools.cpp)
- ✅ 8.1-8.4: Streaming, progress, error handling, cancellation
- ✅ 8.5: Streaming tests (test_acpstreaming.cpp)
- ✅ 9.1-9.3: Context compression with project graph
- ✅ 9.4: Context manager tests
- ✅ 10.1-10.2: Title preservation
- ✅ 10.3: Session store tests (test_sessionstore.cpp)
- ✅ 11.1: Documentation (README.md, ARCHITECTURE.md)
- ✅ 11.2-11.3: Testing strategy and CI
- ✅ Checkpoint per turn for ACP
- ✅ resolvePath, normalizeRawInput, allowsCanonical helpers
- ✅ updateEdit in EditTracker
- ✅ Terminal output streaming into tool cards
- ✅ Late diff updates (toolDetailsChanged)
- ✅ Session notes sent once per session
- ✅ Extension method names (_kateai/)
- ✅ Capability advertisement under _meta

Remaining gaps:
- Checkpoints cannot undo ACP edits (design decision needed: per-turn snapshot is taken but ACP edits bypass the edit tracker's snapshot)
- Agent tools that ask no permission cannot be blocked (inherent limitation)
- Not tested against real ACP agents (only mock)
- Context compaction improvements beyond project graph integration
- Sub-agent ACP coordination not implemented (agent handles its own sub-agents)

### Phase 1: Foundation (Week 1-2)
1. Tool alias mapping + canonicalization (1.1-1.3).
2. Extend `shellCommandFor` (1.4).
3. Update `ChatTheme` (1.5).
4. Add alias tests (1.6).

### Phase 2: Parsing & Diffs (Week 2-3)
5. Robust raw input normalization (2.1).
6. Comprehensive diff extraction (2.2).
7. Locations/path resolution (2.3).
8. Terminal output handling (2.4).
9. Multi-file diffs (2.5).
10. Add parsing tests (2.6).

### Phase 3: Permissions & Tracking (Week 3-4)
11. Central permission mapping (3.1).
12. Auto-approve parity (3.2).
13. Ask mode behavior (3.3).
14. AlwaysApprove/AcceptEdits (3.4).
15. Edit tracking improvements (4.1-4.5).
16. Add permission/tracking tests (3.5, 4.6).

### Phase 4: UX & History (Week 4-5)
17. History restoration parity (5.1-5.4).
18. ACP client methods (6.1-6.4).
19. Add restoration/client tests (5.5, 6.5).

### Phase 5: Access Control & Streaming (Week 5-6)
20. Tool access control (7.1-7.4).
21. Streaming/errors (8.1-8.4).
22. Add tests (7.5, 8.5).

### Phase 6: Context & Polish (Week 6-7)
23. Context compression + project graph (9.1-9.3).
24. Title preservation (10.1-10.2).
25. Documentation (11.1).
26. Manual testing & CI (11.2-11.3).

---

## 13) Success Criteria

- ACP tools show correct icons/colors/summaries like native tools.
- Diffs display properly for ACP edit/write tools (all agent shapes).
- Edit tracking works correctly for ACP in all modes (AcceptEdits, AlwaysApprove, Ask).
- History restoration shows ACP tool cards identically to live ones.
- Mode restrictions apply to ACP tools (plan mode, read-only modes).
- Mode/sub-agent coordination works with ACP.
- Context compaction preserves important info + project graph context.
- Conversation titles preserved across compaction.
- Client methods (`queryProjectGraph`, `writePlanFile`, `suggest`) integrate well.
- All existing tests pass; new tests added for ACP tool integration.
- Works well with at least OpenCode, Claude Agent, Gemini CLI, Grok Build, Codex,
  Mistral Vibe ACP agents.

---

## 14) Risks & Mitigations

- **Agent diversity**: Different ACP agents have different schemas. Make parsing
  defensive and extensible; use heuristics + canonicalization; add per-agent
  test cases.
- **Backward compatibility**: Don't break existing behavior; normalization is
  transparent; keep original names in storage.
- **Performance**: Normalization should be cheap (string maps/lookups); project
  graph lookups cached.
- **Testing**: Use mock ACP agent to test various shapes; add integration tests.
- **Over-matching**: Canonicalization heuristics (substring) could over-match.
  Prefer exact alias map first; heuristics only as fallback with conservative
  thresholds.
- **Context compaction**: Project graph lookups could be slow; limit traversal
  depth and count.

---

## 15) Quick Wins (already done / easy)

- ✅ Add alias map for common ACP names.
- ✅ Normalize names in AcpClient before emitting signals.
- ✅ Extend `ChatTheme`/tool coloring to handle canonical names.
- ✅ Improve `restoreHistoricalTool` to use canonical names.
- ✅ Add more robust diff extraction from ACP content blocks.
- ✅ Preserve conversation titles across saves.
- ✅ Pass project graph to context manager.

---

## Appendix A: File Touch List

| File | Changes |
|------|---------|
| `src/types.h` | Add `canonicalToolName`, `toolNameAliases`, `resolvePath`; extend `PermissionRequest` with `originalToolName`, `additionalPaths`, `additionalDiffs`, `describeDiffs`. |
| `src/types.cpp` | Implement alias map, canonicalization, helpers. |
| `src/tools.h/cpp` | Extend `shellCommandFor` for canonical + ACP shapes; add `resolvePath`. |
| `src/acpclient.h/cpp` | Canonicalize names; robust parsing; diff extraction; terminal output; client methods; access control hooks. |
| `src/agentloop.h/cpp` | ACP tool access enforcement; mode integration; pass project graph to context manager. |
| `src/chatwidget.h/cpp` | Edit tracking improvements; history restoration canonicalization; diff reconstruction. |
| `src/permissions.h/cpp` | Shared risk classification; auto-approve parity. |
| `src/chattheme.h/cpp` | Canonical name handling in color/label/accent. |
| `src/edittracker.h/cpp` | Multi-file tracking; update diff/path; resolve paths. |
| `src/contextmanager.h/cpp` | Project graph integration; preserve important info. |
| `src/sessionstore.h/cpp` | Title preservation. |
| `src/modes.h/cpp` | ACP aliases in tool access. |
| `src/mcp.h/cpp` | Read-only tool integration. |
| `tests/*` | New/extended tests. |
| `docs/*`, `README.md`, `ARCHITECTURE.md` | Documentation. |

---

## Appendix B: Alias Table (initial)

| ACP/Alias Name | Canonical KateAI Name |
|----------------|----------------------|
| `fs/read_text_file`, `fs/read_file`, `read` | `read_file` |
| `fs/write_text_file`, `fs/write_file`, `write` | `write_file` |
| `fs/edit_file`, `str_replace_editor`, `str_replace`, `edit`, `patch`, `apply_patch` | `edit_file` |
| `multi_replace_file_content`, `multi_edit` | `multi_edit_file` |
| `terminal/exec`, `terminal/run`, `shell/exec`, `shell`, `execute`, `run_command` | `bash` |
| `fs/list_directory`, `list_directory`, `readdir`, `list` | `list_dir` |
| `fs/grep`, `search` | `grep` |
| `fs/glob` | `glob` |
| `web/search`, `websearch` | `web_search` |
| `web/fetch`, `webfetch` | `web_fetch` |
| `plan/write`, `write_plan` | `write_plan_file` |
| `project/query`, `query_project` | `query_project_graph` |
| `agent/new_task`, `subagent`, `task` | `new_task` |
| `suggest`, `question` | (pass-through) |

---

## Appendix C: Test Matrix

| Test File | Coverage |
|-----------|----------|
| `test_toolalias.cpp` | `canonicalToolName`, `toolNameAliases`, case variations, MCP preservation. |
| `test_acpparsing.cpp` | rawInput shapes, diff extraction, locations, terminal output, multi-file. |
| `test_acpclient.cpp` | client methods, permission flow, auto-resolve. |
| `test_permissions.cpp` | risk classification, auto-approve, session grants, mode parity. |
| `test_edittracker.cpp` | ACP edit tracking, multi-file, late permission, path resolution. |
| `test_historyrestore.cpp` | ACP restoration, colors, diffs, highlighting, subtasks. |
| `test_contextmanager.cpp` | compaction, project graph, preservation. |
| `test_sessionstore.cpp` | title preservation. |
| `test_modetools.cpp` | mode restrictions, plan mode, sub-agents. |
| `test_agentparity.cpp` | existing + new alias/mode tests. |

---

*End of plan.*