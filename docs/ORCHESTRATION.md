# Model Orchestration

Two-model planner/executor orchestration for API-key providers, modelled on the
Antigravity CLI (`agy`) Orchestrator. One stronger (usually more expensive)
model plans, reviews and repairs the plan. One cheaper model does the searches,
edits, builds and tests. Both models can come from different providers, but
both providers must be reachable through a stored API key. ACP agents and
Telegram are deliberately out of scope.

This document started as the implementation analysis and now also records the
shipped implementation. The design below is the one in `AgentLoop`: a single
loop that switches between a read-only planner, the normal executor, and a
read-only planner review. The open questions in section 17 were resolved as
recommended there; the remaining sections are kept as rationale and test
guidance.

---

## 1. What the feature does

Orchestration is not a router that sends one prompt to one model. Both models
work in the same user turn and hand control back and forth:

```text
User request
    |
    v
Planner (read-only tools)
    |  submit_plan
    v
Executor (full tools: read, edit, bash, build, debug, tests)
    |  request_replan  <--------------------+
    v                                      |
Planner repairs the plan ------------------+
    |
    v
Executor finishes
    |
    v
Planner review (read-only, optional)
    |  submit_review(approved=false) -> Executor fixes -> review again
    |  submit_review(approved=true)  -> final answer
    v
Done
```

Rules carried over from Antigravity:

- Every user turn starts with a read-only planning phase.
- If the request is a question or needs no change, the planner answers directly
  and the executor never runs.
- The executor follows the plan. It does not silently redesign the solution.
- Control goes back to the planner when the executor hits real friction: a
  verification step fails twice, an interface behaves differently than the
  plan assumed, or a design decision was not settled.
- The planner reviews the actual result before the turn is declared done.
- One agent conversation, one transcript, one permission/checkpoint system;
  only the model behind each step changes.

---

## 2. Current code: what already works in our favour

### 2.1 Provider and model selection is already per-request data

`Settings` stores a provider enum plus one model string per API-key provider,
and the matching API key in `CredentialStore`:

- `Settings::provider`, `Settings::grokModel`, `Settings::openaiModel`, ...
  (`src/types.h`, around the provider fields).
- `providerBaseUrl(const Settings &)`, `apiKeyFor(const Settings &)`,
  `modelFor(const Settings &)` in `src/types.cpp`.
- `Settings::provider` is just an enum. Switching a route means copying
  `Settings`, changing `provider`, and setting that provider's model field.
  Nothing in the transport is tied to one provider for the lifetime of a loop.

`LlmClient::doComplete()` (`src/llmclient.cpp`) reads the provider, key, base
URL, model, API format and auth headers at the moment `complete()` is called.
That means a per-step route can be implemented by setting the client's
`Settings` immediately before `complete()`. No new HTTP transport is needed.

### 2.2 The agent loop is already a single reusable harness

`AgentLoop` owns:

- messages, transcript, compaction, token calibration;
- tool registry, MCP tools, permissions, sandbox, checkpoints;
- sub-agents, skills, hooks, middleware, event log, session state;
- the model-step state machine (`sendToModel`, `onFinished`, `processQueue`,
  `executeOne`, `finishTurn`).

A second model inside the same loop therefore reuses the whole safety and
workspace layer. A separate pair of loops would have to duplicate or proxy all
of it through `ChatWidget`, which talks to one `AgentLoop` directly.

### 2.3 Prompt, tools and context already accept provider/model as parameters

- `PromptRequest` carries `provider` and `modelName`
  (`src/harness/prompt.h`), and `PromptAssembler::assemble()` picks the prompt
  variant, context window and edit dialect from them.
- `LlmClient::advertisedTools()` and `ToolAccess` decide the request's tool
  schema; `AgentLoop::syncToolAccess()` rebuilds the registry.
- `AgentLoop::m_messagesForRequest()` chooses the context window through
  `ModelProfiles::contextWindow(provider, model)` and compacts per request.
- `ModelProfiles` already knows context windows, cache styles, tool-schema
  dialects and prices per provider/model.

So per-role prompt/tool/context shaping is a matter of passing the active
route into the existing builders, not inventing a second harness.

### 2.4 Existing per-agent model override is not enough

`AgentProfile` supports `model`, and `applyModelOverride()` writes that model
into the current provider's field (`src/agentloop.cpp`). It cannot switch
provider, and sub-agents get an isolated context with a summary-only return.
That is useful for delegation, but it is not the continuous planner/executor
handoff this feature needs.

### 2.5 The existing "Orchestrator" mode is a different feature

`src/modes.cpp` defines a built-in `orchestrator` mode that is read-only and
delegates work through `new_task`. It coordinates sub-agents. Model
orchestration coordinates two models in the same loop. The two can coexist,
but the shared word will confuse users unless the UI distinguishes them:

- Feature name in the model picker: **Orchestrator** (or "2-model").
- Existing mode label: recommended to become **Team** or **Delegate** while
  keeping the id `orchestrator` for configuration compatibility.

### 2.6 What is missing

- No concept of a model route independent of `Settings.provider`.
- No role in the model-step state machine.
- No role in `ChatMessage`, event payloads, permission cards or usage.
- No per-role context window, summary, token calibrator or rate bucket.
- No way to advertise different tool sets per step.
- Settings, session payload and inspector assume one active model.
- `ChatWidget` renders one model chip and one provider/model picker.
- Model selection is global and `start()` aborts if busy; there is no mid-turn
  user steering. (Antigravity supports sending a message while the executor
  works. v1 should not attempt that; Stop then Send is the existing contract.)

---

## 3. Goals and non-goals

### Goals

1. Enable a user to choose a planner route and an executor route, each with its
   own provider, model and reasoning effort.
