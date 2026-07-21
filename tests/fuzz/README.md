# Persistent fuzzing

The fuzz target is intentionally a standalone CMake project. It instruments
both the core library and the harness with libFuzzer coverage plus ASan/UBSan,
without changing normal library builds.

Configure and run it with a Clang toolchain:

```sh
cmake -S tests/fuzz -B fuzz-build -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build fuzz-build --target libAutoUpdater_parser_fuzzer --parallel
mkdir -p fuzz-artifacts
./fuzz-build/libAutoUpdater_parser_fuzzer \
  tests/fuzz/corpus \
  -dict=tests/fuzz/autoupdater.dict \
  -artifact_prefix=fuzz-artifacts/ \
  -max_len=1048576 \
  -timeout=10 \
  -max_total_time=300
```

CI should preserve `tests/fuzz/corpus` between runs or upload a minimized
merged corpus from trusted runs. Crash artifacts must be reviewed before they
are promoted to regression seeds because arbitrary artifacts may contain
sensitive local bytes.
