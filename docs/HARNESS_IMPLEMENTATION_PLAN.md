# KateAI Harness Implementation Plan

**How to build the roadmap in `docs/HARNESS_ROADMAP.md`, in order, with file-level tasks, acceptance criteria, tests, and release gates.**

| | |
| --- | --- |
| Status | Implemented through 1.0 in `feature/harness_complete`; see `docs/HARNESS_1.0.md` |
| Branch | `feature/harness_complete` |
| Baseline | 0.6.0, 55 CTest cases green, Linux sandbox validated |
| Target releases | 0.6.1 (quick wins) → 0.7.0 (substrate) → 0.8.0 (context/tools) → 0.9.0 (extensibility/agents) → 1.0.0 (platform) |
| Related | `docs/HARNESS_ROADMAP.md` (research and design), `ARCHITECTURE.md`, `CHANGELOG.md` |
| Assumption | One senior C++/Qt developer, part-time review, ~4 productive days/week; ±50% on estimates |
| Non-goal | Rewriting KateAI, adopting an agent framework, or changing the editor integration model |

---

## 1. Goals and scope

### 1.1 Outcomes by release

| Release | Outcome | User-visible proof |
| --- | --- | --- |
| **0.6.1** | Stop wasting context and make the harness legible | `/context` and `/usage` explain the window; the static prompt no longer changes every turn; tool output can be paged; no dead tools or commands |
| **0.7.0** | Make turns replayable and verification deterministic | `kateai replay` reproduces a failed turn; a `Stop` check blocks completion until tests pass; policies/hooks are enforced and audited |
| **0.8.0** | Make long sessions and large tool catalogs viable | 100-turn sessions stay coherent; a 100-tool MCP catalog adds <10% context; prompt cost drops ≥10% at equal success |
| **0.9.0** | Make the harness extensible and parallel | A project ships a policy + hooks + a skill; a worktree subagent returns a patch without touching the parent tree |
| **1.0.0** | Make the harness a platform | CI runs the scenario suite across providers; another host drives KateAI over ACP; telemetry and cost budgets are stable |

### 1.2 Scope boundaries

**In scope**

- The seven harness subsystems: loop, model transport shaping, tools/ACI, context/memory, safety, orchestration, extensibility.
- Session persistence, replay, fork, rewind, audit.
- Evaluation and observability infrastructure.
- A headless client and an ACP server profile.
- Documentation, migration, and release engineering for the above.

**Out of scope for this plan**

- Model training, fine-tuning, or hosted inference.
- A general plugin marketplace.
- A remote/cloud execution backend.
- Rewriting the Qt UI; the inspector is additive.
- Changing the KTextEditor integration or the review-queue model.

### 1.3 Definition of done (applies to every work package)

A work package is done only when:

1. The code is behind a feature flag (until its release gate) and the flag is documented.
2. Unit tests cover the new logic, including failure and edge cases.
3. One integration test exercises it through `AgentLoop` or the harness core.
4. Migration/backward-compatibility behaviour is implemented and tested (if it touches persisted state).
5. The user-visible change is documented in `CHANGELOG.md` and, when relevant, `README.md`/`ARCHITECTURE.md`.
6. No new setting is added without a test and a default; dead settings touched by the change are removed.
7. `ctest` is green on Linux; the Qt-offscreen suites pass; the diff has no `TODO`/`FIXME`.
8. A rollback path exists (flag off or migration downgrade note).

---

## 2. Delivery conventions

### 2.1 Engineering rules

- **C++20/Qt6/KF6 only.** No new third-party runtime dependencies. Use Qt (`QJsonDocument`, `QSqlDatabase`, `QRegularExpression`, `QProcess`) and the standard library.
- **One core, many clients.** New harness logic goes into `kateai_core` (`src/harness/`), never into `pluginview.cpp`/`chatwidget.cpp`. UI reads signals/events; it does not own policy.
- **Main-thread discipline.** Anything that touches `KTextEditor` documents stays on the GUI thread. Disk-only work goes to the existing worker-thread pattern (`ToolRunner::runReadToolAsync`). Never block the UI for more than a frame; coalesce event batches.
- **Append, don't mutate.** Events and summaries are appended. Rewind moves a head pointer; it never deletes history.
- **Deterministic rendering.** No timestamps, locale-dependent formatting, unordered containers, or memory addresses in prompts or event payloads that affect cache keys.
- **Fail closed, explain.** Security defaults deny; errors tell the user what happened and what to do. Sandboxed commands never silently degrade.
- **Small PRs.** One work package per PR where possible; a PR over ~800 changed lines needs a design note in the description.

### 2.2 Feature flags

Add `src/harness/harnessflags.{h,cpp}`:

```cpp
struct HarnessFlags {
    bool eventLog = true;        // always on once migrated; read fallback
    bool middleware = false;     // 0.7
    bool verificationGate = false;
    bool policyV2 = false;
    bool hooks = false;
    bool promptV2 = false;       // 0.8
    bool contextV2 = false;
    bool memoryV2 = false;
    bool toolSearch = false;
    bool skills = false;
    bool agentsV2 = false;
    bool inspector = false;
    bool telemetry = false;
    bool headless = false;       // 1.0

    static HarnessFlags load(const Settings &settings);  // settings + env
    bool enabled(const QString &name) const;
};
```

- Sources, in order: defaults → KConfig `[Harness]` group → `KATEAI_HARNESS_FLAGS` environment (comma-separated `+flag`/`-flag`) → per-workspace `.kateai/harness.json` (only for non-security flags).
- The settings page exposes a single "Experimental harness features" group in developer mode; no per-flag UI sprawl.
- Flags are removed one release after their feature becomes the default.

### 2.3 Branching and merge order

```
feat/state-of-the-art-harness      docs only (this plan + roadmap)
  └── harness/foundation           flags, skeleton, CI targets
        ├── harness/event-log      EP-2
        ├── harness/prompt-v2      EP-7 (independent)
        ├── harness/context-v2     EP-8 (after prompt budgets)
        ├── harness/tool-registry  EP-10 (independent)
        ├── harness/policy-hooks   EP-5 + EP-6 (after middleware)
        ├── harness/agents         EP-13 (after event log + middleware)
        ├── harness/inspector      EP-15 (after events + prompt + registry)
        └── harness/eval           EP-17 (can start early with mocks)
```

Rebase feature branches on `harness/foundation`; merge to `main` only when the release gate for that phase passes. Keep `main` releasable.

### 2.4 Versioning and release trains

- **0.6.1** — patch/minor: quick wins, no database schema migration.
- **0.7.0** — minor: session DB schema 2, policy/hook formats, trace.
- **0.8.0** — minor: prompt/context/tool registry; payload version 4 if needed.
- **0.9.0** — minor: skills, agents, inspector; config version 2.
- **1.0.0** — major-ish: headless/ACP, stable formats, long-term support commitment.

A release is cut only when its gate (Section 10) passes on the eval matrix.

---

## 3. Work breakdown overview

### 3.1 Epics

| Epic | Name | Release | Depends on | Estimate (days) | Risk |
| --- | --- | --- | --- | --- | --- |
| EP-0 | Foundation: flags, module skeleton, test targets | 0.6.1 | — | 3 | Low |
| EP-1 | Drift cleanup and safety floor | 0.6.1 | — | 4 | Low |
| EP-2 | Session event log + migrations + replay | 0.7 | EP-0 | 15 | High |
| EP-3 | Turn controller + middleware pipeline | 0.7 | EP-0, EP-2 (events optional) | 12 | High |
| EP-4 | Verification gate | 0.7 | EP-3 | 7 | Medium |
| EP-5 | Policy engine + hardline + audit | 0.7 | EP-3 | 10 | High |
| EP-6 | Hooks v1 | 0.7 | EP-3, EP-5 | 8 | Medium |
| EP-7 | Prompt assembler v2 + cache boundary | 0.7–0.8 | EP-0 | 10 | Medium |
| EP-8 | Context engine v2 + content store | 0.8 | EP-7 | 12 | Medium |
| EP-9 | Memory hierarchy v2 | 0.8 | EP-7 | 8 | Medium |
| EP-10 | Tool registry v2 + search + result budgets | 0.8 | EP-0 | 12 | Medium |
| EP-11 | Edit robustness (canonicalization/fuzzy/model-aware) | 0.9 | EP-10 | 6 | Low |
| EP-12 | Skills v1 | 0.9 | EP-7, EP-9, EP-10 | 9 | Medium |
| EP-13 | Subagents v2 + sidechains + worktrees | 0.9 | EP-2, EP-3, EP-10 | 12 | High |
| EP-14 | Model profiles + request shaping | 0.8–0.9 | EP-7, EP-10 | 7 | Medium |
| EP-15 | Inspector UI | 0.9 | EP-2, EP-7, EP-8, EP-10 | 9 | Low |
| EP-16 | Telemetry + cost accounting | 0.8–1.0 | EP-2 | 7 | Low |
| EP-17 | Evaluation harness + scenario suite | 0.7–1.0 | EP-0 | 12 | Medium |
| EP-18 | Headless CLI + ACP server | 1.0 | EP-2, EP-3, EP-5 | 14 | High |
| EP-19 | Documentation, migration, release hardening | every | all | 8 | Low |

