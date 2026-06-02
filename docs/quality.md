# Quality Gates

## Local

```sh
git diff --check
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

## CI

GitHub Actions 覆盖：

- Source hygiene。
- clang-format。
- clang-tidy。
- GCC / Clang / AppleClang / MSVC。
- Debug / Release。
- no optional dependency build。
- ASan / UBSan。
- Coverage summary。
- Install tree and `find_package` consumer probe。
- GitHub Raw real update demo on libcurl, WinHTTP, and CFNetwork.
- CodeQL C++ analysis。

## Fuzz Smoke Tests

`tests/FuzzSmokeTests.cpp` 使用确定性随机输入覆盖：

- SemVer parser。
- release manifest parser。
- index manifest parser。
- apply-plan parser。
- managed path validation。

这不是替代 libFuzzer/AFL 的长期 fuzzing，而是用于 CI 的轻量防回归。
