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
- Install tree and `find_package` consumer probe.
- Real GitHub Raw update demo on libcurl, WinHTTP, and CFNetwork.
- CodeQL C++ analysis.

## Fuzz Smoke Tests

`tests/FuzzSmokeTests.cpp` uses deterministic random inputs for:

- SemVer parser.
- Release manifest parser.
- Index manifest parser.
- Apply-plan parser.
- Managed path validation.

This is not a replacement for long-running libFuzzer or AFL coverage. It is a lightweight CI regression guard.
