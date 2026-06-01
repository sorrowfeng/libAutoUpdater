# AGENTS.md

This file gives guidance to future coding agents working in this repository.

## Project Intent

`libAutoUpdater` is a C++17 cross-platform desktop online update library. The design target is a production-ready library with minimal core dependencies, static-file release hosting, safe self-update through an external updater process, and testable infrastructure abstractions.

Read [docs/architecture-plan.md](docs/architecture-plan.md) before making architectural changes.

## Engineering Principles

- Keep the core library independent of GUI frameworks.
- Keep network, hash, filesystem, signature, process, event-dispatch, and persistence concerns behind interfaces.
- Prefer pure decision logic for planning code so it can be unit tested without real IO.
- Do not let the main application process replace its own files directly.
- Treat `apply-plan.json` as the contract between the library and the external updater.
- Validate all manifest paths before touching the filesystem.
- Preserve rollback behavior when changing updater apply logic.
- Use explicit error codes and messages instead of leaking exceptions through the public API.

## Implementation Constraints

- Language standard: C++17.
- Build system: CMake.
- Public headers live under `include/libAutoUpdater/`.
- Core implementation lives under `src/`.
- External updater implementation lives under `updater/`.
- Tests live under `tests/`.
- Tools live under `tools/`.
- Documentation lives under `docs/`.

## Dependency Policy

- The core architecture should not depend directly on libcurl, OpenSSL, Qt, or nlohmann/json types in public APIs.
- Default implementations may use optional dependencies behind interfaces.
- Qt support should remain optional and isolated to examples or adapters.

## Safety Checklist

Before changing update or apply behavior, verify:

- Path traversal is rejected.
- Absolute paths from manifests are rejected.
- Files are downloaded into staging, not directly into the install directory.
- SHA-256 verification runs before an update becomes ready to apply.
- Apply operations are backed up before replacement or removal.
- Failed apply operations can roll back.
- Concurrent updater processes cannot modify the same install directory.

## Documentation

Prefer clear Chinese documentation for user-facing design docs in this repository. API names and code identifiers should remain English.

## Current Status

The repository currently contains architecture planning materials. Implementation should proceed according to the phases in `docs/architecture-plan.md`.

