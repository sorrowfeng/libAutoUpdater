# libAutoUpdater

[![CI](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/ci.yml/badge.svg)](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/ci.yml)
[![CodeQL](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/codeql.yml/badge.svg)](https://github.com/sorrowfeng/libAutoUpdater/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

English | [Chinese](README.zh-CN.md)

`libAutoUpdater` is a production-oriented C++17 online update library for desktop applications on Windows, macOS, and Linux.

It is built for static-file update hosting: publish manifests and release files to any HTTP/HTTPS server, object storage bucket, CDN, or GitHub Raw endpoint. No custom backend service is required.

## Why This Project

- Cross-platform desktop self-update flow.
- GUI-independent core library for Qt, CLI, native UI, or custom shells.
- File-level incremental updates based on SHA-256.
- Safe self-replacement through an external updater process.
- Rollback support for failed apply operations.
- Optional manifest signature verification.
- HTTP/HTTPS backend abstraction with libcurl, WinHTTP, CFNetwork, and custom adapters.
- Testable architecture with injectable network, hash, filesystem, signature, process, dispatcher, and state-store interfaces.

## How It Works

```text
Application
  -> libAutoUpdater checks the manifest
  -> changed files are downloaded into a staging directory
  -> every downloaded file is verified by SHA-256
  -> libAutoUpdater writes apply-plan.json
  -> autoupdater_apply waits for the app to exit
  -> files are backed up, replaced, verified, rolled back on failure
  -> the application is restarted
```

The main application never overwrites its own running executable. All file replacement is delegated to `autoupdater_apply`.

## Quick Start

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

Run the CLI example against a local or remote manifest:

```sh
build/examples/cli/Debug/libAutoUpdater_cli.exe \
  --manifest file:///C:/path/to/release/manifest.json \
  --version 1.3.0 \
  --install C:/path/to/install \
  --updater build/updater/Debug/autoupdater_apply.exe
```

On Linux and macOS, executable paths usually live directly under `build/examples/cli/` and `build/updater/` unless you use a multi-config generator.

## Minimal Client Usage

```cpp
#include <libAutoUpdater/Updater.h>

autoupdater::Config config;
config.appId = "com.example.myapp";
config.manifestUrl = "https://example.com/updates/releases/1.4.0/windows-x64/manifest.json";
config.currentVersion = autoupdater::Version::parse("1.3.0").value();
config.installDir = "C:/Program Files/MyApp";
config.updaterExecutable = "C:/Program Files/MyApp/autoupdater_apply.exe";
config.restartCommand = {"C:/Program Files/MyApp/MyApp.exe"};

autoupdater::Updater updater(config);
updater.setCallbacks(callbacks);
updater.checkAndDownloadAsync();
```

When an update is ready, call `applyAndRestartAsync()` from your UI or command flow. When the new version starts successfully, call:

```cpp
updater.markCurrentVersionHealthy();
```

## Build Options

Important CMake options:

```text
LIBAUTOUPDATER_BUILD_UPDATER=ON
LIBAUTOUPDATER_BUILD_EXAMPLES=ON
LIBAUTOUPDATER_BUILD_TESTS=ON
LIBAUTOUPDATER_WITH_CURL=ON
LIBAUTOUPDATER_REQUIRE_CURL=OFF
LIBAUTOUPDATER_WITH_WINHTTP=ON
LIBAUTOUPDATER_WITH_CFNETWORK=ON
LIBAUTOUPDATER_WITH_OPENSSL=ON
LIBAUTOUPDATER_WITH_QT=OFF
LIBAUTOUPDATER_ENABLE_WARNINGS=ON
LIBAUTOUPDATER_WARNINGS_AS_ERRORS=OFF
LIBAUTOUPDATER_ENABLE_SANITIZERS=OFF
LIBAUTOUPDATER_ENABLE_COVERAGE=OFF
```

Useful presets:

- `dev`: default developer build with examples, tests, updater, and optional dependency probing.
- `no-optional-deps`: verifies the core library without optional HTTP or crypto backends.
- `windows-winhttp`: Windows HTTPS support through the native WinHTTP backend.
- `macos-cfnetwork`: macOS HTTPS support through CFNetwork/CoreFoundation.
- `vcpkg-debug`: uses vcpkg manifest mode for libcurl and OpenSSL.
- `release`: release-oriented local build.

## Dependency Model

The public API does not expose libcurl, OpenSSL, Qt, or JSON-library types.

Network backend selection:

- Local paths and `file://` URLs are always supported.
- libcurl is used for HTTP/HTTPS when CMake finds `CURL::libcurl`.
- Windows can use the native WinHTTP backend without libcurl.
- macOS can use the native CFNetwork/CoreFoundation backend without libcurl.
- Qt users can inject a `QNetworkAccessManager` adapter.

Applications decide how runtime dependencies are shipped. If libcurl or OpenSSL are linked dynamically, package the matching runtime libraries and license notices with the application.

## CMake Integration

After installation:

```cmake
find_package(libAutoUpdater CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

Or embed directly:

```cmake
add_subdirectory(external/libAutoUpdater)
target_link_libraries(MyApp PRIVATE libAutoUpdater::libAutoUpdater)
```

## Release Feed Generation

Generate a release manifest from a directory:

```sh
python tools/make_manifest.py dist/MyApp \
  --app-id com.example.myapp \
  --platform windows \
  --arch x64 \
  --version 1.4.0 \
  --release-date 2026-06-01T10:00:00Z \
  --base-url https://example.com/updates/releases/1.4.0/windows-x64/
```

For larger projects, avoid duplicated files across releases by using content-addressed storage:

```sh
python tools/make_manifest.py dist/MyApp \
  --content-addressed \
  --object-root publish/updates/objects/sha256 \
  --object-prefix objects/sha256 \
  --app-id com.example.myapp \
  --platform windows \
  --arch x64 \
  --version 1.4.0 \
  --release-date 2026-06-01T10:00:00Z \
  --base-url https://example.com/updates/ \
  --output publish/updates/releases/1.4.0/windows-x64/manifest.json
```

See [docs/content-addressed-storage.md](docs/content-addressed-storage.md).

## Example Manifest

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
  "baseUrl": "https://example.com/updates/releases/1.4.0/windows-x64/",
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

## Real Update Demo

This repository contains a real static update feed under `examples/update-server`. GitHub serves it through `raw.githubusercontent.com`, so the repository itself acts as the update server.

```sh
python examples/github_update_demo.py
```

The demo creates a local `1.0.0` install tree, downloads a `2.0.0` manifest from GitHub Raw, applies the update with `autoupdater_apply`, and prints the before/after file tree.

The same flow runs in CI on Ubuntu/libcurl, Windows/WinHTTP, and macOS/CFNetwork.

## Documentation

- [Getting started](docs/getting-started.md)
- [Integration guide](docs/integration.md)
- [API overview](docs/api.md)
- [Architecture plan](docs/architecture-plan.md)
- [Server layout](docs/server-layout.md)
- [Content-addressed storage](docs/content-addressed-storage.md)
- [Security model](docs/security-model.md)
- [Release process](docs/release-process.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Quality gates](docs/quality.md)
- [Ecosystem packaging](docs/ecosystem.md)

## Project Layout

```text
include/libAutoUpdater/       Public C++ headers
src/                          Core implementation and default adapters
updater/                      External apply executable
examples/cli/                 CLI example
examples/qt/                  Optional Qt example
tests/                        Unit and flow tests
tools/                        Release and packaging tools
docs/                         Design and integration documentation
```

## CI/CD

GitHub Actions covers source hygiene, clang-format, clang-tidy, GCC/Clang/AppleClang/MSVC builds, optional-dependency-off builds, ASan/UBSan, coverage, install-tree packaging, consumer `find_package` probes, CodeQL, and the real GitHub-hosted update demo.

The release workflow publishes Windows, macOS, and Linux install-tree ZIPs, SHA-256 files, SPDX SBOM files, and release notes extracted from `CHANGELOG.md`.

## Security

- Keep TLS verification enabled in production.
- Treat manifest signing as mandatory for public update channels.
- Keep update private keys outside the static file server.
- Use `allowedBaseUrls` for public channels.
- Do not include absolute paths or `..` segments in manifests.
- Prefer package managers over self-update for package-manager-owned installs.

See [SECURITY.md](SECURITY.md) and [docs/security-model.md](docs/security-model.md).

## Community

- [Contributing guide](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Code of conduct](CODE_OF_CONDUCT.md)
- [Changelog](CHANGELOG.md)

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
