# Plan: Better Integration Between KateAI Tools and ACP Agents' Tools

This plan is based on analysis of the codebase (README, ARCHITECTURE, `AgentLoop`, `AcpClient`, `ToolRunner`, `Permissions`, `ChatWidget`, `EditTracker`, and existing tests). It focuses on concrete, testable improvements to make KateAI's native tool UX, permissions, edits tracking, diffs, history restoration, and MCP/ACP interoperability more consistent across all ACP agents.

## Goals
- Unify tool representation (names, args, risks, diffs) between KateAI's native tools and ACP agent tools.
- Make ACP tool cards, permissions, and edit tracking behave like native tool cards (same UX, same diff previews, same review flow).
- Improve robustness of ACP tool call parsing (rawInput/content/locations/diff shapes) across different agents (OpenCode, Claude Agent, Gemini CLI, Grok Build, Codex, etc.).
- Better history restoration for ACP conversations (tool cards with correct icons/colors/diffs/outputs) — build on `test_historyrestore`.
- Tighter integration between ACP's `suggest`/`query_project_graph`/`write_plan_file` and KateAI UI/flows.
- Improve sub-agent/ACP coordination, tool access control, and error reporting.

## Current State Summary (Key Findings)
- **ACP integration**: `AcpClient` speaks JSON-RPC, handles session updates (`tool_call`, `tool_call_update`), permissions (`session/request_permission`), and Kate extensions (`queryProjectGraph`, `writePlanFile`, `suggest`). It normalizes diffs/args in `permissionFromToolCall` and builds `PermissionRequest` with `mutatesFiles`, `describeDiff`, `path`.
- **Native tools**: `ToolRunner` defines tools, `describe()` builds permission requests/diffs, `run()` executes them. `AgentLoop` enforces mode-based `ToolAccess`, permissions, checkpoints, edit locks, sub-agents.
- **Edit tracking**: `ChatWidget` tracks mutating tools in `AcceptEdits` (native) or **all modes** for ACP (`mutatesFiles` flag / name heuristics). `EditTracker` reviews pending edits.
- **History restoration**: `ChatWidget::rebuildTranscript()`/`restoreHistoricalTool()` rebuild cards from stored messages; tests cover native and ACP ordering (results before assistant).
- **Tool UX**: `ToolCallWidget` shows diffs, streaming output, colors by tool. ACP aliases like `fs/read_text_file`, `edit`, `terminal/exec` need proper coloring/summaries.
- **Permissions**: ACP auto-resolves in AlwaysApprove/AcceptEdits cases; Kate answers its own client methods; permissions routed via `AgentLoop` for native, `AcpClient` for ACP.

## Plan

### 1) Unify Tool Schema & Naming Across Native/ACP
**Problem**: ACP agents use different names (`fs/read_text_file`, `fs/write_text_file`, `terminal/exec`, `edit`, `write`, `apply_patch`, `str_replace_editor`), making access control, coloring, summaries inconsistent.

**Tasks**:
- [ ] **Tool alias map**: Add a central mapping in `types.h/cpp` (or `tools.h/cpp`) for ACP/native aliases → canonical KateAI tool name (e.g., `fs/read_text_file`→`read_file`, `terminal/exec`→`bash`, `write_text_file`→`write_file`, `edit`/`str_replace*`/`apply_patch`→`edit_file`/`multi_edit_file`).
- [ ] **Canonicalize in AcpClient**: Normalize `toolCall.name`/kind on ingest (`handleSessionUpdate`, `permissionFromToolCall`) to canonical names for UI/policy/tracking.
- [ ] **ToolAccess compatibility**: Ensure `ToolAccess`/mode restrictions and auto-approve lists work with both canonical and qualified names (and MCP `mcp__*`).
- [ ] **Shell summary parity**: Extend `shellCommandFor()` to better summarize ACP aliases (show paths/commands clearly).
- [ ] **Tests**: Add tests for alias normalization and mode access with ACP names.

**Files**: `src/types.{h,cpp}`, `src/tools.{h,cpp}`, `src/acpclient.cpp`, `src/agentloop.cpp`, tests in `tests/test_agentparity.cpp`/new tests.

### 2) Improve ACP Tool Call Parsing & Diff Extraction
**Problem**: Different agents put args in different places (`rawInput`, `content[]` blocks, `locations[]`, diff blocks). Current parsing is good but can miss some shapes.

