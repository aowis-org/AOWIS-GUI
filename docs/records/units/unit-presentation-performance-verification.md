# Unit presentation performance verification — finishing roadmap Step 8

**Input baseline:** `gui(20261011-165505).zip`; 11 October 2026. The user reported **21/21 Linux `aowis` CTest tests passed** (0.63 s) after fixing the Step-7 test compilation issue. The report did not establish whether both Debug and Release passed. This document describes a benchmark added in this patch. **A synthetic benchmark run from the user's Linux machine was subsequently supplied; real application map, Windows and browser WebAssembly performance measurements remain unavailable.** Do not mark Step 8 fully verified until those results are collected.

## Run benchmarks with visible results

The dedicated runner builds only the benchmark targets and executes all CTest
cases labeled `performance` **with verbose output**. CTest normally hides
stdout from passing tests; this runner prints each JSON measurement in the
terminal, followed by the test result. Use Release for comparative timing;
Debug is useful for correctness and diagnostics.

```sh
./compile_linux_benchmarks.sh             # Release; shows benchmark JSON
./compile_linux_benchmarks.sh --debug     # Debug
./compile_linux_benchmarks.sh --both      # Debug followed by Release
```

It reuses `build-linux-tests-debug` and `build-linux-tests-release` so it does
not require separate configurations. CMake's `aowis-benchmarks` target collects
benchmark executables; future benchmarks should be added to that target and
registered with the `performance` CTest label. `compile_linux_tests.sh` retains
the full regular test workflow. Benchmark timings are reported, **not** used as
fixed pass/fail thresholds.

## Automated CTest benchmark (also runnable standalone)

The benchmark source is `tests/unit_presentation_performance_benchmark.cpp`. It uses real Qt Widgets, the current production `UnitPresentationContext`, `UnitAwareView`, and `UnitTablePresentation` paths with **synthetic canonical data**. It does not construct actual EPANET networks or query the database and cannot measure the bgfx map renderer.

From the GUI repository root (Qt 6 and the Model submodule required):

```sh
cmake -S . -B build-linux-tests-release -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-tests-release --target aowis-unit-presentation-benchmark --parallel
QT_QPA_PLATFORM=offscreen ./build-linux-tests-release/aowis-unit-presentation-benchmark | tee unit-presentation-benchmark-release.jsonl

cmake -S . -B build-linux-tests-debug -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux-tests-debug --target aowis-unit-presentation-benchmark --parallel
QT_QPA_PLATFORM=offscreen ./build-linux-tests-debug/aowis-unit-presentation-benchmark | tee unit-presentation-benchmark-debug.jsonl
```

The benchmark **is registered with CTest** under the `aowis` and `performance` labels, and the `aowis-tests` target builds it. Therefore `./compile_linux_tests.sh`, including `--both`, runs it automatically: **22 tests per configuration** (previously 21). It fails only for deterministic correctness violations, **never** for exceeding a wall-clock time threshold. Running the executable directly, as above, is still useful for saving or comparing JSON timing reports.

The probe performs these operations:

| Scenario | Load | Structural assertion / metric |
| --- | --- | --- |
| Large virtual table | 100,000 rows × 4 numeric columns, fixed viewport 1024 × 680 | One active-unit switch invalidates read-only display data; inspect `viewport_display_reads`, `viewport_refresh_ms` |
| Viewport format-on-demand | 400,000 *possible* cells | Display-role fetches must be nonzero but below 5% of total cell count (per repaint), proving no full-table reformatting |
| Many hidden unit subscribers | 250 inactive child views | Zero hidden callbacks for 500 profile changes, then exactly one callback per region upon activation |
| Visible updates | 500 profile changes (metric/customary alternating) | One refresh per change for the visible table; report notification elapsed time |
| Precision/read-only model | Selected synthetic canonical measurements | Values unchanged across every profile change; no setter exists in the synthetic model |
| Post-burst repaint | Same viewport | Still visible-cell-only formatting; report repaint elapsed time |

