# CHANGELOG

## Unreleased

### Changed

- `Config::tempDir` is now consistently treated as a staging root; the updater
  derives version- and manifest-specific directories beneath it.
- Healthy confirmation now enforces the configured completion deadline while
  retaining manifest-scoped rollback backups for operator-managed cleanup.
- Update planning now indexes normalized local snapshot paths, avoiding
  quadratic lookup work for large manifests.
- Download resume persistence now uses credential-free resource identities and
  one bounded atomic sidecar batch per task, with release, age, count, and byte
  pruning.
- SHA-256 now processes complete input blocks without per-byte buffering, and
  download progress consistently reports signed complete sizes during active
  and resumed transfers.
- Errors now carry an operation phase for apply, rollback, recovery, state
  persistence, and restart failures. Bundled diagnostics and transaction
  journal rewrites omit arbitrary messages and credential-bearing URLs.
- Installed packages now export the updater executable when it is built, and
  static and shared consumers are validated from a clean `find_package()`
  project. Windows shared builds produce an import library, while installed
  Unix updater helpers use a relative runtime search path.
- Release tags now require an exact successful `main` CI run, release packages
  rerun tests, publishing uses an approval environment and job-scoped write
  access, and workflow Actions are pinned to immutable revisions. SPDX SBOMs
  now describe selected HTTP/crypto components, relationships, and unique
  build provenance.
- Dependency compatibility floors are now explicit for libcurl, OpenSSL, and
  Qt, and builds can require the bundled OpenSSL verifier. That verifier now
  accepts only Ed25519 or RSA PKCS#1 v1.5/SHA-256 with RSA keys of at least
  2048 bits; CI exercises both the required-crypto and failure contracts, and
  the single-key rotation and compromise-recovery model is documented.
- Regression coverage now includes security-boundary combinations,
  transaction fault injection, state-machine concurrency, real network and
  process adapters, explicit crypto skips, a persistent libFuzzer target, and
  installed-package apply/rollback plus signed-artifact validation.
- Manifest tooling now reports the artifact base separately instead of
  incorrectly deriving a client manifest URL from it; package and feed
  documentation now matches installed targets and detached-signature names.

## v0.1.5 - 2026-06-05

### Added

- Added a Chinese GitHub Raw update demo that runs the full cloud-hosted update flow in a Chinese local install path.
- Added Unicode path unit coverage and a Chinese-path end-to-end update test.

### Changed

- GitHub Raw update demos now prefer `build/dev` executables and retry transient network failures.

### Fixed

- Fixed Windows Unicode path handling across CLI arguments, updater apply-plan arguments, apply-plan JSON, state-store JSON, process launch, local `file://` URLs, and progress reporting.
- Fixed local `file://` path decoding so percent-encoded UTF-8 paths work in static-file update tests.

## v0.1.4 - 2026-06-03

### Added

- Added a generated architecture diagram image and referenced it from the English README, Chinese README, and architecture plan.

### Changed

- Release workflow no longer publishes separate `.sha256` assets; release ZIPs and SPDX SBOM files remain published.

## v0.1.3 - 2026-06-03

### Added

- Added open-source governance files, issue and PR templates, API documentation, security model, integration guide, server layout guide, troubleshooting guide, quality gate documentation, and ecosystem packaging templates.
- Added content-addressed object storage support to `tools/make_manifest.py`, allowing `path` to point to server-side objects and `localPath` to point to installation paths.
- Added `tools/gc_objects.py`, `tools/check_release_ready.py`, `tools/extract_changelog.py`, and `tools/make_sbom.py`.
- Added clang-format, clang-tidy, coverage, and content-addressed packaging validation to CI. The release workflow now emits SBOM files and CHANGELOG-based release notes.
- Added lightweight fuzz smoke tests for version parsing, release manifest parsing, index manifest parsing, apply-plan parsing, and managed path validation.
- Added `README.zh-CN.md` while keeping English as the default project documentation language.
- Added README project status, support matrix, package manager status, one-minute demo result, security-at-a-glance table, and FAQ sections.

### Changed

- `Config::clientVersion` now defaults to the current library version to avoid version metadata drift.
- The `no-optional-deps` preset disables WinHTTP on Windows, matching the intent of a build without optional HTTP backends.
- Default Markdown documentation is now English.
- Reworked the default README into a more scannable open-source project landing page.

## v0.1.2 - 2026-06-02

### Fixed

- Fixed the Windows `autoupdater_apply` target using the console subsystem, which could show a console window during desktop self-update. The updater executable is now built as a GUI subsystem target on Windows.

## v0.1.1 - 2026-06-02

### Fixed

- Fixed the updater subdirectory using `CMAKE_SOURCE_DIR` when consumed through `add_subdirectory` or an external project. It now uses `libAutoUpdater_SOURCE_DIR`.
- Fixed managed file paths being handled as local narrow-character paths in `safeJoin`. Paths are now decoded as UTF-8, improving cross-platform handling for non-ASCII paths.

## v0.1.0 - 2026-06-02

### Added

- Initial C++17 cross-platform online update library core.
- External updater executable.
- CLI example.
- Packaging scripts.
- CI.
- Real GitHub Raw update example.
