# Unit-system Step 4 — Linux Debug, Release and optional sanitizer verification

This document describes **Linux unit-system verification**, not cross-platform
or production-renderer sign-off. On `gui(20261011-204355).zip`, the user ran
`./compile_linux_tests.sh --both` and reported **23/23 Debug tests passing**
(0.79 s) and **23/23 Release tests passing** (1.08 s). Both included the
registered synthetic benchmark. The Step-5 patch introduces an additional
cross-surface Qt test; the results above precede that new test and cannot be
used to mark it passed.

## Existing Debug and Release workflow

In the real GUI Git checkout, with Qt 6 development packages, Ninja, Node.js,
and initialized submodules, use the **existing** test script:

```sh
./compile_linux_tests.sh --both
```

The script configures, builds `aowis-tests` and runs the `aowis`-labeled CTest
suite **separately** in:

- `build-linux-tests-debug` (`CMAKE_BUILD_TYPE=Debug`)
- `build-linux-tests-release` (`CMAKE_BUILD_TYPE=Release`)

Either failed build or failed test run causes a nonzero exit status. It also
checks that the selected test list is nonempty. For one configuration, use
`./compile_linux_tests.sh --debug` or `./compile_linux_tests.sh --release`.
Without options, `./compile_linux_tests.sh` retains the original
`build-linux-tests` workflow (whose build type may be independently configured).

The existing script is the only Linux suite entry point; no parallel
acceptance/test runner is needed. Record the test counts, any
failures and the CMake build type **for each run**; the Step-5 patch is expected to register 24 `aowis` tests (provided Node.js is available), but test counts may legitimately grow. CTest stores
its execution log under each build tree's `Testing/Temporary/LastTest.log`.
The regular suite includes the unit-presentation benchmark (labeled both
`aowis` and `performance`), and the existing test script uses verbose CTest
output so the benchmark's JSON appears in the terminal. For a dedicated
benchmark run, use `./compile_linux_benchmarks.sh --both` instead.

## Optional ASan/UBSan checks

If memory-safety or undefined-behavior validation is required, use ordinary
CMake build directories rather than adding another project-specific runner.
For example, an AddressSanitizer build:

```sh
cmake -S . -B build-linux-tests-asan -G Ninja \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address -fno-omit-frame-pointer -g' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer -g' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address'
cmake --build build-linux-tests-asan --target aowis-tests --parallel
ASAN_OPTIONS=detect_leaks=1 \
  ctest --test-dir build-linux-tests-asan --output-on-failure -L '^aowis$'
```

For UndefinedBehaviorSanitizer, use a separate `build-linux-tests-ubsan`
directory and the same commands with `address` replaced by `undefined`, with
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` for its CTest run.
Sanitizer instrumentation and leak detection may be limited by third-party
libraries, platform configuration or sanitizer runtime availability; record
what was actually executed. Treat sanitizer errors as defects requiring
investigation, not as passing results.

## Checks and evidence

For every configuration actually run, retain the source and submodule
revisions, build type, Qt version, CTest summary and any failing logs. The
suite should have a nonzero registered test count, and every test should pass.
Do not rely on Release-disabled `assert(...)` checks; the unit-system tests
use unconditional checks. Preserve the benchmark JSON when comparing
performance and verify structural invariants including:

- `canonical_writes: 0` for the **synthetic read-only benchmark model**;
- `hidden_refreshes_before_show: 0`;
- exactly one hidden-view catch-up per hidden region when shown;
- viewport-limited display reads rather than whole-table conversion.

Benchmark timings are observations, **not** fixed machine-independent pass/fail
thresholds. The benchmark's `canonical_writes: 0` does not prove that the
production hydraulic Model experiences no unit-only writes.

## Previously performed checks in the inspection environment

The inspection environment lacked Qt 6 development packages and the external
EPANET checkout, so the full Qt/CTest suite was **not** executed there. Six
standalone C++ tests from the earlier Step-4 source review were built directly
and passed under each of these three configurations:

| Mode | Tests | Result |
| --- | --- | --- |
| `-O2 -DNDEBUG` | Model conversion, Model header authority, valve setting sorting, numeric binding, coordinate binding, emitter coefficient binding | 6/6 passed |
| AddressSanitizer | The same six standalone tests | 6/6 passed |
| UndefinedBehaviorSanitizer | The same six standalone tests | 6/6 passed |

These checks do **not** replace the Qt suite, do not cover QObject subscription
lifecycles and do not establish native application or map-renderer behavior.

## Remaining acceptance

Separate Linux Debug and Release results are supplied for the pre-Step-5 baseline; the newly added acceptance target still requires execution. Full production and platform sign-off additionally requires live
inspector/undo behavior, large-network tables and map rendering, Windows and
browser WebAssembly, EPANET/MSX interoperability, and matching canonical data
semantics through **both SQLite and PostgreSQL**. See
[unit-system-final-acceptance.md](unit-system-final-acceptance.md).