2. Support different providers in the pair, using the existing API keys.
3. Planner is read-only by construction, not only by prompt.
4. Executor runs the existing full tool loop, permissions, checkpoints and
   verification.
5. Automatic handoff on friction, with a bounded number of replans.
6. Optional planner review of the result.
7. One transcript, one turn id, one cost report, one session restore.
8. Clear failure modes and a clean fallback to the single-model loop.
9. No ACP, no Telegram, no new credential storage.

### Non-goals (v1)

- Parallel planner and executor streaming.
- Mid-turn user messages.
- Planner calling MCP tools or sub-agents.
- Two models editing the workspace simultaneously.
- Automatic routing of arbitrary single prompts by task classification.
- Conversation rewind; existing checkpoint restore remains files-only.
- Cross-provider continuation of a live ACP session.

---

## 4. Architecture options

### Option A: single `AgentLoop`, per-step model role (recommended)

Keep one `AgentLoop` and one message history. Add an `OrchestrationState` that
decides which role runs next. Every model request uses the active route's
provider/model/prompt/tools/context window. Planner tools and internal handoff
tools are intercepted before normal tool execution.

**Pros**

- Reuses permissions, sandbox, checkpoints, MCP, skills, hooks, middleware,
  event log, transcript, sub-agents, session store and all reports.
- `ChatWidget` keeps its single `AgentLoop` connection surface.
- Role switches are just model steps; the state machine already runs one model
  request at a time.
- Cross-provider history already works: canonical `ChatMessage` objects are
  serialised per provider and hidden reasoning is never replayed.

**Cons**

- Several `AgentLoop` methods assume one global model. They must become
  role-aware (prompt, tools, context window, calibration, rate bucket,
  counters, usage).
- A shared history means the executor sees the planner's read-only tool calls
  and the planner sees the executor's work. That is mostly desirable, but it
  costs context; see §7.4.

### Option B: planner as a separate child `AgentLoop`

Run a read-only planner loop for each plan, return a summary, then start or
resume the executor loop.

**Pros**

- Clean context separation, separate budgets, no model-role changes inside the
  loop.
- Reuses the sub-agent machinery.

**Cons**

- The planner cannot see the live conversation unless it is re-briefed every
  time; the executor cannot ask a quick question without a serialization layer.
- Two loops need a controller that proxies start/stop/questions/permissions/
  sessions, and `ChatWidget` currently drives one `AgentLoop` directly.
- Session restore and transcript ordering become harder.

### Option C: two full loops with a facade

Planner and executor loops live in a new `Orchestrator` class that mimics the
`AgentLoop` interface to `ChatWidget`.

**Pros**

- Strong isolation and independent contexts.

**Cons**

- Largest change: many `ChatWidget` signals and methods
  (`start`, `abort`, `resolvePermission`, `answerQuestion`, `sessionData`,
  `restoreSession`, reports, checkpoints, MCP, modes) would need forwarding.
- Duplicate workspace graph/tools/checkpoints or share them through a third
  owner. High regression risk.

### Decision

**Option A.** It is the smallest correct change for this codebase and preserves
the safety/approval architecture. The planner remains genuinely read-only and
the executor remains the only writer.

---

## 5. Data model

### 5.1 `ModelRoute`

Add to `src/types.h` (and helpers in `src/types.cpp`):

```cpp
struct ModelRoute {
    Provider provider = Provider::Grok;
    QString model;
    QString effort;      // empty = provider default
    bool isValid() const { return provider != Provider::Acp && !model.trimmed().isEmpty(); }
    QString key() const; // providerId + ':' + model, for caches and rate buckets
};

struct OrchestrationSettings {
    bool enabled = false;
    ModelRoute planner;
    ModelRoute executor;
    int maxReplans = 3;
    int maxPlannerSteps = 12;
    int maxReviewRounds = 2;
    // "auto" reviews only when files changed or the plan has several steps.
    QString reviewMode = QStringLiteral("auto"); // auto | always | off
    bool showPlannerActivity = true;
    // v1 recommendation: fail fast; see §14.
    bool fallbackToExecutor = false;
};
```

Add `OrchestrationSettings orchestration;` to `Settings`.

Route helpers:

```cpp
bool providerSupportsOrchestration(Provider provider);
// Acp/Telegram rejected; key must be present for the selected provider;
// model must be non-empty; the provider must speak an API format.
bool modelRouteUsable(const Settings &settings, const ModelRoute &route,
                      QString *error = nullptr);
Settings settingsForRoute(const Settings &base, const ModelRoute &route);
QList<Provider> orchestrationProviders(const Settings &settings);
```

`settingsForRoute()` is the existing switch from `modelFor()` wrapped in a copy:

```cpp
Settings copy = base;
copy.provider = route.provider;
switch (route.provider) { /* set that provider's model field */ }
copy.reasoningEffort = route.effort; // when non-empty
return copy;
```

`providerSupportsOrchestration()` is:

```cpp
return provider != Provider::Acp && provider != Provider::Telegram;
```

API-key enforcement uses the existing `apiKeyFor(copy).trimmed()` check. This
also means OpenAI-compatible local endpoints need a non-empty key, which matches
the current `providerIsSelectable()` behaviour; document it.

### 5.2 `OrchestrationState` inside `AgentLoop`

```cpp
enum class OrchestrationRole { Single, Planner, Executor, Reviewer };

struct OrchestrationPlan {
    QString summary;
    QStringList steps;
    QStringList files;
    QStringList acceptance;
    QStringList replanIf;
    bool needsChanges = true;
    QString directAnswer;
    QString hash;             // detects an unchanged replan
    int version = 0;
};

struct OrchestrationState {
    bool active = false;
    OrchestrationRole role = OrchestrationRole::Single;
    ModelRoute planner;
    ModelRoute executor;
    OrchestrationPlan plan;
    int plannerSteps = 0;
    int replans = 0;
    int reviewRounds = 0;
    int plannerNudges = 0;
    bool reviewPending = false;
    bool planWasProduced = false;
    QString lastReplanReason;
    QString executorSummary;
    QStringList lastErrors;
};
```

