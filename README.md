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

## Dependency Model

The public API does not expose libcurl, OpenSSL, Qt, or JSON-library types.
External integrations are isolated behind interfaces so applications can keep
their own dependency policy.

Network backend selection:

- `file://` and plain local paths are always supported by the default adapter.
- libcurl is used for HTTP/HTTPS when CMake finds `CURL::libcurl`.
- On Windows, if libcurl is not available and `LIBAUTOUPDATER_WITH_WINHTTP=ON`,
  the default adapter uses the native WinHTTP API.
- Qt users can provide a `QNetworkAccessManager`-based adapter without changing
  the core library API.

Signature verification is also optional. If OpenSSL is found, the default
OpenSSL verifier can be built. If not, applications can inject their own
`ISignatureVerifier` implementation or leave manifest signing disabled.

## Build

Default local build:

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
LIBAUTOUPDATER_REQUIRE_CURL=OFF
LIBAUTOUPDATER_WITH_WINHTTP=ON
LIBAUTOUPDATER_WITH_OPENSSL=ON
LIBAUTOUPDATER_WITH_QT=OFF
LIBAUTOUPDATER_ENABLE_WARNINGS=ON
LIBAUTOUPDATER_WARNINGS_AS_ERRORS=OFF
LIBAUTOUPDATER_ENABLE_SANITIZERS=OFF
```

Preset-based builds are available when using CMake 3.21 or newer:

```sh
cmake --list-presets
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Useful presets:

- `dev`: default developer build with examples, tests, updater, and automatic optional dependency probing.
- `no-optional-deps`: verifies the core library without libcurl, OpenSSL, or Qt.
- `windows-winhttp`: Windows build that disables libcurl and uses native WinHTTP for HTTPS.
- `vcpkg-debug`: uses vcpkg manifest mode to provide libcurl and OpenSSL.
- `vcpkg-release`: release-oriented vcpkg build without examples or tests.

### Using vcpkg

The repository includes [vcpkg.json](vcpkg.json) with `curl`, `openssl`, and a
pinned vcpkg registry baseline for developers who want a repeatable dependency
setup.

Install or clone vcpkg, bootstrap it, and set `VCPKG_ROOT`:

```sh
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$PWD/vcpkg
```

On Windows PowerShell:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\src\vcpkg
C:\src\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\src\vcpkg"
```

Then build through the preset:

```sh
cmake --preset vcpkg-debug
cmake --build --preset vcpkg-debug
ctest --preset vcpkg-debug
```

vcpkg is optional. Linux and macOS users can also provide curl/OpenSSL through
their system package manager and use the default CMake configure command. Windows
users can skip libcurl entirely and use the WinHTTP preset:

```powershell
cmake --preset windows-winhttp
cmake --build --preset windows-winhttp
ctest --preset windows-winhttp
```

### Runtime Packaging

Applications decide how runtime dependencies are shipped:

- If libcurl/OpenSSL are linked dynamically, package the matching runtime
  DLLs/dylibs/shared objects and their license notices with the application.
- If static libraries are used, verify the resulting license obligations and
  TLS backend configuration.
- Windows WinHTTP builds do not require shipping libcurl because WinHTTP is a
  system component.
- Package-manager-owned Linux installs should usually delegate updates to the
  package manager instead of self-replacing files.

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

## CI/CD

GitHub Actions is configured as a C++ library quality gate:

- `source-hygiene`: whitespace checks plus Python tool and manifest generation validation.
- `build-test`: GCC, Clang, AppleClang, and MSVC builds across Debug/Release and optional-dependency-off variants.
- `library-only-config`: verifies the library can be configured without updater, examples, tests, curl, or OpenSSL.
- `sanitizers`: runs ASan/UBSan on Ubuntu with Clang.
- `package-install-tree`: installs the Release build and uploads install-tree artifacts on pushes.
- `github-hosted-demo`: runs the real GitHub Raw update flow on Ubuntu/libcurl and Windows/WinHTTP.
- `CodeQL`: builds the C++ project and runs GitHub code scanning on pushes, pull requests, and a weekly schedule.
- `release`: when a `v*` tag is pushed, builds release ZIPs for Windows, macOS, and Linux, writes SHA-256 files, and publishes them to GitHub Releases.

The normal test command also includes the end-to-end update flow:

```sh
ctest --test-dir build -C Debug --output-on-failure
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

Add `--apply` to let the CLI launch the external updater after the files are
downloaded and the apply plan is ready.

The default network adapter always supports local paths and `file://` URLs.
HTTP/HTTPS is provided by libcurl when available. On Windows, if libcurl is not
available and `LIBAUTOUPDATER_WITH_WINHTTP=ON`, the default adapter uses the
native WinHTTP API instead.

## GitHub-Hosted Demo

This repository also contains a real static update feed under
`examples/update-server`. GitHub serves it over HTTPS through
`raw.githubusercontent.com`, so the repository itself acts as the update server.

Build the project with an HTTP/HTTPS backend, then run. On Windows this works
without libcurl because WinHTTP is enabled by default:

```sh
cmake -S . -B build -DLIBAUTOUPDATER_WITH_WINHTTP=ON
cmake --build build --config Debug --parallel
```

On Linux/macOS, install libcurl development files and configure with:

```sh
cmake -S . -B build -DLIBAUTOUPDATER_WITH_CURL=ON -DLIBAUTOUPDATER_REQUIRE_CURL=ON
cmake --build build --config Debug --parallel
```

Run the demo:

```sh
python examples/github_update_demo.py
```

The script creates a local `1.0.0` install tree, fetches this manifest from
GitHub:

```text
https://raw.githubusercontent.com/sorrowfeng/libAutoUpdater/main/examples/update-server/releases/2.0.0/manifest.json
```

Then it runs the CLI with `--apply`, launches `autoupdater_apply`, and prints
the file tree before and after the update. You should see:

- `bin/demo_app.txt` replaced from `version=1.0.0` to `version=2.0.0`.
- `resources/feature.txt` added.
- `legacy/remove-me.txt` removed.
- `config/settings.json` left unchanged because its SHA-256 already matches.

The `GitHub-hosted update demo` CI job also runs this flow on pushes to `main`,
using GitHub Raw as the real HTTPS update server.

## End-to-End Flow Test

The test suite includes a smoke test for the complete static-file update flow:

```sh
ctest --test-dir build -C Debug -R libAutoUpdater_e2e_flow --output-on-failure
```

That test creates a local `1.0.0` install tree, creates a `2.0.0` release tree,
generates `manifest.json`, runs the CLI to check and download only changed
files, launches `autoupdater_apply`, and verifies that replaced, added,
unchanged, and removed files all end in the expected state.

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
