# Unit-system regression and platform verification — completion roadmap step 7

**GUI baseline:** `gui(20261011-163635).zip` (11 October 2026). The user reports **21/21 labeled Linux CTest tests passed** (1.08 s) before this step-7 patch. This is an automated-test baseline, **not** evidence of native Windows, browser WebAssembly, manual GUI interaction, or complete external DB/EPANET/MSX verification.

## Automated Linux test configurations

From the GUI repository root, with Qt development packages, submodules, Ninja and CMake available:

```bash
./compile_linux_tests.sh             # Existing build-linux-tests directory/workflow
./compile_linux_tests.sh --debug     # Separate Debug build-linux-tests-debug directory
./compile_linux_tests.sh --release   # Separate Release build-linux-tests-release directory (-DNDEBUG)
./compile_linux_tests.sh --both      # Debug followed by Release; stop on any failure
```

The build target remains `aowis-tests`, **not** the entire application. The runner invokes labeled CTest tests, rejects an empty test selection (compatible with the project's CMake 3.21 minimum), and exits nonzero for build/test failures. Explicit configurations use independent directories, so a previously configured directory cannot silently masquerade as another configuration. The no-argument behavior and directory are unchanged. This only verifies the GUI targets CMake actually registers; `node` availability affects the browser-JavaScript scale test registration.

Existing and expanded regression coverage includes:

| Contract | Automated evidence | Remaining acceptance |
| --- | --- | --- |
| No-op and profile-switch precision | `numeric_property_binding_test.cpp`, `unit_numeric_spin_box_test.cpp`, `slider_number_control_test.cpp` | Repeat on production inspector fields with values beyond displayed precision |
| Independent coordinate-axis edits | `coordinate_property_binding_test.cpp`, `coordinate_numeric_spin_box_test.cpp` | Select/switch real nodes and Demand Points; verify altitude preservation. DVMDB-backed adjacent Undo/Redo is wired for eligible single-network private drafts; end-to-end Qt acceptance remains open. |
| Emitter coefficient and exponent | `emitter_coefficient_binding_test.cpp`, `emitter_numeric_spin_box_test.cpp` | Edit on actual enabled junctions with mixed network exponent constraints |
| Visible/hidden unit generations | `unit_aware_view_test.cpp` | This step adds hidden nested-page, 500 profile-change coalescing, and destroyed-owner checks; still test real dock/tab lifecycles |
| Typed table cells and filters | `unit_table_presentation_test.cpp` | Edit/filter/sort across every live entity column and user clipboard/export behavior |
| Map scale logic (desktop and browser) | `map_render_neutral_tests.cpp` (map scale, HUD lengths) | Real browser/WebAssembly profile change while 2D/3D map active |
| External format independence | `epanet_inp_unit_roundtrip.inc`, `epanet_js_converter_unit_reference.inc`, GUI integration targets | REST/DB (SQLite + PostgreSQL), other EPANET/MSX revisions, and actual project export UI |

All `CHECK`-based regression conditions must continue executing in Release (`NDEBUG`); never put required actions only inside `assert()`.

## Windows native acceptance

Use a Windows-native Qt/CMake/Ninja installation that matches the GUI application's actual toolchain. From the GUI root in a suitable PowerShell environment:

```powershell
cmake -S . -B build-windows-tests-debug -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-windows-tests-debug --target aowis-tests --parallel
ctest --test-dir build-windows-tests-debug --output-on-failure -L '^aowis$'

cmake -S . -B build-windows-tests-release -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows-tests-release --target aowis-tests --parallel
ctest --test-dir build-windows-tests-release --output-on-failure -L '^aowis$'
```

CTest configures `QT_QPA_PLATFORM=offscreen` on the Qt widget tests. If the selected Windows Qt distribution lacks the offscreen platform plugin, resolve the Qt platform setup rather than removing those tests. These commands are **instructions, not executed Windows evidence**.

For UI acceptance, run the native Windows application and switch unit profiles while editing a high-precision pipe diameter, node coordinates and a demand point; visit an inactive tab and compare unchanged canonical values. Include tables, pressure/flow/quality readings, and the map scale bar. Capture the build configuration, Qt version, result and any plugin limitations.

## WebAssembly acceptance

Run the repository's established `./compile_wasm.sh` and serve `build-wasm-dist` through its intended HTTP(S) configuration (not a `file://` URL). Compile success by itself does not prove browser-runtime behavior. In a real browser, verify:

1. Profile switching changes **both** the Qt display and JS scale bar (e.g. metric m/km → US customary ft/mi) without changing zoom, center, tile coordinates or physical map distance.
2. Map Monitor, Map Editor and hidden/stacked pages pick up the latest display units after becoming visible; no duplicated scale labels remain.
3. A `.ejsdb` import preserves its source-unit semantics and optionally offers a custom display profile; network properties and EPANET export remain canonical/source-defined.
4. Rapid repeated switches do not trigger unexpected network reloads, solver reruns, or render geometry rebuilds. Record actual timings for Step 8.

The native Node.js test `tests/bgfx_map_canvas_test.js` only checks how `aowis-bgfx-map.js` places the bgfx canvas, in a mocked JavaScript VM, **not** a real Qt/WASM/browser environment. Do not report it as a completed WebAssembly acceptance test.

## Evidence ledger

| Environment | Debug | Release | Browser/UI integration | Evidence |
| --- | --- | --- | --- | --- |
| Linux native | **Not yet independently rerun for this patch** | **Not yet independently rerun for this patch** | Manual GUI unverified | User's preceding 21/21 Linux result did not specify build configuration |
| Windows native | Reported working | Reported working | Reported working | User report, 14 November 2026 (no logs recorded) |
| WebAssembly | Not applicable to native CTest target | Not applicable to native CTest target | Pending | Requires a produced WASM bundle and browser runtime |
| External DB/REST + EPANET/MSX | Outside GUI-only test suite | Outside GUI-only test suite | Pending | Requires matching repository snapshots and backend-specific validation |

**Step 7 status:** Expanded regression coverage and explicit Debug/Release Linux runner are implemented in this patch. Full *cross-platform acceptance is not closed* until the above results are actually collected. Proceed to Step 8 benchmarks in parallel once Debug/Release checks pass; do not interpret missing cross-platform evidence as a verified pass.