**Total:** ~185 engineer-days, roughly **9–10 months at one developer**, **5–6 months with two** (one on core/substrate, one on UI/tools/evals). The critical path is EP-2 → EP-3 → EP-4/EP-5/EP-6 → EP-13 → EP-18.

### 3.2 Critical path and parallel tracks

```
EP-0 ─▶ EP-2 ─▶ EP-3 ─┬─▶ EP-4 ─┐
                       ├─▶ EP-5 ─┴─▶ EP-6
                       └─▶ EP-13 ─────▶ EP-18
EP-7 ─┬─▶ EP-8 ────────────────▶ EP-15
      ├─▶ EP-9 ─▶ EP-12
      └─▶ EP-14
EP-10 ─┬─▶ EP-11
       ├─▶ EP-12
       └─▶ EP-13
EP-17 (starts with mocks, day 1) ─▶ gates every phase
EP-16 (after EP-2) ─▶ EP-15/EP-18
```

If only one developer: run EP-7/EP-10 early because they are independent and improve cost immediately; delay EP-18 until 1.0.

---

## 4. Detailed work packages

Each package lists the files it touches, the steps, and the acceptance criteria. Ticket IDs are suggestions for the issue tracker.

### EP-0 — Foundation (3 days)

**Goal:** create the module boundary, flags, and test scaffolding without changing behaviour.

**Files**
- New: `src/harness/harnessflags.{h,cpp}`, `src/harness/harnesscore.{h,cpp}` (empty facade for now).
- Modify: `src/CMakeLists.txt` (add sources to `kateai_core`), `tests/CMakeLists.txt` (`kateai_test(test_harness_flags)`).
- New: `tests/test_harness_flags.cpp`.
- Docs: `ARCHITECTURE.md` one paragraph on the new `src/harness/` layer.

**Steps**

1. Add `src/harness/` with a `README.md` describing module ownership and the "no UI dependencies" rule.
2. Implement `HarnessFlags` with defaults, KConfig, environment parsing, and a `toStringList()` for the inspector.
3. Add `HarnessCore` as a plain struct holding flags, workspace, and a `QObject` parent; do not wire it into `AgentLoop` yet.
4. Add the test target and a CI job that runs the new tests (reuse the existing `ctest` step).
5. Add a `KATEAI_HARNESS_FLAGS` documentation block to `README.md` (developer section).

**Acceptance**
- `ctest -R test_harness_flags` passes.
- No behaviour change with flags default-off; `kateai --version`/plugin load unaffected.
- `HarnessFlags` round-trips through QSettings and env with precedence tests.

**Rollback:** delete the new module; no persisted state.

---

### EP-1 — Drift cleanup and safety floor (4 days)

**Goal:** remove misleading surface before adding new surface.

**Files**
- `src/modes.cpp` (tool list), `src/types.cpp` (remove `goal_report` definition), `src/tools.cpp` (remove handler), `src/tools.h`.
- `skills/system/run-workflow/SKILL.md` (remove or replace with a real skill; preferred: delete until the skill loader exists).
- `src/acpclient.{h,cpp}`, `src/chatwidget.cpp`/`src/pluginview.cpp` (wire or stop advertising ACP commands).
- `src/types.h` (remove unused settings), `src/settings.cpp` (config migration to version 2), `src/configpage.cpp` (remove controls).
- `src/tools.cpp` (`write_file` staleness; MCP result clip in `agentloop.cpp`/`tools.cpp`).
- `src/sandbox.cpp`/`src/permissions.cpp` (hardline floor, deny-first ordering).
- `ARCHITECTURE.md` (code names).

**Steps**

1. **Decide `goal_report`:** either add it to a tool group and use it, or remove the definition/handler/handler label. Recommendation: remove; the verification gate supersedes it.
2. **Remove or fix the `run_workflow` skill:** delete the file now; re-add a working version in EP-12.
3. **ACP slash commands:** add a minimal command menu in the composer or stop advertising `available_commands`. Recommendation: wire the menu (small) because agents advertise useful commands.
4. **Subagent question/permission stall:** forward the child request to the parent's intent dock and answer it through `resolvePermission`/`answerQuestion`; add a test.
5. **`write_file`:** route through `DocumentBridge::writeDocumentIfUnchanged` with the expected content (read-before-write) or require an explicit `overwrite: true`; add a conflict test.
6. **MCP clipping:** apply `clip()`/result budget to MCP output in `formatToolResult`; cap at the tool's `maxResultChars`.
7. **Unused settings:** remove `compactOnFailure`, `adaptiveTemperature`, `explorationTemperature`, `exploitationTemperature`, `enablePlanUpdates`, `narrativeProgress`, `messageSpeed`; either use `toolCallTimeoutMs` as the native per-tool timeout or delete it. Bump `SettingsStore::configVersion()` to 2 and migrate/ignore old keys.
8. **Hardline floor:** add an immutable list in `PermissionPolicy`/`Sandbox` that survives `AlwaysApprove` (reuse `isAlwaysDeniedCommand` and extend it: `rm -rf /`, `mkfs`, `dd of=/dev/`, fork bombs, shutdown, `git push --force` over protected refs). Add tests.
9. **Deny-first:** reorder `PermissionPolicy::evaluate` so a deny (hardline, deny glob, mode policy) always wins over auto-approve/session grant. Add tests for "broad deny vs narrow allow".
10. **Docs:** fix `ARCHITECTURE.md` names (`start()`/`systemPrompt()`), MCP transport description, README command list.

**Acceptance**
- No advertised tool/command dead-ends; `rg "goal_report|run_workflow" src/` returns nothing (or the feature works).
- A denied action cannot be allowed by session/auto-approve/hardline override; tests cover each layer.
- `write_file` refuses a changed file; test simulates the race.
- `ctest` green; settings migration test passes from a v1 config.

**Rollback:** settings migration keeps a backup key; hardline is additive.

---

### EP-2 — Session event log (15 days, 0.7)

**Goal:** append-only events as the source of truth, with projections and migration.

**Files**
- New: `src/harness/events.{h,cpp}` (event types, payload helpers, redaction), `src/harness/eventstore.{h,cpp}` (SQLite append/read), `src/harness/projections.{h,cpp}` (messages/UI), `src/harness/replay.{h,cpp}`.
- Modify: `src/sessionstore.{h,cpp}` (schema 2, event table, migrate/backfill), `src/agentloop.{h,cpp}` (emit events alongside existing state), `src/chatwidget.cpp` (read projection; live append).
- Tests: `tests/test_harness_events.cpp`, `tests/test_harness_migration.cpp`, `tests/test_harness_replay.cpp`.
- Tool: `kateai-headless replay` stub (full CLI in EP-18) or a test-only `ReplayRunner`.

**Steps**

1. Define the event vocabulary and payload structs (Appendix B of the roadmap). Freeze v1 of the schema; add `event_version` for evolution.
2. Implement `EventStore::append(conversationId, events)` as a single transaction updating `conversations.head_seq` and `message_projection`; `read(conversationId, fromSeq, limit)`.
3. Implement redaction at the write path: credentials, `Authorization`, env secret patterns; reuse `sanitizedProcessEnvironment` patterns.
4. Add content-store references for large payloads: `blobs/<sha256>` under the data dir; event payload holds `contentRef`, size, hash.
5. Migrate existing payloads: on first open of a v3 conversation, synthesize `turn/start`, `assistant/message`, `tool/call`, `tool/result`, `turn/end` events (best effort, `imported: true`); rebuild `message_projection`; verify equality with the payload's messages; on failure, keep legacy read-only mode and surface a notice.
6. Emit events from `AgentLoop` at: turn start/end, step start, request headers, assistant message, tool call/result, permission decision, checkpoint, compaction, notice. Keep the existing signals until the UI migrates.
7. Implement replay: given a conversation and turn, reconstruct the request/tool sequence without calling the model (recorded responses) or with a scripted provider; compare tool sequence and final state.
8. Add export/import JSONL (for debugging; not the primary store).
9. Add retention/GC for blobs and events (policy: keep full log; prune with conversation deletion).

**Acceptance**
- Append/read ordering, transaction atomicity (kill between append and projection → projection rebuilds), redaction, and migration tests pass.
- Replaying a recorded 20-step turn reproduces the same tool-call sequence and final diff.
- A 200-turn conversation loads in <100 ms and its projection equals the legacy payload's messages.
- Schema version 2 is documented; a v1 database migrates in place with a backup copy.

