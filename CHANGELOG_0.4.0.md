# KateAI 0.4.0

0.4.0 focuses on the failure modes that lose work or leak data: credentials,
session history, checkpoint restores, stale edits, process environments, and
turn lifecycle. It is a compatibility release — existing settings, sessions, and
agent workflows keep working, and the on-disk formats are migrated in place.

## Reliability

- **Session database recovery.** The SQLite store now runs
  `PRAGMA quick_check` on open, uses `synchronous=FULL` and a busy timeout, and
  quarantines a genuinely corrupted database as
  `sessions.sqlite.corrupt-<timestamp>` instead of reporting an empty history
  forever. The damaged file is kept for manual recovery. Transient
  `locked`/`busy` failures never quarantine a healthy database.
- **Schema versioning.** Session payloads are version 3 (adding the last turn
  id) and the SQLite schema records its own version. A database written by a
  newer build is left untouched with a warning rather than downgraded.
- **Safe checkpoint restores.** Restoring a checkpoint first snapshots the
  current worktree as a `Before restoring <id>` checkpoint. If that snapshot
  fails the restore fails closed. A mistaken restore can therefore be undone
  from the same checkpoint menu.
- **Stale-edit protection.** `edit_file`, `multi_edit_file`, and `apply_patch`
  now compare the content they planned against the current file/buffer before
  writing. If the user or another process changed it in between, the tool
  fails with a conflict instead of silently overwriting newer work. New-file
  hunks use an exclusive create (`QFile::NewOnly`), so a file that appears
  after planning is a conflict rather than being replaced.
- **Turn-state UI.** The status strip now reflects the stable turn vocabulary:
  starting, waiting for the model, stopping, and the existing tool-specific
  activity text.
- **File metadata preservation.** `QSaveFile` writes keep the original file
  permissions, so replacing an executable script no longer makes it
  non-executable.
- **Question and suggestion cards fixed.** Both cards now accept the field
  names and nested shapes providers actually send (`question`/`prompt`/
  `message`/`text`, `questions[]`, option objects, `suggestion`/`message`/
  `advice`, `file_path`/`start_line`). Their markdown bodies bake the theme's
  text colour and are measured after layout, so the question or suggestion is
  visible instead of a header over an empty, oversized card.
- **ACP permission race fixed.** A synchronous answer to a parked
  `fs/write_text_file` / `terminal/create` request could reset the active
  request while the permission signal was still being emitted, aborting the
  process. The request is now copied before the signal.

## Security

- **Secure credential storage.** Provider and web-search API keys move from
  the plaintext KConfig file into `CredentialStore`: KDE Wallet when available,
  otherwise an atomic owner-only `credentials.json`. Legacy
  `credentials.conf` and plaintext KConfig keys are read and migrated, and a
  failed migration leaves the legacy copy in place for a retry.
- **Atomic fallback file.** The JSON fallback is written through `QSaveFile`
  with owner-only permissions set before commit, so a crash cannot truncate all
  credentials and a permissive umask cannot expose the new file.
- **Environment sanitization.** Stdio MCP servers and ACP `terminal/create`
  commands no longer inherit ambient secrets (`*_API_KEY`, `*_TOKEN`,
  `*_PASSWORD`, provider-specific names, …). Variables a user or agent
  explicitly configures are added back deliberately.
- **OS-level shell sandbox.** Sandboxed shell and ACP terminal commands now
  run inside real OS isolation. On Linux, bubblewrap creates user, PID, IPC,
  UTS and cgroup namespaces, drops capabilities, starts a new session, mounts a
  private `/tmp`, and exposes the host read-only for the Workspace/Read-only
  profiles or a tmpfs root for Strict. Workspace and Strict get a writable
  workspace bind; Read-only gets a read-only one. Read-only and Strict also
  unshare the network namespace. The workspace is also mounted at its real
  absolute path, so commands that spell out the project directory keep working.
  Common credential stores and session sockets (SSH/GPG/AWS/cloud CLIs,
  keyrings, browser profiles, DBus, Docker/Podman, the user runtime directory)
  are masked with tmpfs/empty-file binds, and shell commands start from a
  scrubbed environment that drops secret-shaped variables and DBus, X11,
  Wayland, SSH-agent, GPG-agent, and audio-session channels. The workspace is
  never mounted writable over the host `/tmp`.
