# Public API Overview

The public API lives under `include/libAutoUpdater/`.

If Doxygen is installed locally, generate HTML API documentation with:

```sh
doxygen Doxyfile
```

The output directory is `build/docs/html`.

## Updater

`Updater` is the facade class. It is responsible for:

- Checking manifests.
- Planning updates.
- Downloading changed files.
- Writing apply plans.
- Launching the external updater.
- Periodic checks, cancellation, and rollback entry points.

`Updater` exposes asynchronous methods. Check, download, and apply scheduling run in the background. Callbacks are delivered through `IEventDispatcher`.

## Config

`Config` describes the client environment and policy:

- Application identity: `appId`, `channel`, `platform`, `arch`.
- Versions: `currentVersion`, `clientVersion`.
- Paths: `installDir`, `tempDir`, `updaterExecutable`. `tempDir` is a staging
  root; `Updater` always derives `<version>/<manifest-sha256>` beneath it so
  caller-supplied roots cannot mix artifacts or apply plans from two manifests.
  Give each installation its own root; sharing one custom `tempDir` across
  installations is unsupported.
- Network policy: connect and transfer timeouts, TLS, resumable downloads. Positive timeout values bound transport waits; zero disables the corresponding bound.
- Security policy: signatures, public key, URL allowlist, anti-downgrade, expired manifest rejection. Setting `rejectDowngrade=false` permits only downgrades requested by an independently verified release manifest; it does not make unsigned downgrade requests effective.
- Apply policy: process wait timeout and healthy-confirmation timeout. Both
  values are restricted to the inclusive range from zero to 24 hours. A zero
  `healthConfirmationTimeout` disables the deadline. Otherwise the deadline
  starts at the durable completion time recorded by the external updater;
  equality is expired. An expired pending update makes a check enter `Failed`
  and `markCurrentVersionHealthy()` fail while retaining rollback evidence.
  Successful confirmation clears only the exact pending state and retains the
  manifest-specific backup for operator-managed cleanup.

## Manifest

`Manifest` represents a server release manifest. `files[]` describes the
complete managed file set of the target version. The downloader only fetches
files that are missing locally or whose SHA-256 differs. Optional metadata
timestamps use `YYYY-MM-DDTHH:MM:SS[.1-9DIGIT](Z|+/-HH:MM)`; `expiresAt` is an
exclusive validity bound.

`ManifestFile::path` is the server path. `ManifestFile::localPath` is an optional installation path. Content-addressed storage relies on this separation.

The effective installation targets (`localPath`, or `path` when `localPath` is
empty, plus `remove[]`) must be unique under portable ASCII case-insensitive
comparison. Duplicate, replace/remove, ancestor/descendant, and reserved
`.autoupdater` targets are rejected by manifest parsing, planning, apply-plan
parsing, and the external updater boundary.

## Result and Error

The public API does not leak exceptions to callers. Failures are represented through:

- `Result<T>`
- `ErrorCode`
- `Error::message`

## Interfaces

Injectable interfaces:

- `INetworkClient`
- `IHashProvider`
- `IFileSystem`
- `ISignatureVerifier`
- `IEventDispatcher`
- `IProcessLauncher`
- `IStateStore`

Use these interfaces for tests, Qt integration, custom network stacks, and sandboxed environments.

`IStateStore::savePendingUpdate()` is create-if-absent: replaying the exact
value is idempotent, while a different existing pending value must not be
overwritten. The bundled JSON store also implements the separate
`IPendingUpdateCompareAndSet` capability. Custom stores that want `Updater` to
reconcile a completed rollback must implement that atomic capability; it is
kept outside `IStateStore` so existing implementations retain their interface
and virtual-table contract.

Download resume keys passed by `Updater` through `IStateStore` are opaque
lowercase hashes, not request URLs; custom stores must not reconstruct or
persist a credential-bearing URL in their place. The bundled JSON store
additionally uses an internal batch capability, persists advisory records in a
separate atomic `.resume` sidecar, and bounds them by release, age, entry count,
and bytes. Custom stores keep the existing per-record virtual interface and are
responsible for equivalent expiry, capacity, synchronization, and durability
policies if they persist resume metadata.

`IProcessLauncher::launch()` reports success only after the operating system has
created the requested executable image. Arguments are UTF-8 values passed
without shell interpretation; child setup, working-directory, and executable
loading failures are returned synchronously.

## Threading Contract

- `Updater` methods may be called from the application main thread.
- A single `Updater` instance serializes its internal state transitions.
- Callbacks are not guaranteed to run on the UI thread; the dispatcher decides.
- Callers should avoid long blocking work inside callbacks.
- `cancel()` is cooperative and may not immediately interrupt a low-level system call.

## ABI Note

The project is currently in the `0.x` phase and does not promise a stable ABI. Prefer CMake package or source integration, and rebuild when upgrading.