The output is a machine-readable single-line JSON object with timings in **milliseconds**, callbacks, and counts. Compare the **same machine, build type, Qt version, viewport, and offscreen platform** across revisions. Prefer medians of several runs after a warm-up. Never impose hard millisecond limits in CTest; evaluate regressions against a measured baseline, including responsiveness observed in the real application. The `canonical_writes: 0` output describes the *synthetic read-only table only*, not production hydraulic model writes.

If the benchmark's structural assertions fail, investigate why a refresh is touching hidden regions, repeatedly invalidating rows or formatting an entire table. If only timings rise, profile before changing data structures.

## Required production performance acceptance (not covered by the synthetic benchmark)

Use an existing large project or generate a **representative large hydraulic network** with many junctions, pipes, Demand Points and simulation result values. Record the dataset counts, CPU, RAM, OS, Qt version, map renderer backend (bgfx with Vulkan, OpenGL, Direct3D 11, Metal or WebGL 2, as logged at startup), build type and run date.

1. **One visible inspector:** Switch between metric and US display units several times; confirm only the inspector's presentation changes. Count canonical `HydraulicData`/revision writes (target zero) and no undo entries.
2. **Large entity table:** Keep a scrolled/sorted table open. Profile role requests, paints, filter state and unit-change latency; verify only viewport cells need new formatting and canonical sorting/filter bounds remain unchanged.
3. **Hidden tabs and dialogs:** Switch profiles many times while inactive, then activate each tab once. Count display refreshes (zero while hidden, one catch-up per presentation region). Test nested pages and destroyed widgets.
4. **Map Monitor 2D and 3D:** Compare frame times, label regeneration, overlay/heatmap data rebuild counts, GPU buffer uploads, and topology/geometry/terrain cache rebuilds (`AOWIS_MAP_RENDER_DIAGNOSTICS=1` logs frame and globe rebuild counters). **Display-unit-only changes should cause zero geometry/network/terrain rebuilds and zero simulation runs.** Do not infer these values from the synthetic benchmark.
5. **Map Editor and simulation statistics/plots:** Test active and hidden states, cursor/legend/axis unit updates, and preserve camera state and solver results. Measure CPU time and unintended model writes.
6. **WebAssembly in a real browser:** Change units while map labels and the scale are visible. Confirm matching scales without tile reload, no network geometry rebuild, and acceptable frame latency; use browser performance tooling.
7. **Native Windows:** Repeat at least the inspector, large-table, and map measurements on the target backend.

Collect structured results, with a before/after or reproducible baseline, for each renderer and target platform. If data collection is impossible, record **unverified** rather than reporting success.

## Acceptance status

| Evidence | Status |
| --- | --- |
| Lazy unit-generation behavior and precision invariants | Covered by existing regression tests; user's latest report: 21/21 pass |
| Synthetic 100k-row virtualized table + 250 hidden views + 500 changes | **Linux benchmark executed**; structural checks passed. Viewport refresh 10.890 ms, notifications 5.614 ms, hidden catch-up 4.603 ms, post-burst repaint 8.107 ms (single run; no cross-machine threshold) |
| Debug/Release performance comparison on Linux | Pending actual measurements |
| Production map geometry/terrain rebuild counters | Not instrumented by this patch; requires profiling application |
| Production hydraulic revision/model write counters | Not instrumented by this patch; requires application run |
| Windows/native browser WASM performance | Pending |

**Step 8 status:** The synthetic benchmark has passed in a direct Linux run: 100,000 rows × 4 columns, 250 hidden views, 500 changes, 120 display-role reads per viewport, zero premature hidden refreshes, and 250 catch-up refreshes. It is now included as the **22nd regular Linux CTest** and is built by `aowis-tests`. This patch requires a new 22-test suite run for confirmation. Final *production* performance sign-off still needs the real application measurements above. The Step-9 [acceptance ledger](unit-system-final-acceptance.md) records the blocking evidence. **Step 9 (final audit/sign-off) must not treat this step as complete merely because the benchmark target compiles.**
