# libAutoUpdater

`libAutoUpdater` is a production-oriented C++17 online update library for desktop applications on Windows, macOS, and Linux.

The project is designed around static-file release hosting: an application can publish manifests and release files to any HTTP/HTTPS server without running a custom backend.

The detailed architecture lives in [docs/architecture-plan.md](docs/architecture-plan.md).

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

## Features

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
- CLI example and optional Qt example.
- CMake package export for `find_package` and `add_subdirectory`.

## Repository Layout

```text
include/libAutoUpdater/       Public C++ headers
src/                          Core implementation and default adapters
updater/                      External apply executable
examples/cli/                 CLI example
examples/qt/                  Optional Qt example
tests/                        Unit tests
tools/                        Release manifest generation script
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

## Build

Configure and build:

```sh
cmake -S . -B build
cmake --build build --config Debug
```

Run tests:

```sh
ctest --test-dir build -C Debug --output-on-failure
```

CMake options:

```text
LIBAUTOUPDATER_BUILD_UPDATER=ON
LIBAUTOUPDATER_BUILD_EXAMPLES=ON
LIBAUTOUPDATER_BUILD_TESTS=ON
LIBAUTOUPDATER_WITH_CURL=ON
LIBAUTOUPDATER_WITH_OPENSSL=ON
LIBAUTOUPDATER_WITH_QT=OFF
```

Use from another CMake project after installation:

```cmake
find_package(libAutoUpdater CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

Or embed directly:

```cmake
add_subdirectory(external/libAutoUpdater)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

## Minimal Client Usage

```cpp
autoupdater::Config config;
config.appId = "com.example.myapp";
config.manifestUrl = "https://example.com/releases/1.4.0/windows-x64/manifest.json";
config.currentVersion = autoupdater::Version::parse("1.3.0").value();
config.installDir = "C:/Program Files/MyApp";
config.updaterExecutable = "C:/Program Files/MyApp/autoupdater_apply.exe";
config.restartCommand = {"C:/Program Files/MyApp/MyApp.exe"};

autoupdater::Updater updater(config);
updater.setCallbacks(callbacks);
updater.checkAndDownloadAsync();
```

Startup and periodic checks are also available:

```cpp
updater.checkOnStartupAsync();
updater.startPeriodicCheck(std::chrono::hours(6));
```

When the new application version starts successfully, call:

```cpp
updater.markCurrentVersionHealthy();
```

If a pending update should be reverted before it is marked healthy, call:

```cpp
updater.rollbackLastUpdate();
```

## Release Manifest Generation

Generate a manifest from a release directory:

```sh
python tools/make_manifest.py dist/MyApp \
  --app-id com.example.myapp \
  --platform windows \
  --arch x64 \
  --version 1.4.0 \
  --release-date 2026-06-01T10:00:00Z \
  --base-url https://example.com/releases/1.4.0/windows-x64/
```

The generated `manifest.json` can be uploaded with the release files to any static HTTP/HTTPS server.

For multi-platform release routing, generate an index manifest:

```sh
python tools/make_index.py \
  --app-id com.example.myapp \
  --channel stable \
  --generated-at 2026-06-01T10:00:00Z \
  --target windows/x64=https://example.com/releases/1.4.0/windows-x64/manifest.json \
  --target macos/arm64=https://example.com/releases/1.4.0/macos-arm64/manifest.json \
  --output index.json
```

`Config::manifestUrl` may point directly to a release manifest or to an index manifest.

Optionally sign the manifest with a detached signature:

```sh
python tools/sign_manifest.py dist/MyApp/manifest.json \
  --private-key keys/update-ed25519-private.pem \
  --algorithm ed25519
```

This writes `manifest.json.sig` as base64 text. Configure `SecurityOptions::requireManifestSignature` and embed the matching public key in the client.

## CLI Example

```sh
build/examples/cli/Debug/libAutoUpdater_cli.exe \
  --manifest file:///C:/path/to/release/manifest.json \
  --version 1.3.0 \
  --install C:/path/to/install \
  --updater build/updater/Debug/autoupdater_apply.exe
```

Without libcurl, the default network adapter supports local paths and `file://` URLs. With libcurl available, HTTP/HTTPS manifests and files are supported.

## Security Notes

- Keep TLS verification enabled in production.
- Treat manifest signing as mandatory for public update channels.
- Detached signatures may be raw binary or base64 text.
- Resumable downloads persist partial `.download` file metadata in `.autoupdater/state.json`.
- The external updater uses `.autoupdater/update.lock` as an atomic lock directory to prevent concurrent apply operations.
- When `allowedBaseUrls` is set, index-selected release manifest URLs and release `baseUrl` values must match the allowlist.
- Store the public key in the application binary or another trusted channel.
- Do not include absolute paths or `..` segments in manifests; the parser rejects them.
- Prefer staging under the install directory so replacements stay on the same filesystem.
- For package-manager-owned installs, use the system package manager rather than self-updating.

## Roadmap

1. CMake skeleton, public headers, error/result model, version support.
2. Manifest parsing, validation, and pure update planning.
3. Download executor, staging directory, hashing, retry, cancellation.
4. Apply plan writer and external updater executable.
5. Security features: signatures, allowed base URLs, anti-downgrade, anti-replay.
6. Examples, packaging tool, tests, and full documentation.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
