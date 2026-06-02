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
- Paths: `installDir`, `tempDir`, `updaterExecutable`.
- Network policy: timeouts, TLS, resumable downloads.
- Security policy: signatures, public key, URL allowlist, anti-downgrade, expired manifest rejection.
- Apply policy: process wait timeout and healthy-confirmation timeout.

## Manifest

`Manifest` represents a server release manifest. `files[]` describes the complete managed file set of the target version. The downloader only fetches files that are missing locally or whose SHA-256 differs.

`ManifestFile::path` is the server path. `ManifestFile::localPath` is an optional installation path. Content-addressed storage relies on this separation.

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

## Threading Contract

- `Updater` methods may be called from the application main thread.
- A single `Updater` instance serializes its internal state transitions.
- Callbacks are not guaranteed to run on the UI thread; the dispatcher decides.
- Callers should avoid long blocking work inside callbacks.
- `cancel()` is cooperative and may not immediately interrupt a low-level system call.

## ABI Note

The project is currently in the `0.x` phase and does not promise a stable ABI. Prefer CMake package or source integration, and rebuild when upgrading.
