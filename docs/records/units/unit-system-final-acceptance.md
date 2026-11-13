# AOWIS unit-system completion — final audit and acceptance record

**Audit baseline:** `gui(20261011-172420).zip`, 11 October 2026. The accompanying historical user report states **21/21 Linux `aowis` CTest tests passed**, total 0.67 seconds. That report does not identify its CMake configuration; do not interpret it as both Debug and Release. The Model, EPANET wrapper, database/REST and standalone service were **not supplied in this audit snapshot**.

**Decision:** **Implementation source audit completed; full nine-step roadmap acceptance is *not yet signed off*.** Steps 1–5 have source/test coverage; Step 6 has GUI-controlled format coverage but external repository boundaries are unverified; Step 7 has a passing reported Linux suite but incomplete platform acceptance; Step 8 has both the synthetic benchmark implementation and one reported Linux measurement, but still lacks production-renderer measurements. Step 9 collects these findings and specifies how to close them without conflating successful compilation with full acceptance.

This document is the final *evidence ledger*, not a new conversion layer, engineering design proposal or an assertion that all remaining work is finished. The detailed GUI surface audit remains in [unit-gui-coverage-matrix.md](unit-gui-coverage-matrix.md).

## Update — Steps 4 and 5 (11 October 2026)

For `gui(20261011-204355).zip`, the user supplied results of
`./compile_linux_tests.sh --both`, confirming **23/23 in Debug** (0.79 s)
and **23/23 in Release** (1.08 s), with the registered synthetic benchmark
passing in both. **The Linux Debug/Release regression gate is passed for that
baseline.** These results are not evidence of production renderer performance,
actual solver non-invocation, or cross-platform behavior.

Step 5 adds a single cross-surface Qt acceptance regression under the existing
`aowis-tests` target. It exercises real editor, view-lifecycle, table and
map/simulation presentation helpers together; it is **not the actual running
production GUI**. Its results must be reported separately after the patch is
built. The full [production acceptance protocol](unit-system-production-acceptance.md)
identifies the still-pending live inspector, table, map, simulation and
large-network checks. No new test runner or duplicate build script is added.

Use the existing `./compile_linux_tests.sh --both` for independent Debug and
Release tests, and `./compile_linux_benchmarks.sh --both` for benchmark-only
runs. Optional ASan/UBSan checks use normal CMake/CTest, as described in
[unit-system-linux-verification.md](unit-system-linux-verification.md).

---

## 1. Source-level closure review

| Step | Code and test evidence in this snapshot | Audit disposition |
| --- | --- | --- |
| 1 — WASM map scale | C++-provided display-unit scale choices (`MapScaleDistance`), drawn by the bgfx HUD on desktop and WASM; covered by `map_render_neutral_tests.cpp`. (The JavaScript scale in `aowis-browser-map.js` and its tests were removed with the JavaScript map renderer.) | **Source-implemented**. Actual browser/WASM switch and no-map-reload behavior must be observed |
| 2 — Typed table quantities | `UnitTablePresentation::ColumnUnits`; `TableColumn` explicit quantity and filter kind; row-dependent special cases; `unit_table_presentation_test.cpp` | **Source-implemented**. Audit table modes, clipboard/export and live filter/editor transitions |
| 3 — Precision-safe slider controls | `SliderNumberControl::precise_value`, `NumericPropertyBinding`, separate slider/spin representations; `slider_number_control_test.cpp` | **Source-implemented**. Verify real settings persistence with values more precise than the widget shows |
| 4 — Cleanup | `include/aowis/model/units/conversion.h` forwards to Model; `unit_model_header_authority_test.cpp` verifies Model-only units; obsolete Settings text corrected; tank suffix cleanup | **Source-implemented**. The compatibility forwarding file intentionally remains because a ZIP patch cannot delete a tracked file; remove it via Git only after all downstream includes are migrated |
| 5 — Coverage inventory | `unit-gui-coverage-matrix.md`, `unit-conversion-inventory.md`, `legacy-numeric-control-audit.md`, `unit-table-metadata.md` | **GUI source inventory completed**, subject to runtime and cross-repository reconciliation |
| 6 — External I/O | `epanet_inp_unit_roundtrip.inc`, `epanet_js_converter_unit_reference.inc`, documented EPANET-JS import/INP export separation | **Partially verified**. Cannot certify DB/REST/standalone/EPANET-MS X source versions from GUI-only ZIP |
| 7 — Regression/platforms | User-reported **23/23 Debug and 23/23 Release** for the current pre-Step-5 GUI baseline; `compile_linux_tests.sh --both` exists; tests use Release-safe checks, and `aowis-tests` has all directly registered C++ test executables as dependencies | **Linux Debug/Release both passed** for the pre-Step-5 baseline; browser WASM and Step-5 added cross-surface test results remain pending (Windows: reported working by the user, 14 November 2026) |
| 8 — Performance | CTest-registered `aowis-unit-presentation-benchmark`, 100k-row synthetic table / 250 hidden regions / 500 switches; [performance runbook](unit-presentation-performance-verification.md) | **Synthetic Linux measurements supplied; production acceptance pending**. Benchmark included in the 23-test Debug/Release suites |
| 9 — Final audit | This ledger; prior conversion and GUI documentation; checks below | **Audit performed, final release sign-off blocked** by outstanding Step 6–8 evidence |

No additional production refactor is justified solely by this audit. Existing profile notifications are used by Units Settings and toolbar, and the Model-header forwarding file still protects deployed legacy include trees. Keep intentional compatibility paths until a verified Git deletion is possible.

## 2. Minimum deterministic checks before sign-off

