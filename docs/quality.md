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
- Local loopback transport contracts for libcurl, WinHTTP, CFNetwork, and Qt,
  including redirects, slow responses, timeout, cancellation, and downloads;
  native POSIX and Windows process launch/wait behavior runs in the same matrix.
- CodeQL C++ analysis.
- A mandatory-OpenSSL contract that proves missing crypto fails configuration
  and exercises the real signature verifier when OpenSSL is present.
- Offline deployment-evidence validator regression tests, including signing
  workflow JSON-parser consistency. These test decision rules, not production
  deployment state.
- Release configurations rerun tests before packaging, and the publishing job
  accepts only tags whose exact `main` commit already passed CI.

Workflow Actions are pinned to immutable commit SHAs and updated through
reviewed Dependabot pull requests.

## Fuzz Testing

`tests/FuzzSmokeTests.cpp` uses deterministic random inputs for:

- SemVer parser.
- Release manifest parser.
- Index manifest parser.
- Apply-plan parser.
- Managed path validation.

The smoke test remains a lightweight deterministic regression guard. A
separate Clang/libFuzzer target in `tests/fuzz/` instruments the library and
runs the parsers, managed-path policy, and URL helpers persistently under
ASan/UBSan with a checked-in seed corpus and dictionary. CI runs a bounded
coverage-guided session and preserves the evolved corpus and crash artifacts
for review.

Installed-package validation consumes both static and shared packages from a
clean project, generates and independently verifies a signed release artifact,
then uses the installed external updater to apply and roll back a fixture
update.