- **Fail-closed behaviour.** On a platform with no isolation mechanism,
  Read-only and Strict refuse to run a command rather than claim a confinement
  they cannot enforce. Workspace remains policy-only there and is documented as
  such; Off is the explicit unsandboxed choice.
- **macOS seatbelt profiles.** `sandbox-exec` profiles now deny credential
  stores and writes outside the workspace, and the Strict profile exposes only
  the runtime, `/etc`, temporary directories, and the workspace. macOS was not
  runtime-validated in this release.
- **Redaction fix.** The `api_key=...` redaction pattern now uses a real
  capture group, so assignment-shaped secrets are actually masked.

## Agent lifecycle

- Added a public `AgentLoop::TurnState` vocabulary (`idle`, `preparing`,
  `running`, `awaiting-model`, `awaiting-tool`, `awaiting-permission`,
  `cancelling`, `completed`, `failed`, `interrupted`) and a
  `turnStateChanged(turnId, stateId)` signal.
- Every turn gets a UUID. The id is persisted with the session, so a restored
  transcript and the live runtime agree on which turn is being described.
- Terminal states are emitted once per turn even when abort and completion race.

## Configuration and packaging

- Added `ConfigVersion` to the KateAI KConfig group. Version 0 → 1 is the
  credential relocation described above; a config from a newer release is read
  without being rewritten.
- Version metadata is consistent at 0.4.0 across CMake, plugin metadata, ACP
  client info, README, and all packaging scripts/metadata (Arch, Debian,
  Fedora, macOS, Windows, source tarball).
- Added `KATEAI_HAVE_KWALLET` CMake detection; KWallet is optional and the
  build continues without it.

## Tests

- 54/54 tests pass on Linux. New coverage:
  - `test_sandbox`: namespace/mount flag construction, private `/tmp`, scrubbed
    environment, credential-path masking, and live bubblewrap checks for
    filesystem, network, PID, HOME, and environment confinement.
  - `test_livecards` / `test_questionwidget` / `test_suggestionwidget`: alias
    and nested question shapes, restored card text, painted body content, and
    compact body heights.
  - `test_tools`: nested `questions[]` and alias `suggestion` arguments.
  - `test_credentials`: fallback permissions/atomicity, redaction, `Auto`
    override, legacy migration, and failed-wallet migration safety.
  - `test_configmigration`: `ConfigVersion` and legacy `MaxIterations`
    migration.
  - `test_sessionrecovery`: a deliberately corrupted database is quarantined,
    preserved, and replaced by a usable store.
  - `test_processenv`: secret detection, essential-variable preservation, and
    explicit re-adding.
  - `test_checkpoints`: safety snapshot contents and opt-out behaviour.
  - `test_documentbridge`: stale conditional writes, successful conditional
    writes, create-only writes, open-buffer rejection, and permission
    preservation.
  - `test_tools`: `apply_patch` refuses to overwrite a file created between
    planning and the write.
- Tests that share the test-mode KConfig and credential file are serialized in
  CTest to remove a save/load race between parallel suites.
- Test working directories are pinned to the source tree, so mock-agent suites
  pass from an out-of-tree build directory as well.

## Platform status

- **Linux:** configured, built, and tested in this environment (Qt 6.12,
  KDE Frameworks 6.32). Bubblewrap isolation was exercised by the test suite;
  this is the only validated platform.
- **macOS:** the `sandbox-exec` profile path is implemented and tightened, but
  it was not built or runtime-validated for this release.
- **Windows:** the build scripts were updated to 0.4.0 but not built or tested.
  There is no bundled OS sandbox helper; Workspace stays policy-only, and
  Read-only/Strict refuse to run.

## Upgrade notes

- No manual action is required for credentials or sessions: API keys are moved
  on first load/save, and the session database/schema migrate automatically.
- Sandboxed shell commands now use a private `/tmp` and a scrubbed
  environment. Files written under `/tmp` disappear with the command, and
  ambient provider/session variables are not inherited; add required variables
  explicitly where the tool supports it, or select **Off** for an unsandboxed
  shell.
- If a corrupt session database is found, look for
  `sessions.sqlite.corrupt-<timestamp>` next to the original and recover it
  with `sqlite3` if needed.
- KWallet is used when it is available and unlocked. Otherwise credentials are
  stored in `credentials.json` with owner-only permissions; the settings page
  reports which backend is active.
