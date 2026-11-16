# KateAI 0.4.0 Release Audit

## Scope

Audit and stabilization pass for 0.4.0, building on the existing uncommitted
0.4.0 work rather than replacing it. The goal was production readiness without
removing capabilities or breaking existing configuration, sessions, or agent
workflows.

## Baseline

- Repository: KateAI (`/home/dizeman/zed_folder/KateAI`)
- Previous declared version: 0.3.0
- New declared version: 0.4.0
- Baseline commit: `660f774964688b752ab08c8a066bbf79678b583e`
- Baseline state: build succeeded, `ctest` 52/52 passed
- Validation environment: Linux, Qt 6.12.0, KDE Frameworks 6.32.0, GCC 16.2.1

## What was found

The baseline already had a working agent runtime, provider layer, ACP/MCP
integration, permission system, editor bridge, and test suite. The highest-risk
gaps were:

1. API keys written to the main KConfig file in plaintext.
2. A damaged SQLite session database silently produced an empty history on
   every start.
3. Checkpoint restore overwrote newer work with no recovery point.
4. Read-modify-write tools could clobber a file changed between read and write.
5. MCP/ACP child processes inherited every ambient provider credential.
6. The ACP parked-permission path could abort on a synchronous reply.
7. Version metadata was stale in most packaging scripts.
8. The existing bubblewrap/sandbox-exec paths were partial: the Workspace
   profile bound the host `/tmp` writable, credential stores were only
   policy-blocked, the shell inherited the ambient session environment, and the
   isolation was not exercised by runtime tests.

## Implemented

### Credentials

- `CredentialStore` with KWallet, owner-only JSON fallback, in-memory test
  backend, and environment overrides.
- Atomic fallback writes via `QSaveFile`; owner-only directory and file
  permissions set before commit.
- Legacy `credentials.conf` and plaintext KConfig migration, with the legacy
  copy removed only after a confirmed store.
- Correct `Auto` override handling and working assignment-shaped redaction.

### Session persistence

- `PRAGMA quick_check`, `synchronous=FULL`, `busy_timeout=5000`,
  `foreign_keys=ON`.
- Corrupt databases moved to `.corrupt-<timestamp>` and replaced once; the
  damaged file is preserved.
- Lock/busy failures are treated as transient, never as corruption.
- Session payload version 3 stores the last turn id.
- Versioned SQLite schema; newer schemas are left unchanged.

### Checkpoints and editing

- Restore creates a `Before restoring <id>` safety snapshot and fails closed if
  it cannot be created.
- `changedFiles()` exposes the affected paths, with `changedFileCount()`
  delegating to it.
- `writeDocumentIfUnchanged()` and its use by `edit_file`,
  `multi_edit_file`, and `apply_patch` prevent stale overwrites.
- `writeDocumentIfMissing()` uses exclusive creation for new-file patch hunks,
  so a file that appears after planning is a conflict rather than a clobber.
- Disk writes preserve existing file permissions.
- The status strip consumes `turnStateChanged` for starting, model wait, and
  stopping phases; tool/ACP signals still supply the specific activity text.

### Agent lifecycle

- Public `TurnState` enum, `turnStateChanged` signal, per-turn UUID, terminal
  state de-duplication, and persistence/restoration of the last turn id.
- Existing internal state machine retained; the new vocabulary is additive.

### Process isolation

- `isSecretEnvironmentName()` / `sanitizedProcessEnvironment()`.
- Stdio MCP servers and ACP `terminal/create` start from a scrubbed
  environment; explicit server/terminal env entries are re-added.
- ACP's main agent process environment is intentionally unchanged so agent
  authentication keeps working.
- Sandboxed shell and ACP terminal commands run under OS-level isolation:
  - Linux bubblewrap: user/PID/IPC/UTS/cgroup namespaces, private `/tmp`,
    capability drop, new session, host read-only for Workspace/Read-only,
    tmpfs root for Strict, network namespace for Read-only/Strict, workspace
    bind at both a shallow path and its real absolute path.
  - Credential stores and session sockets are masked with tmpfs/empty-file
    binds; the shell environment drops secret-shaped variables and DBus, X11,
    Wayland, SSH-agent, GPG-agent, and audio-session channels.
  - Read-only and Strict fail closed on platforms without an isolation
    mechanism; Workspace stays policy-only there and is documented as such.
  - macOS seatbelt profiles were tightened and now deny credential stores and
    out-of-workspace writes, but were not runtime-validated here.

### Packaging and configuration

- Version 0.4.0 in CMake, plugin metadata, README, Arch, Debian, Fedora,
  macOS/Windows bundlers, and the source tarball script.
- `ConfigVersion` and version-0 credential migration in settings.

## Test results

Command:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure -j4
```

Result: **54/54 tests passed** on Linux. The same 54/54 was reproduced from
a clean out-of-tree build under `/tmp`, after pinning test working directories
to the source tree so mock-agent tests no longer depend on the build location.

New or expanded suites:

- `test_sandbox` (namespace/mount/environment construction and live runtime
  confinement checks)
- `test_credentials`
- `test_configmigration`
- `test_sessionrecovery`
- `test_processenv`
- `test_checkpoints` (safety snapshots)
- `test_documentbridge` (conditional writes, permissions)
- `test_acpclient` regression for synchronous parked-method approval

## Residual risks and follow-up work

- **Platforms:** macOS and Windows packaging was updated but not built or
  tested. Only Linux is validated.
- **KWallet:** the KWallet backend compiles and is selected when available, but
  the automated tests use the memory/config backends. A manual KWallet
  integration check is still recommended.
- **Sandbox:** Linux bubblewrap isolation is implemented and runtime-tested in
  this environment (namespaces, mounts, network, PID, HOME, environment).
  macOS `sandbox-exec` profiles compile into the command line but were not
  runtime-validated (`sandbox-exec` is also deprecated by Apple). Windows has
  no bundled OS sandbox helper: Workspace stays policy-only, and
  Read-only/Strict fail closed. No seccomp filter or cgroup resource limits are
  applied; the sandbox shares the host kernel. Deny globs for project files
  remain a command/file-tool policy plus masking of the known credential
  stores listed above; the OS layer does not enumerate every project file that
  matches an arbitrary custom glob.
- **Config fallback:** when KWallet is unavailable, credentials are protected by
  file permissions, not encryption at rest.
- **Broader UI:** the status strip consumes the main `TurnState` phases, but the
  transcript/tool-card transitions still use the existing signals. No
  behaviour was removed.

## Release readiness

The Linux build and full test suite pass, including live bubblewrap checks for
filesystem, network, PID, and environment confinement. The release is suitable
as a 0.4.0 Linux release. macOS/Windows validation and manual KWallet
verification remain open before claiming cross-platform or wallet-specific
support.