Run in the GUI checkout with matching Model submodule, Qt 6 and Node.js:

```sh
./compile_linux_tests.sh --both

cmake -S . -B build-linux-tests-release -G Ninja \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-tests-release \
  --target aowis-unit-presentation-benchmark --parallel
QT_QPA_PLATFORM=offscreen \
  ./build-linux-tests-release/aowis-unit-presentation-benchmark \
  | tee unit-presentation-benchmark-release.jsonl
```

Capture the **Debug and Release test counts, failures and dates separately**. Do not infer a particular test total from this ledger if more targets are subsequently added. Check the benchmark JSON reports its expected invariant fields (e.g. `profile_switches`, `hidden_refreshes_before_show`, `hidden_refreshes_after_show`, `viewport_display_reads`, `canonical_writes`). Its `canonical_writes: 0` refers only to the *synthetic read-only table* and does not prove production Model writes are zero. Compare benchmark timings with repeat runs on the same machine; do not impose a machine-independent time cutoff.

The audit also independently checked the following in this snapshot:

- `node tests/browser_map_scale_test.js` **passed** at the time; that test was later removed together with the JavaScript map renderer.
- The ordinary C++ test executables **and the benchmark** are dependencies of `aowis-tests`; the benchmark is now registered as a CTest test with labels `aowis` and `performance`, and uses structural assertions without timing cutoffs.
- Unit conversion's GUI compatibility header contains no duplicate scale constants; it forwards to the Model submodule.
- A passing unit test is evidence only for the cases it exercises. Complete native Qt application and production renderer runs were **not performed by this audit**.

## 3. Runtime and integration acceptance cases

Record real results for each of these before marking Step 9 complete:

| Gate | Required observation | Evidence to attach | Status |
| --- | --- | --- | --- |
| Linux Debug and Release | Entire registered `aowis` suite passes separately under each configuration with actual assertions enabled | User-provided `./compile_linux_tests.sh --both` log | **Passed: 23/23 each, pre-Step-5 baseline** |
| Precision/undo in live inspectors | Edit one rounded coordinate without rounding its neighbor; emitter exponent change rejects stale coefficient; open/close, select different entity and unit-switch-only actions cause no unintended hydraulic Model writes; History Undo/Redo is implemented but requires live acceptance | Scenario record with starting and ending canonical values | **Pending live GUI run** |
| Tables and filters | All active hydraulic/quality table modes use typed quantities; sort/filter retain canonical semantics; human copy contains correct displayed units | Column/mode matrix, snapshots or results | **Pending live GUI run** |
| Linux production performance | Large-network profile switch with inspector, table, Map Monitor 2D/3D, Map Editor and simulation open/hidden; zero unit-only network/terrain/geometry rebuilds and solver runs; hidden regions refresh on activation | Dataset sizes, renderer, timing/counter report, benchmark JSON | **Pending** |
| Windows desktop | Representative inspector, table, simulation and map profile-switch tests on supported Windows build | Version/backend, test logs and manual validation | **Reported working by the user (14 November 2026)** |
| WebAssembly browser | Metric/US scale labels and Qt map HUD change together without camera/tile reload or geometry rebuild; import dialog/profile selection works in real browser | Browser/build version, scale profile result, console log and network/cache observations | **Pending** |
| EPANET + MSX | Match wrapper revision; explicit format unit conversions and solver/report contracts independent of GUI presentation | Wrapper revision and relevant test output | **Pending wrapper snapshot/run** |
| AOWIS DB/REST (SQLite **and** PostgreSQL) | Schema/API quantity contract and identical canonical semantics across both backends; preserve precision/nulls and round trips | Schema references, paired backend test output | **Pending DB/REST source and runtime** |
| Optional affine temperature conversions | If an actual temperature-valued quantity is introduced, registry supports offset conversions; otherwise document as deliberately out of current hydraulic scope | Model registry/spec decision | **Deferred, not a silent pass** |

**Acceptance rule:** mark a gate Passed only after its evidence is provided. Mark N/A only with a reason and reviewer acknowledgment; unexamined paths stay Pending. The GUI's EPANET-JS `.ejsdb` *import* is not evidence of AOWIS SQLite database persistence, and `.ejsdb` **export is not supported** in the documented GUI integration.

## 4. Completion decision

- **What can be accepted today:** the source-level implementation of Steps 1–5, the user-reported 23/23 Debug and 23/23 Release regression results for the pre-Step-5 baseline, and the integrated synthetic benchmark. The added Step-5 cross-surface test still requires its first full build/run.
- **What cannot be signed off today:** complete cross-platform operation, DB/REST parity, matching EPANET/MSX wrapper behavior, and production Model-write/repaint and performance invariants. Synthetic benchmark timings are not production renderer measurements.
- **No new architecture is indicated:** preserve the canonical Model boundary, explicit quantity metadata, presentation-generation lazy refresh, and precision-safe editing as the completed core.

**Final sign-off status: OPEN — awaiting evidence above.** When those gates are complete, record the versions, test logs, benchmark JSON, acceptance date and reviewer here. Do not change this decision solely because the regular Linux suite continues to pass.

| Release / revision | Linux Debug | Linux Release | Windows | Browser WASM | DB/REST/EPANET | Production performance | Reviewer/date |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `gui(20261011-204355)` (pre-Step-5) | 23/23 passed | 23/23 passed | Reported working (14 Nov 2026) | Pending | Pending | Pending | Not signed |
| Step-5 patch (new cross-surface test) | Pending new test run | Pending new test run | Reported working (14 Nov 2026) | Pending | Pending | Pending | Not signed |