**Rollback:** flag `eventLog` (read legacy payload; stop writing events). Never drop the new tables; a downgrade reads the payload if its version is supported.

---

### EP-3 — Turn controller and middleware (12 days, 0.7)

**Goal:** one explicit state machine and a policy chain; port existing behaviour with no user-visible change.

**Files**
- New: `src/harness/turn.{h,cpp}`, `src/harness/middleware.{h,cpp}`, `src/harness/middleware/*.cpp`.
- Modify: `src/agentloop.{h,cpp}` (orchestrator only), `src/agentlocks.*` (unchanged), `src/types.h` (context structs).
- Tests: `tests/test_harness_turn.cpp`, `tests/test_harness_middleware.cpp`, `tests/test_stopbehavior.cpp` (extend).

**Steps**

1. Introduce `TurnContext`/`StepContext` and the `TurnController` state machine (`Idle → Starting → ModelWait → ToolWait → Verifying → Stopping → terminal`), with transition assertions in debug builds.
2. Define `TurnMiddleware` and the chain; `Flow::{Next, Replace, Deny, Stop, Retry}`; post-hooks unwind in reverse order.
3. Port, one at a time, each existing policy into middleware, keeping the old code path behind a flag until parity:
   - budgets (`maxModelRequests`, `maxToolCalls`, wall clock);
   - rate limiting;
   - repeat/stuck detection and recovery prompts;
   - permission decision/ask/deny;
   - checkpoint deferral;
   - result clipping (temporary until EP-10);
   - context compaction trigger;
   - telemetry hooks (stubs).
4. Keep `TurnState` signals stable for the UI; map internally.
5. Add property tests: no turn can reach `Completed` without every call having a result; a cancelled turn produces cancelled results for every open call; a late async result cannot settle a new turn.
6. Add a golden trace fixture (mock provider) and assert the event sequence.

**Acceptance**
- With `middleware=false`, behaviour is byte-identical (same event/signal sequence) on the golden trace.
- With `middleware=true`, the same tests pass; only the internal path differs.
- Each middleware is unit-tested in isolation (allow/deny/replace/retry).

**Rollback:** flag off returns to the legacy branches; keep both until 0.8.

---

### EP-4 — Verification gate (7 days, 0.7)

**Goal:** make "done" mean "the check passed".

**Files**
- New: `src/harness/checks.{h,cpp}`, `src/harness/middleware/verification.cpp`.
- Modify: `src/sandbox.*` (run check commands under the same policy), `src/tools.cpp` (optional `verify` tool? no — middleware only), `src/settings.*` (checks config), `src/agentloop.cpp` (remove the prose-only nudge when the gate is on).
- Tests: `tests/test_harness_checks.cpp`, `tests/test_verification_gate.cpp`; fixture repo `tests/fixtures/cpp-calculator` with a failing test.
- Docs: `README.md` "Verification" section, `AGENTS.md` guidance.

**Steps**

1. `CheckPlan` discovery, in order:
   - `.kateai/config.json` `verify` array (explicit commands);
   - `AGENTS.md`/`KATEAI.md` fenced `## Commands` section (parse `- ` lines with a command);
   - auto-detect: `CMakePresets.json`/`build/CTestTestfile.cmake` → `ctest --test-dir build --output-on-failure`; `Cargo.toml` → `cargo test`; `package.json` scripts → `npm test`/`pnpm test`; `pyproject.toml`/`pytest.ini` → `pytest`; `Makefile` → `make check`/`make test`;
   - fallback: `git diff --check` plus a build command if a build dir exists.
2. Classify checks by changed paths (e.g., only run the C++ suite for `.cpp/.h` changes); cap at 3 commands and 10 minutes total.
3. Run through `ToolRunner`/`Sandbox` so permission, env scrubbing and network policy apply; stream output to the card.
4. Middleware: on `Stop`, if mutations occurred and attempts < `maxVerificationAttempts`, run the plan; on failure, append a synthetic tool result with the output and `Flow::Retry`; on success, append an evidence event and complete.
5. Evidence: the final assistant message must contain the command and a pass/fail line; if the model claims success without evidence, append a controller reminder once.
6. Cap: after N failures, complete with `verify_failed` and a visible notice; never loop.
7. Add a `/verify` command that runs the plan on demand.
8. Remove the duplicated prompt sections when the gate is enabled (one completion contract).

**Acceptance**
- The fixture repo initially fails; the agent's first fix attempt does not complete; the gate feeds the failure back; the second attempt passes; the turn ends with evidence.
- A repo with no checks gets one evidence request, then completes.
- A hanging check is killed at the timeout; the user sees why.
- Checks obey sandbox/network policy (a check that tries the network fails in Strict).

**Rollback:** flag off; the existing prompt nudge remains.

---

### EP-5 — Policy engine, hardline, audit (10 days, 0.7)

**Goal:** rules as data, deny-first, auditable, with an immutable floor.

**Files**
- New: `src/harness/policy.{h,cpp}`, `src/harness/policyrules.{h,cpp}`, `src/harness/audit.{h,cpp}`.
- Modify: `src/permissions.{h,cpp}` (adapter), `src/sandbox.{h,cpp}` (matchers), `src/settings.*` (policy paths), `src/configpage.cpp` (policy status, not a full editor).
- Tests: `tests/test_harness_policy.cpp`, `tests/test_harness_audit.cpp`, fixtures with policy files.
- Docs: `docs/POLICY.md` (schema + examples).

**Steps**

1. Define the rule model and JSON schema (Appendix C): `id`, `match` (tool, path, command, network, mcp server/tool, risk, tainted), `decision` (`allow|ask|deny|review`), `reason`.
2. Load layers: managed → user → project → session grants; validate at load; reject unknown fields with a clear error (or warn and continue).
3. Implement matchers with the existing sandbox normalizer (symlink resolution before matching); command argv parsing for prefix/regex; path globs using `Sandbox::globMatch`.
4. Evaluation: hardline → managed deny → project deny → user deny → session deny → ask → allow → default; first match within a class; deny always wins.
5. Pre-filter: tools denied for the whole session are removed from the advertised schema list (`syncToolAccess`).
6. Audit: append `permission/decision` events (rule id, argument hash, decision, reviewer, latency); redact values by default; export JSONL.
7. `review` decision: call a configured reviewer model with a compact prompt; JSON verdict; fail closed; timeout 8 s; circuit breaker after 3 denials; return the reason.
8. Adapter: keep `PermissionPolicy` API for callers; delegate to `PolicyEngine` when the flag is on.
9. UI: permission card shows the matched rule id/reason and offers "Create rule from this" (writes a project policy draft, not auto-applied).

**Acceptance**
- Deny-first tests: broad deny vs narrow allow, managed deny vs project allow, hardline vs bypass.
- Path matching resolves symlinks and catches `src/../.env`.
- Audit contains one event per decision, no secret values.
- A `review` rule approves a safe action and denies a destructive one in tests with a scripted reviewer.
- Policy load errors do not brick the agent; the user sees a warning.

**Rollback:** flag off; legacy `PermissionPolicy` unchanged.

---

### EP-6 — Hooks v1 (8 days, 0.7)

**Goal:** deterministic user/project automation at lifecycle points.

**Files**
- New: `src/harness/hooks.{h,cpp}`, `src/harness/hooksconfig.{h,cpp}`, `src/harness/middleware/hooks.cpp`.
- Modify: `src/sandbox.*` (run hook commands sandboxed), `src/configpage.cpp` (trusted hooks list), `src/sessionstore.*` (trust hashes).
- Tests: `tests/test_harness_hooks.cpp`, fixtures.
- Docs: `docs/HOOKS.md`.

**Steps**

