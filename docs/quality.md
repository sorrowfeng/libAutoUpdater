# Quality Gates

## Local

```sh
git diff --check
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

## CI

GitHub Actions covers:

- Source hygiene.
- clang-format.
- clang-tidy.
- GCC / Clang / AppleClang / MSVC.
- Debug / Release.
- No-optional-dependency build.
- ASan / UBSan.
- Coverage summary.
- Static install tree and clean `find_package` consumer probes.
- Shared installed-package probes on Windows, macOS, and Linux, including the
  exported updater executable and its runtime library lookup.
- Real GitHub Raw update demo on libcurl, WinHTTP, and CFNetwork.
- CodeQL C++ analysis.
- A mandatory-OpenSSL contract that proves missing crypto fails configuration
  and exercises the real signature verifier when OpenSSL is present.
- Release configurations rerun tests before packaging, and the publishing job
  accepts only tags whose exact `main` commit already passed CI.

Workflow Actions are pinned to immutable commit SHAs and updated through
reviewed Dependabot pull requests.

## Fuzz Smoke Tests

`tests/FuzzSmokeTests.cpp` uses deterministic random inputs for:

- SemVer parser.
- Release manifest parser.
- Index manifest parser.
- Apply-plan parser.
- Managed path validation.

This is not a replacement for long-running libFuzzer or AFL coverage. It is a lightweight CI regression guard.