`OrchestrationState` is snapshotted at the start of a turn. A settings change
while a turn runs applies to the next turn, never mid-handoff.

### 5.3 A pure policy class

Add `src/orchestration.{h,cpp}` (core library) for logic that does not need the
network:

- route validation and route-key construction;
- planner/executor/reviewer prompt fragments;
- internal tool JSON schemas (`submit_plan`, `request_replan`,
  `submit_review`);
- plan/review JSON parsing and validation;
- plan formatting for the transient directive;
- plan hash;
- "should review?" decision;
- replan reason thresholds.

This keeps `AgentLoop` from growing another 1,000 lines and gives the logic
cheap unit tests.

---

## 6. Turn state machine

Add orchestration phases on top of the existing `WaitingForNextModel` →
`WaitingForModel` → tool execution loop. No new `TurnState` enum value is
strictly needed; the UI can use a new signal for the role. The model-step
state machine remains:

```text
start()
  -> Planner
       read tools -> ... -> submit_plan
       no changes -> direct answer -> finish
  -> Executor
       normal tool loop
       request_replan / friction -> Planner (plan vN+1) -> Executor
       done + review on -> Reviewer
       done + review off -> finish
  -> Reviewer
       submit_review(approved) -> finish
       fixes -> Executor -> Reviewer
```

### 6.1 Start

In `AgentLoop::start()` after the existing turn reset:

1. Snapshot `m_settings.orchestration` into `m_orchestration`.
2. If `usesAcpNative(m_settings)`, do not orchestrate; existing ACP path.
3. Validate both routes. If invalid, disable orchestration for this turn and
   emit a notice with the exact reason. Never fail the user's turn merely
   because the optional second model is misconfigured.
4. If plan mode or the active mode is read-only, run the planner only: after
   `submit_plan`, finish with the plan/direct answer. There is nothing for an
   executor to do safely.
5. Otherwise set `role = Planner` and schedule the first model step.

The system message stored in `m_messages[0]` stays the executor/base prompt.
Planner and reviewer prompts are applied only to their request copies. This
avoids rewriting history and keeps the transcript stable.

### 6.2 Per-step request preparation

`AgentLoop::sendToModel()` becomes:

```cpp
const OrchestrationRole role = activeRole();
const ModelRoute route = routeForRole(role);
const Settings requestSettings = settingsForRoute(m_settings, route);
const ModeDefinition roleMode = modeForRole(role, activeMode());
const ToolAccess access = accessForRole(role);
const QJsonArray definitions = definitionsForRole(role);

m_client.setSettings(ModeRegistry::settingsForMode(requestSettings, roleMode));
m_client.setToolAccess(access);
m_client.setToolDefinitionsOverride(definitions);
m_client.setExtraToolDefinitions(role == Planner ? QJsonArray{}
                                                 : mcpToolDefinitions());

// rate limit per route key, not per loop
if (rateLimitDelayMs(now, route.key()) > 0) { scheduleNextModelStep(); return; }

const QList<ChatMessage> request = m_messagesForRequest(role);
// insert transient role directive / plan / handoff
m_client.complete(request);
```

Important details:

- `m_client.setSettings()` is enough; `LlmClient` already reads provider, key,
  model and URL at `doComplete()` time. Retries reuse the route because the
  client keeps that Settings until the next step.
- `m_client.isBusy()` is checked by `canStartModelRequest()`; there is only one
  in-flight request per loop.
- `m_activeRole`, `m_activeRoute`, `m_lastRequestRole` and
  `m_lastRequestRoute` are set for the duration of the request. All signals
  (`textDelta`, `thinkingDelta`, `usageReceived`, `finished`, `failed`) are
  attributed to that route.
- On `finishTurn()`/`abort()`/`finishWithFailure()`, clear the active role but
  leave the client on the last route; the next `sendToModel()` sets it again.
  A manual retry therefore retries the same model.

### 6.3 Planner finish

`onFinished` branches on `m_activeRole`.

Planner with tool calls: execute them normally (read-only access enforces the
contract). `submit_plan` is the only terminal call.

Planner with no tool calls:

1. Try the existing `parsePlanFromText()` as a fallback.
2. If it yields a plan, accept it and move to the executor.
3. If not, nudge once with a controller message: "Use submit_plan; do not
   answer with prose unless no change is needed."
4. If it still produces prose and the mode is read-only or the request is a
   question, accept it as a direct answer and finish.
5. Otherwise finish with a clear planner failure.

### 6.4 Executor finish

Reuse the existing `onFinished` body, with one insertion before `finishTurn()`:

```cpp
if (runStopHooks()) return;                 // existing
if (runVerificationGate()) return;          // existing
if (needsExecutorReview()) { startReview(); return; }
finishTurn();
```

`needsExecutorReview()` is true when:

- orchestration is active, and
- `reviewMode == "always"`, or `reviewMode == "auto"` and there were file
  changes or the plan has at least three steps, and
- `reviewRounds < maxReviewRounds`.

Review is skipped for direct answers, read-only modes, and turns with no
changes and no plan.

### 6.5 Replan triggers

`requestReplan(reason)` is called from:

1. The executor's `request_replan` internal tool.
2. `runVerificationGate()` when a verification retry would be issued after the
   configured attempt limit (the second failure is the Antigravity analogue).
3. Repeated-action recovery after the second warning.
4. `m_repeatedTruncatedObservationCount >= 2` (the executor is reading the
   wrong thing and needs a new approach).