1. Config schema (Appendix E): events, matcher, handler (`command|http|prompt`), timeout, `onFailure`.
2. Trust: project hooks require a trust card showing exact commands; store a hash of the file; managed hooks trusted; user hooks trusted after scan.
3. Execution: argv mode (default) with a JSON event on stdin; shell only if declared; cap output 8 KiB; kill process group on timeout; pass a sanitized env + sandbox wrapper.
4. Contract: stdout JSON `{decision, reason, updatedInput?, additionalContext?}`; exit 2 = block; `onFailure: block` for crashes/timeouts.
5. Wire events: `SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `PostToolUseFailure`, `Stop`, `SubagentStart`, `SubagentStop`, `PreCompact`, `SessionEnd`.
6. Ordering: pre-tool hooks after policy pre-filter and before execution; a hook allow cannot override a policy deny; a hook deny always wins; stop hooks run before the verification gate.
7. Inspector/list: show installed hooks, trust state, last run/duration/failure.

**Acceptance**
- A `PreToolUse` hook blocks edits to `migrations/**` deterministically; the model receives the reason.
- A `PostToolUse` formatter runs after edits; failure is reported but does not corrupt the file.
- A `Stop` hook can send the model back to work (exit 2) with a cap.
- A timeout kills the process and follows `onFailure`; no orphan processes.
- Untrusted project hooks do not run until approved.

**Rollback:** flag off; hooks ignored.

---

### EP-7 — Prompt assembler v2 (10 days, 0.7–0.8)

**Goal:** stable, budgeted, model-aware prompts with a real cache boundary.

**Files**
- New: `src/harness/prompt.{h,cpp}`, `src/harness/promptprofiles.{h,cpp}`, `src/harness/promptsections/*.cpp`.
- Modify: `src/agentloop.cpp` (`systemPrompt()` delegates), `src/types.cpp` (`defaultSystemPrompt` becomes a section), `src/contextmanager.*` (message ordering), `src/llmclient.cpp` (cache-control hints), `src/pluginview.cpp` (editor context becomes a turn message).
- Tests: `tests/test_harness_prompt.cpp` with golden files under `tests/golden/prompts/`.
- Docs: `docs/PROMPT.md`.

**Steps**

1. Implement `PromptSection` (id, order, stability, budget, render) and `PromptAssembler` with deterministic rendering.
2. Move sections: identity, protocol (consolidated completion contract), tool rules, model guidance, mode, rules, memory, skills catalog, tool schemas, graph, plan. Static/session boundary after plan.
3. Move editor context, git status, workspace listing, reminders to a **user-context message** appended after the system prompt (not the system prompt). Remove the per-turn `git status` subprocess from `systemPrompt()`; compute it lazily and cache per turn.
4. Add per-section budgets and a total cap (25% of the window); overflow truncates with a visible marker and records a `prompt/budget` event.
5. Add profiles: `anthropic`, `openai`, `deepseek`, `gemini`, `local`; toggle thinking/plan prose, tool dialect, and budgets. Start with the current prompt as `local`; add the others incrementally.
6. Cache control: for Anthropic, add `cache_control` at the end of tools and the static block; for others, keep the prefix byte-stable. Never include a timestamp.
7. Remove duplicate verification/self-critique/execution sections when the verification gate is on; keep one short contract.
8. `/prompt` command (developer mode) dumps the exact request with secrets redacted; add a golden test that the static prefix hash is unchanged when only editor context changes.

**Acceptance**
- Golden tests per profile: stable output; changing editor context changes only the tail message.
- Static prefix hash is invariant across turns in a session (test).
- Measured input-token cost on a 50-turn fixture drops ≥10% at equal success (compare against the baseline trace).
- Prompt sections appear in `/context` with token counts.
- No prompt contains a timestamp or unsorted JSON.

**Rollback:** flag off; legacy `systemPrompt()` path.

---

### EP-8 — Context engine v2 (12 days, 0.8)

**Goal:** graduated compression, paging, merging, rehydration.

**Files**
- New: `src/harness/contextengine.{h,cpp}`, `src/harness/shapers/*.cpp`, `src/harness/contentstore.{h,cpp}`, `src/harness/tokens.{h,cpp}`.
- Modify: `src/contextmanager.*` (adapter/legacy), `src/agentloop.cpp` (call engine; emit compaction events), `src/tools.cpp` (`read_tool_result`), `src/sessionstore.*` (blob refs).
- Tests: `tests/test_harness_context.cpp`, long-trajectory fixtures, `tests/test_harness_contentstore.cpp`.

**Steps**

1. `ContextBudget` from `ModelProfile`; trigger at `window - reserve - 13K` (floor 10% of window).
2. Shaper 1: tool-result budget; write full output to the content store; replace with head/tail + ref; add `read_tool_result` (paged, conversation-scoped, read-only).
3. Shaper 2: prune old tool outputs to one-line summaries, preserving failures and files still in the working set.
4. Shaper 3: snip oldest turns, preserving the first request, plan, and last two user turns.
5. Shaper 4: incremental summary merge with a fixed template; keep the previous summary and update it; persist as an event.
6. Shaper 5: full compaction (existing reset) as last resort.
7. Rehydration: root instructions (if changed), plan, last ≤5 modified files (50K cap), running tasks, invoked skills (5K/25K caps).
8. Calibrate tokens: compare estimated vs provider `usage.input_tokens`; store a per-model factor; clamp 0.5–2.0.
9. `/context` manifest from the last request; show free space and each section.
10. GC: blobs pruned with the conversation; size cap per conversation (e.g., 200 MB) with oldest-first eviction and a notice.

**Acceptance**
- A 200-turn fixture stays under budget, and a question about an early decision is still answerable from the summary (judge or scripted assertion).
- Tool output over the budget is replaced by a ref and can be paged; the model can quote the tail.
- Incremental merge preserves an early decision across two compactions (test).
- Token estimate error vs provider usage is within ±15% after calibration.
- No compaction deletes events; the projection remains reconstructible.

**Rollback:** flag off; legacy `ContextManager::build`.

---

### EP-9 — Memory hierarchy v2 (8 days, 0.8)

**Goal:** scoped, path-aware instructions and a human-gated write path.

**Files**
- New: `src/harness/memory.{h,cpp}`, `src/harness/memorywrite.{h,cpp}`.
- Modify: `src/rules.{h,cpp}` (delegate/expand), `src/agentloop.cpp` (prompt section), `src/tools.cpp` (`memory_write` tool), `src/sessionstore.*` (memory inbox), `src/configpage.cpp` (memory manager).
- Tests: `tests/test_harness_memory.cpp`, `tests/test_memorywrite.cpp`.

**Steps**

1. Implement scopes: managed, user (`~/.config/kateai/AGENTS.md`, `~/.claude/CLAUDE.md`), project root (`AGENTS.md`, `AGENTS.override.md`, `CLAUDE.md`, `KATEAI.md`, `.kateai/rules`, `.claude/rules`, `.agents/rules`), nested (nearest ancestor), local (`*.local.md`, gitignored).
2. Extend the frontmatter parser: `paths`, `priority`, `alwaysApply`, `description`; validate globs.
3. Lazy loading: when a tool touches a path, load matching nested/path-scoped files once per turn; mark as loaded.
4. Deduplicate by resolved path and content hash (AGENTS/CLAUDE pointing at each other).
5. Budgets: 32 KiB/file, 128 KiB total, 32 files; per-scope budget in the prompt; truncation markers.
6. Render with provenance and precedence; document the order.
7. `memory_write`: propose a diff; user approves; write to `.kateai/memory/<topic>.md` (or user file); never silently edit `AGENTS.md`. Add a "promote to AGENTS.md" action that opens the diff in the editor.
8. Optional background extraction (flag): mine a finished conversation into inbox proposals; no auto-apply.

**Acceptance**
- A nested `src/api/AGENTS.md` loads only when a tool reads/writes under `src/api`.
- A `paths:` rule loads for matching globs only.
- Duplicate `CLAUDE.md` → `AGENTS.md` loads once.
- Memory write requires approval and shows a diff; rejection writes nothing.
- Budget tests: a 1 MB rules tree stays under the total cap with clear truncation.

**Rollback:** flag off; legacy `RulesLoader`.

---

### EP-10 — Tool registry, search, result budgets (12 days, 0.8)

**Goal:** a typed tool catalog that scales to large MCP sets.

**Files**
- New: `src/harness/registry.{h,cpp}`, `src/harness/tools/search.{h,cpp}`, `src/harness/tools/bridge.cpp`, `src/harness/toolresult.{h,cpp}`.
- Modify: `src/types.cpp` (`toolDefinitions()` reads specs), `src/tools.cpp` (result envelope + budgets), `src/mcp.*` (specs, deferred), `src/agentloop.cpp` (dispatch via registry), `src/modes.cpp` (groups from specs).
- Tests: `tests/test_harness_registry.cpp`, `tests/test_harness_toolsearch.cpp`, `tests/test_toolresult.cpp`.

**Steps**

1. Define `ToolSpec` (name, title, description, group, risk, concurrencySafe, idempotent, requiresPriorRead, alwaysLoaded, deferByDefault, maxResultChars, schema, renderer, aliases).
2. Convert the 42 built-in definitions to specs; generate the OpenAI/Anthropic JSON from them; keep the existing names/aliases.
3. Compute approximate schema tokens; expose totals to the inspector.
4. Deferral: always-loaded set; deferrable set; trigger at >15 tools or >10% of window.
5. `tool_search` (BM25 over name/description/aliases), `tool_describe`, `tool_call` bridge; discovered schemas appended as tool results, not system-prompt edits; direct calls to deferred tools still execute.
6. Result envelope `{ok, summary, content, files, exitCode, truncated, contentRef, hint}`; per-tool budget; overflow to the content store; `read_tool_result`.
7. Concurrency: dispatch `concurrencySafe` calls up to a cap; serial for writes/executes; results in call order.
8. Read dedup: key by path + mtime + content hash + range; return "unchanged since read at step N" when applicable.
9. MCP: wrap specs; defer by default; preserve `mcp__server__tool` names; add server-level search descriptions.
10. Mode/tool-access integration: `ToolAccess` still filters; add a `deferred` flag per spec.

**Acceptance**
- Golden JSON for built-in schemas is unchanged (no model regression) except deliberate additions.
- With 100 MCP tools, schema tokens drop ≥80% and the model can still discover/call a tool via search.
- A direct call to a deferred tool works; a call to a denied tool is rejected before execution.
- All results obey budgets; paging works; read dedup returns the marker.
- Concurrency test: 4 safe reads overlap; a write waits; order deterministic.

**Rollback:** flag off; legacy `toolDefinitions()`/dispatch.

---

### EP-11 — Edit robustness (6 days, 0.8)

**Goal:** fewer failed edits without weakening the contract.

**Files**
- Modify: `src/tools.cpp` (`edit_file`, `multi_edit_file`, `apply_patch`), `src/types.cpp` (patch parser), `src/documentbridge.*` (normalization hooks), `src/harness/models.*` (dialect).
- Tests: `tests/test_edits.cpp`, `tests/test_patch.cpp` with whitespace/Unicode/indentation corpora.

**Steps**

1. Add canonicalization: NFC/NFKC where safe, smart quotes → ASCII, CRLF/LF normalization, trailing-whitespace-insensitive matching, with a byte-fidelity overlay (apply in normalized space, write original bytes elsewhere).
2. Keep exact unique match as the primary; on failure, return a "closest match" with line numbers and a copy-ready snippet.
3. Optional fuzzy fallback (flag): Levenshtein ≥0.95, block-anchor; only when the model is in the `fuzzy` dialect; never for `apply_patch`.
4. Model-conditional dialect: `apply_patch` first for GPT/Codex profiles; exact edit for Claude/DeepSeek; fuzzy for small local models.
5. Improve patch diagnostics: hunk-level errors, "file changed since planning", "context not found", with a suggested next action.
6. Add a hunk-level retry hint (do not auto-retry; the model decides).
7. Tests: indentation changes, tabs vs spaces, Unicode quotes/dashes, line endings, duplicate blocks, very large files.

**Acceptance**
- The corpus shows ≥95% successful application for matches that differ only in whitespace/Unicode.
- Exact-match ambiguity still errors; no silent wrong-location edit.
- `apply_patch` remains atomic and byte-faithful.
- No line-number-based edit path exists.

**Rollback:** flag for the fuzzy fallback; canonicalization is safe to keep.

---

### EP-12 — Skills v1 (9 days, 0.9)

**Goal:** progressive-disclosure capability bundles, with trust.

**Files**
- New: `src/harness/skills.{h,cpp}`, `src/harness/skilltrust.{h,cpp}`, `src/harness/tools/skill.cpp`.
- Modify: `src/harness/prompt.cpp` (catalog section), `src/kateaicommand.cpp` (`/skill`), `src/chatwidget.cpp` (command menu), `src/configpage.cpp` (skills manager), `src/sessionstore.*` (trust hashes).
- Tests: `tests/test_harness_skills.cpp`, `tests/test_skilltrust.cpp`, fixtures.
- Docs: `docs/SKILLS.md`, 3 first-party skills.

**Steps**

1. Discovery: `.kateai/skills/<name>/SKILL.md`, `.agents/skills`, `.claude/skills` (read-compat), user dir.
2. Frontmatter: `name`, `description` (required), `paths`, `allowed-tools`, `disable-model-invocation`, `context` (`inline|fork`), `model`, `effort`, `version`, `license`.
3. Catalog: name + truncated description (≤1.5K total); body loaded by the `skill` tool; supporting files one level deep; scripts executed, not loaded.
4. Invocation: `skill(name)` tool; `/<name>` command; user-only skills.
5. `context: fork` runs in a subagent (EP-13) and returns a summary.
6. Trust: scan for prompt injection phrases, executables, network commands, path traversal; trust card with file hashes; project skills require approval; managed/bundled trusted.
7. First-party skills: replace `run-workflow` with `diagnose-test-failure`, `review-diff`, `prepare-release` (or keep only what is implemented).
8. Inspector: list skills, trust state, last use; enable/disable.

**Acceptance**
- A skill body is not in the prompt until invoked; the catalog costs <1.5K tokens for 20 skills.
- `disable-model-invocation` prevents model invocation but allows `/<name>`.
- A malicious skill is quarantined and cannot execute scripts until trusted.
- `context: fork` returns a summary, not a transcript.
- Tests cover frontmatter validation, path gating, and trust.

**Rollback:** flag off; no skills loaded.

---

### EP-13 — Subagents v2 (12 days, 0.9)

**Goal:** isolated, summary-returning, optionally worktree-isolated agents with cost accounting.

**Files**
- New: `src/harness/agents.{h,cpp}`, `src/harness/subagentrun.{h,cpp}`, `src/harness/worktree.{h,cpp}`, `src/harness/sidechain.{h,cpp}`.
- Modify: `src/agentteam.*` (AgentSpec), `src/agentloop.cpp` (`new_task` dispatch), `src/chatwidget.cpp`/`subtaskwidget.*` (patch/apply UI), `src/checkpoint.*` (worktree cleanup).
- Tests: `tests/test_harness_agents.cpp`, `tests/test_worktree.cpp`, `tests/test_subagent_events.cpp`.

**Steps**

1. `AgentSpec`: id, description, mode, model (`inherit` or profile), effort, permissionMode, tools allow/deny, skills, `maxTurns`, isolation, memory scope.
2. Parse from `.kateai/agents/*.md` (existing format + new fields) and user settings; keep the disabled-agent→Ask safety rule.
3. Sidechain: each subagent gets a child conversation `subagents/<taskId>.jsonl` (or child row with `parent_id`); append events; parent gets `subagent/start|end`.
4. Result contract: `{status, summary, filesChanged, checks, artifacts, usage, error}`; default summary-only; `subagent_transcript(taskId)` for debugging.
5. Questions/permissions: forward to the parent's intent dock; parent answers; child resumes; timeout/cancel.
6. Worktree isolation: preflight (git, clean enough, disk), `git worktree add --detach` under the cache dir; child workspace root; on end, diff artifact; UI Apply/Open/Discard; cleanup on cancel/exit.
7. Cost: per-child token/cost counters; parent `/usage` breakdown; budget reservation.
8. Concurrency: read-only parallel; write only worktree/disjoint; existing locks remain.
9. Cancel/abort: cancel child events, synthesize results, clean worktree.

**Acceptance**
- A review subagent returns only a summary; its transcript is retrievable by id.
- A worktree agent edits files without touching the parent tree; Apply produces the same diff; Discard removes the worktree.
- A child question blocks only the child; the parent can answer or cancel.
- A cancelled child leaves no running process/worktree and a cancelled result.
- The parent budget is never exceeded by children.

**Rollback:** flag off; existing `new_task` path.

---

### EP-14 — Model profiles and request shaping (7 days, 0.8–0.9)

**Goal:** stop treating every provider the same.

**Files**
- New: `src/harness/models.{h,cpp}`, `resources/models.json`, `src/harness/requestshaping.{h,cpp}`.
- Modify: `src/llmclient.cpp` (headers, cache control, tool choice, reasoning), `src/contextmanager.cpp` (window source), `src/settings.cpp` (overrides), `src/configpage.cpp` (profile view).
- Tests: `tests/test_harness_models.cpp`, golden request tests per provider.

**Steps**

1. `ModelProfile` fields (context, output, prices, cache semantics, tool features, edit dialect, prompt variant, token factor).
2. Bundled catalog + provider catalog merge + user override; unknown models get a conservative profile and a notice.
3. Request shaping per provider: tool schema dialect, parallel calls, tool choice, reasoning/thinking, cache breakpoints, max output.
4. Edit dialect + prompt variant selection.
5. Fallback model on repeated overload/5xx at a step boundary; event + notice.
6. `/usage` per-model cost and cache stats.
7. Remove the hard-coded context-window table or make it the fallback of the profile registry.

**Acceptance**
- Golden requests for 4 providers match expected headers/fields.
- An unknown model gets a safe default and a visible notice.
- Fallback triggers on a mocked overload and preserves the turn.
- Cost accounting matches provider usage for a recorded response.

**Rollback:** flags per behaviour (`modelProfiles`, `promptProfiles`); fallback table empty by default.

---

### EP-15 — Inspector UI (9 days, 0.9)

**Goal:** make the harness visible and steerable.

**Files**
- New: `src/inspectorpanel.{h,cpp}`, `src/contextview.{h,cpp}`, `src/traceview.{h,cpp}`, `src/harnessreport.{h,cpp}`.
- Modify: `src/pluginview.cpp` (action/tab), `src/chatwidget.cpp` (commands), `src/configpage.cpp` (links), `src/CMakeLists.txt` (plugin target).
- Tests: `tests/test_harnessreport.cpp`, offscreen widget test `tests/test_inspector.cpp`.

**Steps**

1. Harness tab with sub-tabs: Context, Tools, Rules/Memory, Hooks/Skills, Trace, Cost, Rewind.
2. Context: stacked bar + table from the last `request/headers`; source links; refresh on turn end.
3. Tools: effective set, always/deferred, token cost, risk, approval rule.
4. Rules/memory: loaded files, scope, bytes, hash, conflicts, memory inbox.
5. Hooks/skills: trust, last run, failures.
6. Trace: waterfall from events, durations, tokens, cache, permissions; export JSON.
7. Cost: per turn/conversation/model; budget warnings.
8. Rewind: timeline with checkpoints; restore code/conversation/both; fork.
9. Commands: `/context`, `/usage`, `/trace`, `/tools`, `/memory`, `/skills`, `/hooks`, `/rewind`, `/fork`.
10. Accessibility/theme: use `ChatTheme` colours; no hard-coded palettes.

**Acceptance**
- The Context tab explains 100% of the last request's estimated tokens.
- The Trace tab reproduces a turn from events (no separate logging).
- Rewind restores both files and conversation in a test; fork creates a new conversation with a parent link.
- Widget tests run offscreen and pass.
- No harness operation is required to open the panel (read-only).

**Rollback:** action hidden by flag.

---

### EP-16 — Telemetry and cost (7 days, 0.8–1.0)

**Goal:** local-first traces and cost accounting; optional OTLP.

**Files**
- New: `src/harness/telemetry.{h,cpp}`, `src/harness/cost.{h,cpp}`, `src/harness/otlp.{h,cpp}`.
- Modify: `src/llmclient.cpp` (usage), `src/acpclient.cpp` (usage if reported), `src/sessionstore.*` (trace store/export), `src/configpage.cpp` (telemetry opt-in).
- Tests: `tests/test_harness_cost.cpp`, `tests/test_harness_telemetry.cpp`.

**Steps**

1. Trace recorder: JSONL per conversation (`events` already; add spans with start/end/duration/parent).
2. Span model: `invoke_agent` → `chat`/`execute_tool`/`compaction`/`permission`; attributes per OTel GenAI + `kateai.*`.
3. Cost: per model price table, cached tokens, per turn/session; budget warnings.
4. Redaction: no content by default; hashes/lengths; opt-in content capture.
5. OTLP export (opt-in): batch, retry, local endpoint; no external default.
6. `/usage` and `/trace` UI; export file.
7. Tests: cost arithmetic, cache price, span tree, redaction.

**Acceptance**
- A turn's cost equals the sum of its model calls at the catalog price (±rounding).
- Cache reads are priced separately and shown.
- Traces contain no secrets with content capture off.
- Export works offline; OTLP failure does not affect the turn.

**Rollback:** flag off; no spans/export.

---

### EP-17 — Evaluation harness (12 days, 0.7–1.0)

**Goal:** prove changes instead of vibing them.

**Files**
- New: `tests/evals/run.py` (or a C++ runner), `tests/evals/scenarios/*.yaml`, `tests/evals/fixtures/`, `tests/evals/scripted_provider.py`, `tests/evals/report.py`.
- Modify: `tests/CMakeLists.txt` (opt-in target), CI workflow (nightly), `docs/EVALS.md`.
- Tests: the runner itself; a smoke scenario in CI.

**Steps**

1. Scenario schema: id, fixture, prompt, provider (`scripted`/live), script, assertions (files, tests, events, cost/turns), budget.
2. Scripted provider: deterministic tool-call sequences; supports streaming/thinking/tool args variants; reuses the mock LLM server style.
3. Fixture repos: C++/CMake (calculator with a failing test), Python, docs; each with a verifier command.
4. Assertions: file contains/unchanged, test exit code, event types/order, max turns/cost, verification evidence.
5. Injection corpus: web, MCP, file, ACP, skill/hook; assertions on egress/destructive actions and audit.
6. Live matrix (nightly): 3 models × 20 scenarios with caps; record/replay.
7. Report: pass@1, cost, tokens, turns, cache hit, p95 time; diff vs baseline; fail the job on regression thresholds.
8. CI: PR runs T1+T2; nightly runs T3; weekly T4 optional.

**Acceptance**
- `ctest -R harness_eval` runs the scripted suite in <2 minutes.
- The injection suite has zero unapproved egress/destructive actions.
- A regression (e.g., prompt change) shows up as a scenario failure or a cost delta.
- Reports are machine-readable (JSON) and human-readable (Markdown).

**Rollback:** opt-in target; no production impact.

---

### EP-18 — Headless CLI and ACP server (14 days, 1.0)

**Goal:** one core, more clients.

**Files**
- New: `src/headless/main.cpp`, `src/headless/args.{h,cpp}`, `src/harness/acpserver.{h,cpp}`, `packaging/` install rules.
- Modify: `CMakeLists.txt` (new executable), `src/CMakeLists.txt` (core visibility), `packaging/*` (ship binary), `README.md`.
- Tests: `tests/test_headless_cli.cpp` (spawn), `tests/test_acpserver.cpp` with a mock client.
- Docs: `docs/HEADLESS.md`, `docs/ACP_SERVER.md`.

**Steps**

1. `kateai-headless run`: workspace, config, prompt, permission/sandbox, max turns/cost, JSONL events, patch artifact, exit codes.
2. Non-interactive permissions: `ask` fails with code 4; `accept-edits`/`auto` apply policy; `bypass` requires an explicit flag + hardline.
3. Verification gate enabled by default; `--no-verify` opt-out.
4. Trace/patch/summary artifacts in an output dir.
5. ACP server: `--stdio`, session→conversation mapping, permission/terminal/fs handlers, `_kateai/*` methods, capability negotiation.
6. Packaging: install the binary; CI artifact.
7. Tests: a headless scenario matches the plugin's event sequence for the same fixture.

**Acceptance**
- `kateai-headless run` solves the C++ fixture and exits 0 with a patch; failure exits 2.
- Permission denied exits 4 without prompting.
- An ACP client can create a session, send a prompt, approve a tool, and read the result.
- The same scenario via plugin and headless produces equivalent event types and files.

**Rollback:** executable is additive; ACP server opt-in.

---

### EP-19 — Hardening and docs (continuous, 8 days)

- Migration rehearsal on real session databases (v3 → v2 schema) with backups.
- Fuzz the event JSON, policy parser, patch parser, skill frontmatter.
- Performance pass: event append, context assembly, tool search, UI coalescing.
- Accessibility/theme pass for the inspector.
- Docs: `docs/POLICY.md`, `HOOKS.md`, `SKILLS.md`, `PROMPT.md`, `EVALS.md`, `HEADLESS.md`, `ACP_SERVER.md`, migration guide.
- Update `ARCHITECTURE.md` and `README.md`; changelog per release.
- Package/release checks (Fedora/Arch/Debian, macOS/Windows build notes).

---

## 5. Sprint plan

Two-week sprints, one developer. With two developers, run the "Track B" items in parallel.

| Sprint | Phase | Track A (core) | Track B (parallel) | Demo |
| --- | --- | --- | --- | --- |
| 1 | 0.6.1 | EP-0 foundation, EP-1 drift (part 1) | — | Flags + tests; dead tool removed |
| 2 | 0.6.1 | EP-1 drift (part 2), QW result budget, prompt tail move | — | `/context` + `/usage`; MCP clipped; stable prefix hash test |
| 3 | 0.7 | EP-2 event schema + store + append/read | EP-7 prompt skeleton | Events visible in a test |
| 4 | 0.7 | EP-2 migration + projection + replay | EP-7 section migration | Replay a failed turn |
| 5 | 0.7 | EP-3 controller + middleware interface | EP-5 policy schema/matchers | Golden trace parity |
| 6 | 0.7 | EP-3 port budgets/stuck/permission/checkpoint | EP-5 evaluation/scopes | Legacy vs new parity |
| 7 | 0.7 | EP-4 verification gate | EP-6 hooks config/trust | Failing fixture goes green |
| 8 | 0.7 | EP-4 evidence/`/verify`; EP-16 trace MVP | EP-6 hook execution/stop | `Stop` hook blocks completion |
| 9 | 0.8 | EP-7 prompt profiles/cache control | EP-10 ToolSpec + conversion | Cost delta on 50 turns |
| 10 | 0.8 | EP-8 shapers 1–2 + content store | EP-10 search/deferral | Page a 1 MB tool output; 100-tool MCP |
| 11 | 0.8 | EP-8 summaries/rehydration/calibration | EP-9 memory scopes | 200-turn recall test |
| 12 | 0.8 | EP-8 `/context`; EP-14 profiles | EP-9 write flow; EP-15 context tab | Inspector MVP |
| 13 | 0.9 | EP-13 AgentSpec + sidechain + result | EP-12 skills loader/trust | Summary-only subagent |
| 14 | 0.9 | EP-13 questions + worktree isolation | EP-12 skill tool/first-party skills | Worktree patch apply |
| 15 | 0.9 | EP-15 trace/cost/rewind; EP-11 edits | EP-12 fork skills; docs | Rewind/fork demo |
| 16 | 0.9 | Hardening; eval T2 suite | EP-17 injection corpus | Release 0.9 gate |
| 17 | 1.0 | EP-18 headless core | EP-17 live matrix | Headless fixture run |
| 18 | 1.0 | EP-18 ACP server | EP-16 OTLP/usage | ACP client demo |
| 19 | 1.0 | Perf, fuzz, migration rehearsal | Docs | Release candidate |
| 20 | 1.0 | Bugfix, package checks | — | 1.0 gate |

**Milestones**

- **M0 (end sprint 2):** 0.6.1 — quick wins.
- **M1 (end sprint 8):** 0.7.0 — substrate.
- **M2 (end sprint 12):** 0.8.0 — context/tools.
- **M3 (end sprint 16):** 0.9.0 — extensibility/agents.
- **M4 (end sprint 20):** 1.0.0 — platform.

Each milestone is a release gate (Section 10), not just a date.

---

## 6. File-level change map

### New

```
src/harness/
  harnessflags.{h,cpp}
  harnesscore.{h,cpp}
  events.{h,cpp}  eventstore.{h,cpp}  projections.{h,cpp}  replay.{h,cpp}
  turn.{h,cpp}  middleware.{h,cpp}  middleware/*.cpp
  checks.{h,cpp}  policy.{h,cpp}  policyrules.{h,cpp}  audit.{h,cpp}
  hooks.{h,cpp}  hooksconfig.{h,cpp}
  prompt.{h,cpp}  promptprofiles.{h,cpp}  promptsections/*.cpp
  contextengine.{h,cpp}  shapers/*.cpp  contentstore.{h,cpp}  tokens.{h,cpp}
  memory.{h,cpp}  memorywrite.{h,cpp}
  registry.{h,cpp}  tools/search.{h,cpp}  tools/bridge.cpp  tools/skill.cpp
  toolresult.{h,cpp}
  skills.{h,cpp}  skilltrust.{h,cpp}
  agents.{h,cpp}  subagentrun.{h,cpp}  worktree.{h,cpp}  sidechain.{h,cpp}
  models.{h,cpp}  requestshaping.{h,cpp}
  telemetry.{h,cpp}  cost.{h,cpp}  otlp.{h,cpp}
src/headless/main.cpp  args.{h,cpp}
src/harness/acpserver.{h,cpp}
src/inspectorpanel.{h,cpp}  contextview.{h,cpp}  traceview.{h,cpp}  harnessreport.{h,cpp}
resources/models.json
docs/POLICY.md  HOOKS.md  SKILLS.md  PROMPT.md  EVALS.md  HEADLESS.md  ACP_SERVER.md
tests/test_harness_*.cpp
tests/evals/{run.py,scenarios/,fixtures/,scripted_provider.py,report.py}
tests/golden/prompts/
```

### Modified (major)

```
src/CMakeLists.txt            add harness sources/executables
CMakeLists.txt                headless executable
src/agentloop.{h,cpp}         orchestration only; delegate to turn/prompt/context/registry
src/contextmanager.*          legacy adapter
src/sessionstore.*            schema 2, events, projections, blobs
src/permissions.*             adapter over policy
src/rules.*                   adapter over memory
src/tools.{h,cpp}             specs, envelopes, budgets, edits
src/types.{h,cpp}             tool definitions from registry; settings cleanup
src/modes.*                   groups from specs; skills/profile refs
src/mcp.*                     deferred specs; result budget
src/llmclient.cpp             request shaping/cache/usage
src/acpclient.cpp             usage, command wiring, server role
src/pluginview.cpp            inspector tab/actions; editor context to tail
src/chatwidget.cpp            commands, projection, trace/cost
src/configpage.cpp            experimental group, policy/hook/skill status
src/sandbox.*                 protected paths, matchers, hardline, checks
```

### Removed

- `goal_report` (or fully wire it; recommendation: remove).
- `skills/system/run-workflow/SKILL.md` (replace in EP-12).
- Unused settings listed in EP-1.
- Legacy policy/context/prompt code paths one release after their flags default on.

---

## 7. Data and configuration migrations

### 7.1 Session database

| Step | Change | Safety |
| --- | --- | --- |
| Schema 1 → 2 | Add `session_event`, `message_projection`; add `parent_id`, `fork_seq`, `head_seq` to `conversations` | Copy DB to `sessions.sqlite.bak-<version>` before migration; `quick_check` before and after |
| Backfill | Synthesize events from the payload messages; `imported: true` | Verify projection equals payload; on mismatch, keep legacy read-only and report |
| Payload | Keep v3 through 0.7; bump to v4 when events are the only write path (0.8) | Document that downgrading below 0.8 loses events but keeps payload |
| Prune | Blobs GC; events pruned only with conversation deletion | Never delete events for an existing conversation |

**Test:** open a copy of a real 0.6 database, migrate, restart, replay a turn, compare messages.

### 7.2 Settings

- `configVersion()` 1 → 2 in EP-1: remove unused keys, add `[Harness]` flags, add `[Checks]`/`[Policy]`/`[Hooks]`/`[Skills]` paths.
- Migration writes a backup of the old group (`LegacyRemoved`) for one release, then drops it.
- No setting is required for the new features; defaults are on where safe, off where experimental.

### 7.3 New configuration files

- `.kateai/policy.json` (project), `~/.config/kateai/policy.json`, `/etc/kateai/policy.json`.
- `.kateai/hooks.json` (project), user/managed.
- `.kateai/skills/<name>/SKILL.md`, `.agents/skills`, `.claude/skills`.
- `.kateai/config.json` `verify` array.
- `.kateai/harness.json` (non-security flags).
- `AGENTS.md`/`CLAUDE.md`/`KATEAI.md` remain the instruction files.

All are optional; absence means defaults. Invalid files produce a warning and are ignored (except a managed policy error, which fails closed).

### 7.4 Rollback rules

- Every flag off restores the previous behaviour within one release.
- Migrations are additive and never destructive; a backup exists.
- A newer payload/schema is never opened read-write by an older build; the session is left untouched with a notice (existing behaviour).
- Worktrees are cleaned on disable/crash at startup (`git worktree prune` for the cache dir).

---

## 8. Test plan

### 8.1 Per-work-package tests

| WP | New tests | Key cases |
| --- | --- | --- |
| EP-0 | `test_harness_flags` | defaults, env precedence, unknown flag |
| EP-1 | `test_permissions`, `test_tools`, `test_settings` | hardline, deny-first, write conflict, MCP clip, migration |
| EP-2 | `test_harness_events`, `test_harness_migration`, `test_harness_replay` | order, torn write, projection rebuild, redaction, replay |
| EP-3 | `test_harness_turn`, `test_harness_middleware`, `test_stopbehavior` | transitions, short-circuit, retry, stale results |
| EP-4 | `test_harness_checks`, `test_verification_gate` | discovery, failure feedback, timeout, cap |
| EP-5 | `test_harness_policy`, `test_harness_audit` | scopes, deny-first, symlinks, review, redaction |
| EP-6 | `test_harness_hooks` | trust, exit 2, timeout, updatedInput, stop |
| EP-7 | `test_harness_prompt` | golden per profile, stable prefix, budgets |
| EP-8 | `test_harness_context`, `test_harness_contentstore` | shapers, paging, merge, rehydration, calibration |
| EP-9 | `test_harness_memory`, `test_memorywrite` | scopes, nested, paths, dedupe, approval |
| EP-10 | `test_harness_registry`, `test_harness_toolsearch`, `test_toolresult` | deferral, search, direct call, budgets, concurrency |
| EP-11 | `test_edits`, `test_patch` | whitespace/Unicode/CRLF, ambiguity, atomicity |
| EP-12 | `test_harness_skills`, `test_skilltrust` | catalog, invocation, gate, scan, fork |
| EP-13 | `test_harness_agents`, `test_worktree`, `test_subagent_events` | summary-only, questions, isolation, cost, cancel |
| EP-14 | `test_harness_models` | profile lookup, request golden, fallback |
| EP-15 | `test_harnessreport`, `test_inspector` | manifest, trace, rewind, fork |
| EP-16 | `test_harness_cost`, `test_harness_telemetry` | pricing, cache, spans, redaction |
| EP-17 | `test_harness_eval` | scenario runner, assertions, injection |
| EP-18 | `test_headless_cli`, `test_acpserver` | exit codes, JSONL, ACP session, permission |

### 8.2 Cross-cutting suites

- **Golden prompt tests:** hash the static prefix; assert unchanged when only editor context changes; per-profile snapshots.
- **Replay determinism:** record 20 turns; replay; compare event types, tool names/args, final files.
- **Injection suite:** web/MCP/file/ACP/skill payloads; assert no unapproved egress, no out-of-workspace read, audit event present.
- **Permission matrix:** every tool × every mode × every policy decision; table-driven.
- **Fuzz:** event JSON, policy/hook/skill frontmatter, patch parser.
- **Soak:** 500-turn synthetic session; memory/event growth bounded; UI responsive (offscreen).
- **Performance:** context assembly <30 ms p95; event append <2 ms p95; tool search <5 ms/500 tools; prompt render <10 ms.

### 8.3 CI layout

| Job | When | Contents |
| --- | --- | --- |
| `linux-build-test` | every PR | configure, build, ctest (existing) |
| `linux-harness` | every PR | new `test_harness_*`, eval scripted suite |
| `macos-build` | every PR | compile + core tests (existing) |
| `windows-build` | every PR | compile (existing) |
| `nightly-models` | nightly | T3 live matrix, record artifacts |
| `weekly-bench` | weekly | optional benchmark adapter, trend report |

No live API keys in PR jobs; use scripted/mock providers. Nightly keys are repository secrets with spend caps.

---

## 9. Metrics and release gates

### 9.1 Primary metrics

- `success@1` on the scripted suite (must not regress).
- `cost_usd` per solved scenario (target −10% at 0.8, −20% at 1.0).
- `tokens_per_solved_task` (−10% / −20%).
- `turns_per_solved_task` (no regression; ideally −10%).
- `permission_prompts_per_task` (−25% via better rules/auto-approve).
- `verification_first_pass` (≥70% on fixtures).
- `cache_hit_rate` (≥60% on provider-cached models).
- `crash_free_sessions` (≥99.5% in soak).
- `replay_success_rate` (100% for recorded turns).

### 9.2 Gates

| Release | Gate (all must pass) |
| --- | --- |
| 0.6.1 | Existing 55 tests green; new flags/prompt/context tests; no dead tool/command; 50-turn cost baseline recorded |
| 0.7.0 | Replay 100% on 20 recorded turns; verification fixture; policy/hook tests; migration from a real 0.6 DB; trace export |
| 0.8.0 | 100-turn recall test; 100-tool MCP overhead <10%; prompt cost −10% at equal success; token calibration ±15% |
| 0.9.0 | Skill/hook/policy demo; worktree apply; injection suite zero escapes; inspector manifest ≈100% of tokens |
| 1.0.0 | Headless + ACP end-to-end; 3-model matrix; cost −20%; no P1 security issues; docs complete; migration guide tested |

A gate failure blocks the release; fix or cut scope (never cut the security gate).

---

## 10. Risks and mitigations

| Risk | Trigger | Mitigation | Owner |
| --- | --- | --- | --- |
| Event migration loses sessions | any `loadFailed` after migration | backup, verify projection, legacy read-only fallback, rehearsal on real DBs | core |
| Middleware refactor changes behaviour | golden trace diff | port one policy per PR, parity flag, property tests | core |
| Prompt regression on a model | eval success drop | per-profile flags, kill switch, model matrix | prompt |
| Cache changes increase cost | `/usage` cache rate down | measure per provider, provider-specific boundary | prompt |
| Tool search hides a tool | scenario failure "unknown tool" | core tools always loaded, direct call works, aliases, eval | tools |
| Hooks/skills are an attack surface | injection test failure | trust cards, scan, sandbox, managed override | security |
| Worktree breaks (submodules/LFS/disk) | preflight failure | fallback to in-process, never auto-merge | agents |
| UI stalls on event volume | frame time >16 ms | coalesce batches, worker thread, virtualise list | UI |
| Scope creep | new settings/features outside plan | freeze new settings; delete one per release; roadmap review | lead |
| Timeline slip | milestone miss | cut Phase 3/4 scope before Phase 1 safety; ship flags off | lead |
| Provider/ACP churn | CI breakage | adapters + capability flags; pin mock versions | integrations |

---

## 11. First 10 working days

A concrete start, assuming the plan is approved.

**Day 1**
- Create `harness/foundation` from `feat/state-of-the-art-harness`.
- Add `src/harness/` skeleton, `HarnessFlags`, CMake entries, `test_harness_flags`.
- Add `KATEAI_HARNESS_FLAGS` docs.

**Day 2**
- Remove `goal_report` (or file a ticket to wire it; default remove).
- Remove `skills/system/run-workflow/SKILL.md`; note the replacement in EP-12.
- Fix `ARCHITECTURE.md` names.

**Day 3**
- `write_file` staleness check + test.
- MCP result clipping + test.
- Hardline floor + deny-first tests.

**Day 4**
- Settings cleanup + config version 2 migration test.
- Wire or stop advertising ACP commands (pick one).
- Fix subagent question/permission stall (forward + answer).

**Day 5**
- Define `Event`/`EventStore` interfaces; implement in-memory store + tests.
- Add `test_harness_events` target.

**Day 6**
- SQLite append/read transaction + projection.
- Migration skeleton + backup.

**Day 7**
- Backfill from payload; projection verification; test on a real DB copy.
- Emit the first events from `AgentLoop` (turn start/end, tool call/result).

**Day 8**
- Replay MVP (recorded tool calls) + test.
- `PromptAssembler` interface + section list; move editor context to a tail message behind a flag.

**Day 9**
- Golden prompt test; stable-prefix hash test.
- Measure tokens/cost on a 50-turn fixture and record the baseline.

**Day 10**
- Demo: `/context`, stable prefix, event log, replay, drift fixes.
- Retro: update estimates; open the sprint-3 tickets.

**Definition of success for the first 10 days:** a user can see why the context is full, the static prompt no longer changes every turn, a failed turn can be replayed, and no advertised capability dead-ends.

---

## 12. Open decisions (need maintainer sign-off)

1. **Event store:** SQLite tables (recommended) vs per-conversation JSONL + index. Affects EP-2 and export.
2. **Payload version:** keep v3 through 0.7 (recommended, allow downgrade) vs bump immediately.
3. **`goal_report`:** remove (recommended) vs wire into a "goal" mode.
4. **ACP slash commands:** build the composer menu (recommended) vs stop advertising them.
5. **Auto-review:** opt-in `review` rules only (recommended) vs a default reviewer for high risk.
6. **Memory writes:** separate `.kateai/memory/` store (recommended) vs direct `AGENTS.md` edits.
7. **Worktree location:** cache dir (recommended) vs `.kateai/worktrees` inside the repo.
8. **Minimum Qt/KF:** keep 6.5 (current) vs require a newer LTS for `QPromise`/features.
9. **Release cadence:** 0.6.1 + 0.7 + 0.8 + 0.9 + 1.0 (recommended) vs fewer, larger releases.
10. **Eval budget:** internal scenarios first (recommended) vs a Terminal-Bench adapter early.

---

## 13. Appendix: ticket template and acceptance examples

**Ticket template**

```
ID: H-<epic>-<n>
Title: <imperative>
Epic/Release: EP-x / 0.x
Context: <why; link to roadmap section>
Files: <paths>
Steps:
  1. ...
Acceptance:
  - [ ] ...
Tests:
  - [ ] unit ...
  - [ ] integration ...
Flags: <name, default>
Migration: none | DB | settings | config
Docs: <changelog/readme/architecture>
Estimate: <days>
Dependencies: <tickets>
Risk: low|medium|high
```

**Example acceptance (verification gate, H-04-1)**

- [ ] A repo with a failing CTest suite cannot complete a mutating turn on the first attempt.
- [ ] The failure output is appended to the conversation once, and the model is asked for a targeted fix.
- [ ] A passing second attempt completes with `verification: passed` evidence in the final message.
- [ ] With `maxVerificationAttempts=0`, the gate is disabled and the legacy nudge is used.
- [ ] A check that exceeds the timeout is killed; the user sees `timed out after Ns`.
- [ ] Checks run under the active sandbox; a network-using check fails in `Strict`.
- [ ] `ctest -R verification_gate` and the C++ fixture pass.

**Example acceptance (tool search, H-10-4)**

- [ ] With 100 MCP tools, the request's tool-schema tokens are <10% of the window.
- [ ] `tool_search("create issue")` returns the GitHub issue tool in the top 3.
- [ ] A direct call to a deferred tool executes through the normal permission pipeline.
- [ ] A deferred tool denied by policy is absent from search results.
- [ ] The static prompt prefix hash is unchanged after discovery.
- [ ] `test_harness_toolsearch` covers ranking, aliases and the empty case.

---

*This plan is deliberately executable, not aspirational. If a work package cannot meet its acceptance criteria within its estimate, cut scope or split it; do not lower the gate.*
