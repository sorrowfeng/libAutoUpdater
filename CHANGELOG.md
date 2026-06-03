# CHANGELOG

## Unreleased

No unreleased changes yet.

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