5. Three consecutive failed tool calls, tracked in `appendToolResult()`.
6. The executor explicitly says it is blocked and calls `request_replan`.

On trigger:

- If `replans >= maxReplans`, finish with a partial-result failure message; do
  not loop. Preserve the executor's summary and changed files.
- Otherwise set `role = Planner`, increment `replans`, and append a transient
  handoff packet (not stored) containing:
  - the current plan and version;
  - the trigger reason and the last 5-10 tool result summaries (ok/fail);
  - changed files and last verification output;
  - the executor's most recent progress text.
- The planner may use read tools, call `question`, or call `submit_plan`.
- A new plan increments the version. If the plan hash is unchanged, the
  executor resumes with a "continue with the same plan" directive and one
  additional replan is consumed; this prevents a planner/executor ping-pong.
- A planner that wants a user decision calls `question`; the turn parks in the
  existing `WaitingForQuestion` state.

### 6.6 Review finish

Reviewer with `submit_review`:

- `approved=true`: finish the turn. Use the executor's final summary as the
  answer; if it was empty, use the review summary.
- `approved=false`: if fixes remain and `reviewRounds < maxReviewRounds`,
  append a transient review-fixes directive, set `role = Executor`, and resume.
  Otherwise finish with the executor summary plus a warning that the planner
  did not approve.
- `ask_user`: emit a normal question and park the turn.

Reviewer with prose only: accept a leading `APPROVED` / `approved` token or a
`## Fixes` list as a fallback; otherwise nudge twice, then finish with a
warning. The primary contract is the tool.

---

## 7. Prompts, tools and context

### 7.1 Role prompts

The planner/reviewer prompt is the normal base prompt plus a role section.
The executor prompt is the normal base prompt plus the executor role section.
Keep role text out of the stored system message; generate it per request.

Planner role text (abridged):

```text
ORCHESTRATION ROLE: PLANNER

You are the planning model in a two-model session. You may inspect the
workspace with read-only tools. You cannot edit files, run commands, change
state, or delegate.

Resolve the user's intent and inspect only what the plan needs. If the request
is a question or needs no code change, answer it directly. Otherwise produce a
concrete plan: ordered steps, exact files/symbols, acceptance checks, and the
conditions that should return control to you.

Do not implement the plan. Do not leave design decisions to the executor.
Prefer 5-15 verifiable steps. Call submit_plan exactly once when ready.
```

Executor role text (abridged):

```text
ORCHESTRATION ROLE: EXECUTOR

A planner model owns the design. The current plan is in <orchestration_plan>.
Follow it. You may search, edit, run commands, build, debug and verify. Keep the
planner's decisions; do not redesign the solution.

If a plan step is impossible, a verification failure repeats, an interface
behaves differently than the plan assumed, or a design choice is missing, call
request_replan with the evidence. Otherwise finish with a summary of changes
and verification.

Update the plan checklist with todo_write as steps complete. Do not claim
success without evidence.
```

Reviewer role text (abridged):

```text
ORCHESTRATION ROLE: PLANNER REVIEW

Review the executor's actual result against the plan and its acceptance checks.
Inspect files or evidence with read-only tools. Approve only when the checks
pass and verification evidence exists. Otherwise return concrete fixes.
Call submit_review.
```

### 7.2 Internal tools

These are advertised only to their role and intercepted by `AgentLoop` before
policy/permission/tool execution. They never touch the workspace.

`submit_plan` (planner only):

```json
{
  "summary": "one paragraph",
  "needs_changes": true,
  "steps": [{"description": "...", "files": ["..."], "verification": "..."}],
  "acceptance": ["..."],
  "replan_if": ["..."],
  "direct_answer": "required when needs_changes is false"
}
```

`request_replan` (executor only):

```json
{
  "reason": "short category: verification_failed | plan_gap | unexpected_interface | design_decision",
  "what_failed": "evidence",
  "attempted": ["..."],
  "question": "optional question for the planner"
}
```

`submit_review` (reviewer only):

```json
{
  "approved": false,
  "summary": "why",
  "fixes": [{"description": "...", "files": ["..."], "verification": "..."}],
  "ask_user": "optional"
}
```

Implementation notes:

- Add the schemas in `orchestration.cpp`; append them to the role's advertised
  definition array, not to `ToolRegistry`. This avoids changing the built-in
  tool count, parity tests and executor schemas.
- Add the names to the role's `ToolAccess` so the generic access check allows
  them.
- Intercept them in `executeOne()` before `describeTool()` and policy. Verify
  the active role; a wrong-role call gets a tool error, not a state change.
- A controller tool must be the only call in its batch (no queued siblings and
  no in-flight tools). Otherwise append a tool error asking the model to submit
  alone. This prevents a plan from being accepted while edits are still
  running.
- Append a normal tool result for the call so the provider's tool-call/result
  pairing stays valid.
- Emit a `toolStarted`/`toolFinished` only if the UI should show a card. The
  recommendation is no generic card; use `planUpdated` and a role badge
  instead.

### 7.3 Tool access per role

Planner/reviewer access is a deliberate safe subset:

```cpp
ToolAccess readOnly = ModeRegistry::toolAccessForGroups({ToolGroup::read(), ToolGroup::web()});
```

Then subtract tools that do not belong in a planning context:
`exit_plan_mode`, sub-agent tools, and (v1) `skill` and MCP. Keep:
`read_file`, `read_tool_result`, `list_dir`, `grep`, `glob`,
`query_project_graph`, `todo_read`/`todo_write` (checklist), `think`, LSP
inspection, debugger inspection, `build_targets`, `web_search`, `web_fetch`,
`question`, `suggest`, plus the role's internal tool.

