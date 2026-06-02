# Contributing to libAutoUpdater

Thank you for improving `libAutoUpdater`. This project aims to be a C++17 online update library that desktop applications can rely on for a long time, so contributions should prioritize testability, cross-platform behavior, and clear security boundaries.

## Development Environment

Required tools:

- CMake 3.21 or newer for presets. The baseline build still supports CMake 3.16.
- A C++17 compiler: MSVC, GCC, Clang, or AppleClang.
- Python 3.9 or newer for packaging tools and end-to-end tests.

Optional tools:

- vcpkg for libcurl and OpenSSL.
- clang-format, clang-tidy, and gcovr for quality checks.

## Local Build

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

On Windows, if `python` points to the Microsoft Store alias, use:

```powershell
uv run python -m py_compile tools/make_manifest.py tools/make_index.py tools/sign_manifest.py
```

## Design Principles

- Keep the core library independent of GUI frameworks.
- Keep network, hash, filesystem, signature, process, event-dispatch, and state-store concerns behind interfaces.
- Never let the main application process replace its own running files directly; replacement must go through the external updater process.
- Validate all manifest paths and reject absolute paths or path traversal.
- Do not leak exceptions through the public API; use `Result<T>`, `ErrorCode`, and error messages.
- Put new behavior in testable decision code when possible. Cover real IO through adapters or flow tests.

## Before Opening a Pull Request

Run at least:

```sh
git diff --check
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

If your change touches packaging tools, also run:

```sh
python tools/make_manifest.py --help
python tools/make_index.py --help
python tools/gc_objects.py --help
```

If your change touches security, signatures, paths, downloads, apply behavior, or rollback, describe the risk boundary and verification evidence in the PR.

## Code Style

- C++ code uses C++17.
- Use 4 spaces for indentation.
- Public headers should include concise API comments.
- New source files should be ASCII unless there is a specific reason to use UTF-8.
- Do not expose third-party library types in public APIs.
- Default project documentation is English. Add localized documents only when they are explicit translations, such as `README.zh-CN.md`.

## Branches and Commits

Recommended commit prefixes:

- `feat:` new feature
- `fix:` bug fix
- `docs:` documentation
- `test:` tests
- `tools:` tooling scripts
- `ci:` CI/CD
- `chore:` maintenance

## Release Process

Before a release, confirm:

1. `project(... VERSION ...)` in `CMakeLists.txt` is updated.
2. `version-string` in `vcpkg.json` is updated.
3. `CHANGELOG.md` contains a matching `vX.Y.Z` section.
4. `main` branch CI and CodeQL are passing.
5. After pushing the `vX.Y.Z` tag, the release workflow uploads Windows, macOS, and Linux ZIPs, SHA-256 files, and SBOM files.
