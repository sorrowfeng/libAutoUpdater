# libAutoUpdater

`libAutoUpdater` is a planned production-oriented C++17 online update library for desktop applications on Windows, macOS, and Linux.

The project is designed around static-file release hosting: an application can publish manifests and release files to any HTTP/HTTPS server without running a custom backend.

> Status: architecture and implementation plan are being established. The detailed design lives in [docs/architecture-plan.md](docs/architecture-plan.md).

## Goals

- Cross-platform desktop self-update support.
- No GUI framework dependency in the core library.
- Static manifest and release file hosting over HTTP/HTTPS.
- File-level incremental updates based on SHA-256.
- Safe self-replacement through an external updater process.
- Rollback support for failed apply operations.
- Optional manifest signature verification.
- Testable core logic with injectable network, hash, filesystem, signature, process, dispatcher, and state-store interfaces.

## High-Level Architecture

```text
Application
  |
  v
Updater Facade
  |
  v
Update Orchestrator
  |
  +-- ManifestFetcher
  +-- ManifestVerifier
  +-- LocalSnapshotBuilder
  +-- UpdatePlanner
  +-- DownloadExecutor
  +-- ApplyPlanWriter
  +-- ApplyLauncher
  |
  v
Infrastructure Interfaces
  |
  +-- INetworkClient
  +-- IHashProvider
  +-- IFileSystem
  +-- ISignatureVerifier
  +-- IEventDispatcher
  +-- IProcessLauncher
  +-- IStateStore
```

File replacement is handled by an external updater executable:

```text
Main App
  -> libAutoUpdater checks, downloads, verifies
  -> libAutoUpdater writes apply-plan.json
  -> libAutoUpdater starts autoupdater_apply
  -> Main App exits
  -> autoupdater_apply backs up, replaces, verifies, rolls back on failure, restarts the app
```

## Planned Features

- SemVer parsing and comparison, including prerelease labels.
- Release manifests with schema versioning.
- Platform, architecture, channel, and release identity validation.
- `minVersion` support for full reinstall prompts.
- `mandatory` updates.
- Managed-file whitelist and path traversal protection.
- File-level diff planning using local SHA-256.
- Download retry, timeout, cancellation, and resumable downloads.
- Detached manifest signature support.
- Anti-downgrade and anti-replay checks.
- Transactional apply plan and updater journal.
- CLI example and optional Qt integration example.
- CMake package export for `find_package` and `add_subdirectory`.

## Planned Repository Layout

```text
include/libAutoUpdater/       Public C++ headers
src/                          Core implementation and default adapters
updater/                      External apply executable
examples/                     CLI and Qt examples
tests/                        Unit and integration tests
tools/                        Release manifest generation scripts
docs/                         Architecture, manifest, and apply-plan docs
```

## Example Manifest Shape

```json
{
  "schemaVersion": 1,
  "appId": "com.example.myapp",
  "channel": "stable",
  "platform": "windows",
  "arch": "x64",
  "version": "1.4.0",
  "releaseId": "1.4.0+20260601.1",
  "releaseDate": "2026-06-01T10:00:00Z",
  "publishedAt": "2026-06-01T10:00:00Z",
  "expiresAt": "2026-07-01T00:00:00Z",
  "minVersion": "1.2.0",
  "minClientVersion": "1.0.0",
  "mandatory": false,
  "allowDowngrade": false,
  "notes": "Fix startup crash and improve sync performance.",
  "baseUrl": "https://example.com/releases/1.4.0/windows-x64/",
  "files": [
    {
      "path": "bin/MyApp.exe",
      "sha256": "9b3f...",
      "size": 18432000
    }
  ],
  "remove": [
    "plugins/old_plugin.dll"
  ]
}
```

## Build Direction

The intended build system is CMake:

```cmake
find_package(libAutoUpdater CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

Planned options:

```cmake
LIBAUTOUPDATER_BUILD_UPDATER=ON
LIBAUTOUPDATER_BUILD_EXAMPLES=ON
LIBAUTOUPDATER_BUILD_TESTS=ON
LIBAUTOUPDATER_WITH_CURL=ON
LIBAUTOUPDATER_WITH_OPENSSL=ON
LIBAUTOUPDATER_WITH_QT=OFF
```

## Roadmap

1. CMake skeleton, public headers, error/result model, version support.
2. Manifest parsing, validation, and pure update planning.
3. Download executor, staging directory, hashing, retry, cancellation.
4. Apply plan writer and external updater executable.
5. Security features: signatures, allowed base URLs, anti-downgrade, anti-replay.
6. Examples, packaging tool, tests, and full documentation.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).