Executor access is exactly `activeToolAccess()` for the active mode, plus
`request_replan`.

This is enforced twice: the request advertises only these tools, and
`executeOne()` rejects anything outside the role's access. The planner cannot
write even if a compromised provider invents a `write_file` call.

### 7.4 Context strategy

v1 recommendation: **shared canonical history**, with role-specific request
views and transient directives.

- `m_messages` remains the canonical tool-call/result history. This preserves
  provider pairing rules, checkpoint evidence and session restore.
- `m_transcriptMessages` gets an `authorRole` tag on each message so the panel
  can render planner/executor/reviewer cards and group them.
- The current plan is **not** stored in the system prompt. It is injected as a
  transient user message for the executor, and as a transient handoff packet
  for the planner. This keeps the role's static prefix and prompt cache stable.
- The planner request can carry a compact evidence digest rather than every
  executor tool result. Start with the full history for correctness; add a
  `ContextOptions.audience` filter in a follow-up if planner cost is too high.
- Context windows are per role:
  `m_messagesForRequest(role)` uses `ModelProfiles::contextWindow(route.provider,
  route.model)` (or the user's override).
- Context summaries are per role: replace the single `m_contextSummary` with
  `QHash<QString, QString> m_contextSummaryByRole`, keyed by `route.key()`.
  `updateState` writes to the active role's entry.
- `maybeCompactStoredHistory()` must not adopt a compaction that only the
  smaller-window role needed. Compute the shared threshold from the smaller of
  the planner/executor windows, or skip adoption while orchestration is active.
  Per-request compaction still runs for each role.
- Token calibration is per route: `QHash<QString, Tokens::Calibrator>`.
- `lastRequestEstimate` and the usage callback must use
  `m_lastRequestRoute`, not `m_settings.provider/model`.

### 7.5 Prompt caching

- Planner and executor have different system prompts and tool sets, so each
  role has its own cached prefix. Switching roles naturally misses the other
  role's cache; that is acceptable.
- Keep plans, evidence and editor state as transient turn messages, never in
  the system prompt. A replan then does not invalidate the executor's cache.
- Gemini native cache resources are single-slot in `LlmClient`. With two Gemini
  roles the cache is rebuilt on every role switch. A follow-up can replace the
  four cache fields with a small LRU map keyed by route; not required for v1.
- Anthropic cache breakpoints already cover the per-role system/tool prefix.

---

## 8. UI and UX

### 8.1 Settings

Add a group to **Settings → AI Providers**:

```text
Model Orchestration (API providers only)

[ ] Enable two-model orchestration

Planner  (heavy reasoning, read-only)
  Provider [ OpenAI        v ]   Model [ gpt-5.5        v ] [Fetch]
  Effort   [ High          v ]

Executor (edits, commands, tests)
  Provider [ DeepSeek      v ]   Model [ deepseek-v4    v ] [Fetch]
  Effort   [ Medium        v ]
  [ ] Use the current chat model as executor

Replans per turn        [ 3 ]
Planner steps per plan  [ 12 ]
Review                  [ Auto (changes or 3+ steps) v ]
Show planner activity   [x]
```

Validation rules:

- Both providers must be in `orchestrationProviders()` and have a non-empty
  key. ACP appears only as a disabled hint ("ACP agents cannot be combined").
- Both models must be non-empty.
- Planner and executor may be the same model, but show a hint that this is a
  single-model turn with extra overhead.
- Enabling with an invalid route keeps the feature off and shows the reason.

Store the whole block as one JSON entry `Orchestration` (config version 3), or
as individual entries. One JSON entry is easier to grow and copy between
machines; typed fields remain in `Settings` for the rest of the code.

### 8.2 Model picker

Add one row at the top of the existing model popup:

```text
Orchestrator   planner → executor
```

- Selecting it enables orchestration and leaves the executor route as
  configured. A gear action opens the orchestration settings group.
- If no valid pair is configured, the row is disabled with "Configure planner
  and executor…".
- Selecting any ordinary model disables orchestration. That matches the
  "select a model to run every step on one model" behaviour in Antigravity.
- The model chip shows `Orchestrator · <planner> → <executor>` and a tooltip
  with providers, models, efforts, replans and review mode.

### 8.3 Transcript and status

New signal:

```cpp
void orchestrationPhaseChanged(OrchestrationRole role, const QString &provider,
                               const QString &model, const QString &reason);
```

UI treatment:

- A small phase separator/pill before role changes:
  `Planner · OpenAI gpt-5.5`, `Executor · DeepSeek deepseek-v4`,
  `Planner review`.
- `m_currentPlan` continues to drive the checklist. Planner `submit_plan`
  populates it; executor `todo_write` updates it.
- Planner/reviewer tool cards get a role badge and are collapsed by default
  under the phase separator. `showPlannerActivity=false` hides them entirely
  while keeping the plan.
- Planner thinking is shown under the planner pill if thinking display is on;
  otherwise it stays collapsed with the planner group.
- Executor final answer is the user-facing answer. A review badge says
  "Reviewed by planner" when approved.
- Usage display: the composer token counter uses the executor window (the model
  doing the work) and optionally shows `P 12% · E 41%`.
- `:kateai usage` gains a per-role block; `:kateai context` and
  `:kateai prompt` report the active role or both manifests.
- Inspector Cost/Trace gain planner/executor/reviewer fields; no new tab is
  required.

### 8.4 Questions and permissions

Add `originRole` (and optionally `originModel`) to `PermissionRequest`. Cards
show `Planner`/`Executor` so a question from the planner is not mistaken for
the executor's. The existing intent dock and queued-approval logic continue to
work because there is still one loop and one tool-call id namespace.

### 8.5 Cancellation

Stop cancels the in-flight role and resets orchestration. It never leaves the
executor running after the planner was cancelled. The existing
`appendCancelledToolResults()` repair is still needed and still works.

---

## 9. Persistence

### 9.1 Settings

- `SettingsStore::configVersion()` becomes `3`.
- Load `Orchestration` as a JSON object; on any parse error, leave it disabled
  and log a warning. Do not reject the rest of the configuration.
- Save it as compact JSON in the existing `[KateAI]` group.
- Add round-trip and v2→v3 migration tests in `test_configmigration.cpp`.

### 9.2 Session

Add to `SessionStore::SessionData`:

```cpp
QJsonObject orchestration; // active, role, plan, replans, versions, route keys
```

- Bump `SessionStore::CURRENT_VERSION` to 5. Additive: version 4 payloads load
  with orchestration disabled.
- Persist the plan, plan version, replan/review counts, last role and route
  keys. Do not persist transient directives or the planner's hidden reasoning.
- On restore, mark the turn completed/interrupted as today; do not auto-resume
  a half-finished model step. The next user turn starts a fresh planner phase
  with the restored plan available as prior context.
- Add `ChatMessage.authorRole` (`None/Planner/Executor/Reviewer`) to the stored
  message JSON so transcript role badges survive a restart. Old payloads
  default to `None`.

### 9.3 Event log and inspector

`emitHarnessEvent()` payloads gain role/provider/model on:

- `StepStart`
- `RequestHeaders`
- `AssistantMessage`
- `ToolCall` / `ToolResult`
- the usage `Notice`
- `Compaction`

Add event types for:

- `orchestration/plan` (version, summary, step count)
- `orchestration/handoff` (reason, replan index)
- `orchestration/review` (approved, fix count)

The event log stays append-only and replayable. No API keys or raw prompts are
written.

---

## 10. Safety and privacy

- Planner cannot mutate. Enforced by role `ToolAccess` and runtime rejection,
  not only by prompt text.
- Planner/reviewer may read any file the normal read tools can read, including
  outside the workspace only when `allowReadsOutsideWorkspace` is on. The
  existing confinement applies.
- Web content remains inside `<untrusted_web_content>`; planner summaries do
  not make it trusted.
- The plan is model-generated input to the executor. It is not a permission
  grant: every edit/command still goes through the existing policy, sandbox,
  approval card and checkpoint.
- Both providers receive the context needed for their role. A plan or evidence
  digest may contain source code. The settings UI should state that the
  workspace context is sent to both selected providers. This is the same
  trust boundary as choosing either provider alone, but now doubled.
- API keys are already in `CredentialStore`; no new key field and no key in
  JSON, logs, events or the plan.
- Sub-agents and skill forks must not recursively orchestrate. Set
  `childSettings.orchestration.enabled = false` in `startSubtask()` and
  `startSkillFork()`. The child should inherit the executor route as its base
  provider/model so a sub-agent spawned during an executor step is not sent to
  the unrelated `Settings.provider`.

---

## 11. Rate limits, retries, budgets and cost

### 11.1 Rate limiting

Replace the single `QQueue<qint64> m_modelRequestTimes` with:

```cpp
QHash<QString, QQueue<qint64>> m_modelRequestTimesByRoute;
```

`rateLimitDelayMs(now, routeKey)` and the enqueue in `sendToModel()` use the
active route key. Same provider for both roles shares one bucket (correct);
different providers get independent buckets. `requestsPerMinute` remains the
per-provider cap.

### 11.2 Retries and failures

`LlmClient` already retries transient errors with the route's Settings. When it
eventually fails:

- include the role, provider and model in the user-facing error:
  `Planner (OpenAI / gpt-5.5) failed: …`;
- do not silently switch to the executor. A planner failure means the turn has
  no plan. v1 recommendation: fail with an actionable message and a hint to
  disable orchestration. A future `fallbackToExecutor` can degrade gracefully
  with a visible notice, but it hides quality regressions and should be opt-in.
- Auth/model-not-found errors are never retried as transient and never fall
  back.

### 11.3 Budgets

- `maxModelRequests` and `maxToolCalls` remain the global turn ceilings and
  count both roles. The UI must say "model requests (planner + executor)".
- Add per-role soft caps for reporting and guardrails:
  `maxPlannerSteps` (default 12) and `maxPlanSteps` for the plan length.
- Planner loops: if `plannerSteps` reaches the cap and no `submit_plan` has
  arrived, append one controller nudge; on the next step fail the planner phase
  with a clear message.
- Replans and review rounds have their own caps (`maxReplans`, default 3;
  `maxReviewRounds`, default 2). They are counted per user turn.
- Tool calls made by the planner count against `maxToolCalls`; read tools are
  cheap but not free.

### 11.4 Usage and cost

Add per-role accumulators:

```cpp
struct RouteUsage { int input = 0, output = 0, cached = 0; double cost = 0.0; };
QHash<QString, RouteUsage> m_usageByRoute;
```

`usageReceived` attributes to `m_lastRequestRoute`. `usageReport()` prints a
planner/executor breakdown plus totals. `ModelProfiles` supplies the prices.
The existing total counters remain for compatibility and quick display.

---

## 12. Interaction with existing features

| Feature | Behaviour under orchestration |
|---|---|
| Plan mode | Planner only; return the plan as the answer. No executor. |
| Read-only modes (Ask/Architect) | Planner only. Executor is skipped. |
| Built-in `orchestrator` mode | Works, but the executor is the read-only delegation loop. Consider hiding the combination in the UI. |
| Sub-agents (`new_task`) | Allowed if the mode allows; children run on the executor route with orchestration disabled. |
| Skills (`context: fork`) | Child fork on the executor route, orchestration disabled. Inline skills work normally. |
| MCP | Executor only in v1. Planner discovery of MCP tools is a follow-up. |
| Checkpoints | Executor mutations create the normal pre-turn/pre-edit checkpoints. Planner never triggers one. |
| Verification gate | Executor only; a repeated failure is a replan trigger. Reviewer sees the evidence. |
| Hooks | Run for executor tools as today. Add role to hook payloads; planner read tools may run hooks too. |
| ACP | Feature disabled. Existing ACP session path untouched. |
| Telegram/other non-chat providers | Not selectable for either route. |
| Completion model | Independent; unaffected. |
| Voice | Independent; unaffected. |

---

## 13. File-by-file implementation plan

### Phase 0 — configuration and data model (behind a flag)

- `src/harness/harnessflags.{h,cpp}`: add `orchestration` flag, default false
  until the integration tests are green; flip to true in the release PR.
- `src/types.h/.cpp`: `ModelRoute`, `OrchestrationSettings`, route helpers,
  `providerSupportsOrchestration()`, API-key/provider validation.
- `src/settings.h/.cpp`: config version 3, JSON load/save, validation.
- `src/sessionstore.h/.cpp`: version 5, orchestration payload, message role tag.
- `tests/test_settings.cpp`, `tests/test_configmigration.cpp`,
  `tests/test_sessionstore.cpp`: round-trip and migration.

### Phase 1 — pure orchestration policy

- New `src/orchestration.h/.cpp`:
  - prompt fragments;
  - internal tool JSON schemas;
  - `submit_plan`/`submit_review`/`request_replan` argument parsing;
  - plan formatting and hashing;
  - review decision and replan thresholds.
- `src/CMakeLists.txt`: add the files to `kateai_core`.
- New `tests/test_orchestration.cpp`: no network, no GUI.

### Phase 2 — request shaping inside `AgentLoop`

- `src/agentloop.h/.cpp`:
  - `OrchestrationState`, active role/route, role helpers;
  - `settingsForRoute` applied before `m_client.complete`;
  - `m_messagesForRequest(role)`, per-role summary/calendar, extra transient
    plan/handoff/review messages;
  - `systemPromptForRole()` / `assemblePromptForRole()`;
  - role tool access/definitions and internal tool interception;
  - per-route rate buckets, calibrators, usage and cost;
  - role in event payloads, permission requests and reports.
- `src/harness/prompt.{h,cpp}`: optional `role` field if the assembler needs to
  suppress executor-only sections for the planner. Keep the default path
  byte-identical.
- `tests/test_agentparity.cpp`, `test_harness_prompt.cpp`,
  `test_harness_context.cpp`, `test_providers.cpp`: guard that single-model
  prompts/tools/requests are unchanged when orchestration is off.

### Phase 3 — planner/executor/reviewer state machine

- `src/agentloop.cpp`:
  - `start()` planner entry;
  - `onPlannerFinished`, `onExecutorFinished`, `onReviewerFinished`;
  - `submit_plan`, `request_replan`, `submit_review` handling;
  - `requestReplan()` triggers;
  - `startReview()`, review fixes loop, caps;
  - direct answers and plan-mode shortcut;
  - cancellation/reset paths.
- New `tests/test_orchestration_flow.cpp` with two mock HTTP endpoints.
- Extend `tests/mock_llm_server.py` or add `tests/mock_orchestration_server.py`.

### Phase 4 — UI

- `src/configpage.{h,cpp}`: orchestration group, provider/model/effort combos,
  fetch buttons, validation, apply/reset.
- `src/chatwidget.{h,cpp}`:
  - model picker Orchestrator row and gear action;
  - chip/tooltip;
  - `orchestrationPhaseChanged` handling;
  - role badges and grouping in the transcript;
  - token display and `:kateai usage` link.
- `src/permissionbar.cpp` / card widgets: role badge.
- `src/inspectorpanel.cpp`: role fields in Cost/Trace.
- Widget tests: `test_inspector`, `test_toolcard`, `test_livecards`,
  `test_transcriptlayout`.

### Phase 5 — documentation and release

- `docs/ORCHESTRATION.md` (this document) and README section.
- `docs/HARNESS_0.10.md` or release notes when the flag flips.
- `:kateai usage`, context, prompt and inspector examples.
- Real-provider smoke tests on at least one cross-provider pair.

---

## 14. Test plan

### Unit

1. `providerSupportsOrchestration`: ACP/Telegram rejected; all API-key
   providers accepted.
2. `modelRouteUsable`: missing key, empty model, ACP rejected; valid pair
   accepted; OpenAI-compatible local endpoint with empty key rejected.
3. Settings JSON round-trip, unknown keys, malformed JSON falls back safely,
   v2→v3 migration.
4. Session v4→v5 load, message role defaults, orchestration plan restore.
5. Plan/review/tool argument parsing: valid, missing fields, too many steps,
   empty steps, direct answer, malformed JSON.
6. Plan hash/version logic.
7. Review decision: auto vs always vs off; no-change turn skips review.
8. Replan threshold logic; caps; unchanged-plan detection.
9. Rate bucket keying: same provider shares, different providers do not.

### Integration (mock providers)

Use two loopback servers with distinct models and request logs.

1. **Happy path**: planner read → submit_plan; executor edit → verify → finish.
   Assert the planner saw only read tools + submit_plan, the executor saw edit
   tools, and the plan was injected before the executor's first request.
2. **Direct answer**: planner submit_plan(needs_changes=false); executor is
   never called; final answer is the direct answer.
3. **Cross-provider**: planner on server A, executor on server B; assert
   `Authorization`/`x-api-key`, base URL and model per request.
4. **Replan on verification**: executor's first verification fails twice;
   planner receives a handoff with the failure, returns plan v2; executor
   resumes; final approved.
5. **Replan cap**: planner/executor keep failing; turn stops after
   `maxReplans` with a partial summary and no infinite loop.
6. **Review loop**: reviewer rejects once, executor fixes, reviewer approves;
   assert review round count and final answer.
7. **Review rejection cap**: reviewer never approves; turn stops after
   `maxReviewRounds` with a warning.
8. **Planner failure**: planner endpoint returns 401; turn fails with a
   role/provider/model-specific message; executor never runs.
9. **Plan mode**: only planner runs; no edit tool advertised.
10. **Permission/question**: executor permission card carries `originRole`;
    planner question parks and resumes.
11. **Stop while streaming**: stop during planner and during executor; no
    stale role continues.
12. **Budget**: `maxModelRequests` counts both roles; the closing summary is
    still requested when the tool budget is hit.
13. **Session restore**: save during review, reload, transcript shows role
    badges and plan; next turn starts a fresh planner phase.
14. **Sub-agent inheritance**: executor spawns a sub-agent; child uses executor
    provider/model and has orchestration disabled.

### Regression

- All existing 72 tests must stay green.
- Prompt/tool/request bytes for the single-model path must be unchanged
  (golden/hash tests).
- ACP tests untouched.
- `test_agentparity` covers single-model behavior.

---

## 15. Acceptance criteria

1. A user can enable orchestration, pick a planner and executor from two
   different API-key providers, and run a normal coding turn.
2. The planner request never advertises write/command tools; an invented write
   call is rejected.
3. A planner plan is visible as the checklist and is injected into the executor.
4. A verification failure after the configured threshold returns control to
   the planner, which can update the plan; the executor resumes.
5. The review phase can approve or return concrete fixes; it is bounded.
6. `:kateai usage` reports planner and executor tokens and cost separately.
7. Session restore preserves the plan, phase and role-tagged transcript.
8. With orchestration off, the single-model path is byte-for-byte unchanged in
   prompts/tools and all existing tests pass.
9. ACP and Telegram cannot be selected for either role; enabling with a
   provider without a key fails with a clear notice and leaves the turn usable.
10. Stop cancels the active role and never leaves the other role running.

---

## 16. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Shared history grows quickly and the executor/planner overflow | Per-role context windows and summaries; transient plan/handoff instead of stored plan prose; compact evidence digest as a follow-up. |
| Cross-provider replay rejects orphan tool calls | Only switch roles after the queue is empty and all results are flushed; controller tools must be alone in a batch; existing `appendCancelledToolResults` repair remains. |
| Planner quietly writes through a delegated tool | Read-only `ToolAccess` plus runtime rejection; no sub-agents/MCP in planner v1. |
| Prompt cache thrash | Stable role prompts; plan as transient message; accept one miss per role switch; optional per-route Gemini cache map later. |
| Cost blow-up from a chatty planner | `maxPlannerSteps`, plan step cap, replan cap, explicit cost report, planner prompt says stop exploring once actionable. |
| Infinite planner/executor ping-pong | Replan cap; unchanged-plan hash detection; reviewer fix cap. |
| User changes model mid-turn | Snapshot routes at `start()`; settings apply next turn. |
| Sub-agent recursion | Disable orchestration in children and skill forks. |
| Naming collision with the existing Orchestrator mode | Rename the mode label to Team/Delegate in the UI; keep the id. |
| Provider outage | Fail with role/provider/model context; optional future fallback. |

---

## 17. Recommended defaults and open decisions

### Recommended defaults

- `enabled = false` until the user opts in.
- Planner effort: high; executor effort: provider default/medium.
- `maxReplans = 3`, `maxPlannerSteps = 12`, `maxReviewRounds = 2`.
- `reviewMode = auto` (changes or 3+ plan steps).
- Planner: read-only tools, no MCP, no sub-agents, no skills.
- Executor: normal mode tools, sub-agents allowed, orchestration disabled in
  children.
- Planner failure: fail the turn with a precise message; do not silently
  degrade. A future opt-in fallback can be added.
- Same model for both roles: allowed, with a warning that it adds overhead
  without adding capability.

### Decisions needed before implementation

1. **Naming**: use "Orchestrator" in the model picker and rename the existing
   mode label to "Team"? Or call the feature "Dual-model" to avoid touching the
   mode UI?
2. **UI placement**: model-picker pseudo-model (recommended) versus a separate
   composer toggle/popover.
3. **Planner context**: shared history v1 (recommended for correctness) versus
   a compact evidence digest from day one (cheaper, more work to validate).
4. **Review default**: auto (recommended), always, or off.
5. **Planner failure fallback**: fail fast (recommended) or executor-only
   degradation with a notice.
6. **Planner MCP**: exclude in v1 (recommended) or allow read-only MCP tools.

---

## 18. Suggested PR sequence

1. **PR 1 — data model + settings**: routes, validation, config v3, session v5,
   tests. No behaviour change.
2. **PR 2 — policy module**: prompts, internal tool schemas, parsing, hashing,
   review/replan decisions, tests.
3. **PR 3 — request shaping**: per-role settings/prompt/tools/context/rate/
   usage inside `AgentLoop`, still single-model unless the flag is on.
4. **PR 4 — state machine**: planner/executor/reviewer transitions, submission
   interception, replan triggers, caps, mock-provider integration tests.
5. **PR 5 — UI**: settings group, model picker, phase UI, role badges,
   inspector/report changes, widget tests.
6. **PR 6 — hardening + docs**: real-provider smoke tests, cost tuning,
   provider-outage behavior, README/docs, flag default flip.

Each PR should keep the flag off until PR 4's integration suite is green; PR 5
can ship behind the flag in a beta build; PR 6 flips it on for the release.