**Tasks**:
- [ ] **Robust diff extraction**: In `permissionFromToolCall`, also scan `content` for `tool_call_content`/diff variants, handle OpenCode's camelCase (`filePath`, `oldText`, `newText`), and handle `tool_call` updates that include diffs.
- [ ] **Raw input normalization**: When `rawInput` is a string (not object), try to parse as JSON; if not, treat as content. Also handle arrays.
- [ ] **Locations/path resolution**: Prefer `locations[].path`, fall back to args, normalize relative paths against workspace (consistent with native).
- [ ] **Multi-file diffs**: Some agents send multiple diff blocks - track all paths and show combined diff or per-file entries in tracker.
- [ ] **Terminal output**: Improve `toolContentText()` for `terminal/exec` (stdout/stderr), and ensure streaming `tool_call_update` progress is captured well.
- [ ] **Tests**: Add cases for different ACP shapes (OpenCode style, Claude style, Gemini style) in ACP-related tests.

**Files**: `src/acpclient.cpp`, `src/types.cpp` (helpers), tests under `tests/`.

### 3) Unify Permissions & Auto-Approval for ACP/Native
**Problem**: ACP permissions go through `AcpClient::handleRequestPermission` with its own `autoResolvePermission`; native go through `AgentLoop`. Need consistent behavior.

**Tasks**:
- [ ] **Central permission mapping**: Share risk classification between `PermissionPolicy` and ACP (respect MCP `readOnlyHint`, tool risk). Ensure ACP mutating tools are treated as writes.
- [ ] **Auto-approve parity**: Apply `Settings::autoApproveTools` and session grants to ACP requests (AcpClient already does partly). Also respect mode restrictions.
- [ ] **"Ask" mode behavior**: ACP agents can ask - ensure Kate's permission UI shows same info (diffs, paths). Handle cases where agent sends permission after starting tool.
- [ ] **AlwaysApprove/AcceptEdits**: In AcceptEdits, ACP edits go to tracker (done). Ensure permission prompts are skipped appropriately.
- [ ] **Tests**: Permission flow tests for ACP tools.

**Files**: `src/acpclient.cpp`, `src/agentloop.cpp`, `src/permissions.cpp/h`, `src/chatwidget.cpp`.

### 4) Better Edit Tracking & Review for ACP Tools
**Problem**: Edit tracking relies on `mutatesFiles` and name heuristics. Need robust detection and correct paths/diffs.

**Tasks**:
- [ ] **Improved detection**: In `ChatWidget`, also detect ACP mutations via canonical names, diff presence, or kind. Consider tool results containing diffs.
- [ ] **Path resolution**: Use `resolvedPath` from permission/request when available; resolve against workspace like native.
- [ ] **Snapshot timing**: For ACP, snapshot at `toolStarted` when we know it's mutating (we do this). Ensure it works even if permission comes late.
- [ ] **Multi-file edits**: Support tracking multiple files from one ACP tool call if agent reports them.
- [ ] **Diff reconstruction**: For history/restoration and live tracking, ensure diffs are attached to tracker entries.
- [ ] **Tests**: Expand edit tracker tests for ACP edits.

**Files**: `src/chatwidget.cpp`, `src/edittracker.cpp`, `src/acpclient.cpp`.

### 5) History Restoration Parity for ACP Tools
**Problem**: `test_historyrestore` covers ACP ordering; need to ensure all ACP tool shapes restore correctly (colors, icons, diffs, language highlighting).

**Tasks**:
- [ ] **Canonical names in restoration**: Normalize tool names during restore (alias map) so `ChatTheme::toolColor/icon` and summaries work.
- [ ] **Diff reconstruction**: For ACP edit/write tools, extract diffs from args/result if stored; attach to cards.
- [ ] **Language highlighting**: Ensure `setFilePath` is called from args/locations/diff headers.
- [ ] **Subtasks/ACP**: Handle ACP-spawned subtasks correctly in history.
- [ ] **Tests**: Add more ACP restoration cases (diffs, terminal output, different agent names).

**Files**: `src/chatwidget.cpp`, `src/types.cpp` (aliases), tests.

### 6) ACP Client Methods & Native Tool Parity
**Problem**: Kate exposes `queryProjectGraph`, `writePlanFile`, `suggest` to ACP agents - need to ensure they integrate well with UI/state.

**Tasks**:
- [ ] **queryProjectGraph**: Ensure it uses current project graph, respects permissions/context, returns useful data. Add error handling.
- [ ] **writePlanFile**: Already writes to temp plan file; ensure it emits events/notifications if needed, and agents can reference it.
- [ ] **suggest**: Already emits `suggestionPosted`; ensure cards appear correctly, can be accepted/dismissed, work in all modes.
- [ ] **Tool result formatting**: When ACP calls these Kate methods, format results nicely.
- [ ] **Tests**: Test ACP calling these methods.

**Files**: `src/acpclient.cpp`, `src/agentloop.cpp`, `src/chatwidget.cpp`.

### 7) Tool Access Control & Mode Integration
**Problem**: ACP tools need to respect mode restrictions and ToolAccess. Currently native path enforces access; ACP tools go through different paths.

**Tasks**:
- [ ] **Enforce access for ACP**: In `AgentLoop` or when ACP tools come in, check against `activeToolAccess()` (canonical names). Reject disallowed tools with clear messages.
- [ ] **Mode-aware tool lists**: Ensure `syncToolAccess()` considers ACP capabilities if relevant.
- [ ] **Plan mode**: ACP tools must respect plan mode restrictions.
- [ ] **Sub-agents**: Ensure child loops handle ACP tools correctly.
- [ ] **Tests**: Mode restrictions with ACP tools.

**Files**: `src/agentloop.cpp`, `src/acpclient.cpp`, `src/modes.cpp`.

### 8) Streaming, Progress & Error Handling
**Problem**: ACP tool progress streaming exists but could be better; errors need clear reporting.

**Tasks**:
- [ ] **Progress aggregation**: For long-running ACP tools, aggregate `tool_call_update` content better in cards.
- [ ] **Error messages**: Surface ACP errors clearly in tool cards and turn status.
- [ ] **Timeouts/cancellation**: Ensure cancellation works well for ACP tools (Kate already has cancel logic).
- [ ] **Retry**: ACP has retry logic; ensure it doesn't interfere with tool tracking.
- [ ] **Tests**: Streaming/progress tests.

**Files**: `src/acpclient.cpp`, `src/agentloop.cpp`, `src/chatwidget.cpp`.

### 9) Documentation & Testing
**Tasks**:
- [ ] **Update docs**: Add section in README about ACP tool integration, aliases, differences.
- [ ] **Update ARCHITECTURE**: Document ACP tool normalization flow.
- [ ] **Add tests**: Comprehensive tests for alias mapping, parsing, permissions, tracking, restoration.
- [ ] **Manual test checklist**: For common agents (OpenCode, Claude Agent, Gemini CLI).

**Files**: `README.md`, `ARCHITECTURE.md`, `tests/` (new test files as needed).

## Implementation Order
1. **Phase 1 (Foundation)**: Tool alias mapping + canonicalization (1) - enables everything else
2. **Phase 2 (Parsing)**: Improve ACP parsing/diffs (2)
3. **Phase 3 (Permissions/Tracking)**: Unify permissions and edit tracking (3,4)
4. **Phase 4 (UX)**: History restoration parity (5), client methods (6)
5. **Phase 5 (Polish)**: Access control, streaming, docs/tests (7,8,9)

## Success Criteria
- ACP tools show correct icons/colors/summaries like native tools
- Diffs display properly for ACP edit/write tools
- Edit tracking works correctly for ACP in all modes
- History restoration shows ACP tool cards identically to live ones
- Mode restrictions apply to ACP tools
- All existing tests pass; new tests added for ACP tool integration
- Works well with at least OpenCode, Claude Agent, Gemini CLI ACP agents

## Risks & Mitigations
- **Agent diversity**: Different ACP agents have different schemas - make parsing defensive and extensible
- **Backward compatibility**: Don't break existing behavior; add normalization as opt-in/transparent
- **Performance**: Normalization should be cheap (string maps/lookups)
- **Testing**: Use mock ACP agent (`tests/mock_acp_agent.py`) to test various shapes

## Quick Wins
- Add alias map for common ACP names
- Normalize names in AcpClient before emitting signals
- Extend `ChatTheme`/tool coloring to handle canonical names
- Improve `restoreHistoricalTool` to use canonical names
- Add more robust diff extraction from ACP content blocks
