# AOWIS — GUI-wide measurement unit I/O coverage matrix

**Inventory snapshot:** originally `gui(20261011-162011).zip` (11 October 2026), after unit cleanup step 4; final source audit cross-checked against `gui(20261011-172420).zip` (11 October 2026). **Reported baseline:** 21/21 `aowis` Linux CTest tests passed (0.71 s); the test report comes from the user, not a rerun by this audit. This document closes **step 5 (source inventory)** of the nine-step finishing roadmap; it does **not** certify runtime/platform/performance completion of steps 6–9.

**Scope:** GUI C++ sources, Qt editors, map/render presentation, browser JavaScript, import and simulation boundaries, and the unit-related tests in this GUI snapshot. There is **no matching Model, EPANET wrapper, DB or standalone-server ZIP** in this step's input; their implementation and end-to-end contracts are explicitly **not verified** here. The earlier `unit-conversion-inventory.md` is an EPANET-JS-focused historical inventory, not a substitute for this matrix.

## Legend and review method

| Code | Meaning |
| --- | --- |
| **W** | Wired in the current source: a unit-aware presentation/adapter or explicit format contract exists. This does not imply cross-platform runtime verification. |
| **M** | Mixed/conditional: multiple cases or manual formatting paths; requires targeted functional verification. |
| **I** | Intentionally independent of the user's physical display profile (coordinate-system, renderer, GUI-only, or external-format value). |
| **B** | Boundary/other repository: unit semantics are explicit or source-native, but integration must be checked separately. |
| **V** | Verification required: runtime behavior or complete coverage not established by this GUI snapshot alone. |

**Canonical** below means the property's *declared AOWIS storage unit*, not always an SI base unit. Existing examples include `m³/h`, `mm`, `m`, `m³`, `kW`, `mg/L`, WGS84 degrees, and model-specific dimensionless coefficients. No physical Model value should be obtained by parsing a rounded display string. In particular, **unit presentation changes must perform zero Model writes**. Treat column heading/source suffix literals as possible *metadata*, not automatically as visible hardcoded output; likewise, do not confuse pixels and UI percentages with physical quantities.

## 1. Central conversion, profile and numeric-input infrastructure (original phases 1–7, 13, 16)

| Surface (source) | Physical / I/O contract | Status | Next evidence / owner |
| --- | --- | --- | --- |
| `src/config/unit_profile_manager.h`, `unit_profile_notifications.h`, `unit_presentation_context.h` | Active quantity-to-unit profile, generation and notification; metadata changes distinguished from effective display changes | **W** | Exercise inactive/identical profile changes, subscriber lifetimes, persistence (step 7) |
| `src/config/unit_aware_view.h` | Visible views refresh; hidden views catch up by generation | **W** | Stacked/hidden/detached window and rapid-switch Qt scenarios (steps 7–8) |
| `src/config/numeric_property_binding.h` | Authoritative property read, pending edit, validation, explicit canonical commit | **W** | Production edit/no-op/focus tests with nonrepresentable precision (step 7) |
| `src/config/numeric_editor_adapters.h`, `unit_numeric_spin_box.h`, `unit_energy_price_spin_box.h` | Spin boxes, integer slider representation, units/locale/ranges; inverse-energy prices | **W** | Range clamping, stepping, profile switch while editing, keyboard commit tests (step 7) |
| `src/config/numeric_text.h` | Formatting/parsing, locale, decimal/significant policies | **W** | Precision, extremes, text entry and invalid value tests (step 7) |
| `src/config/unit_bulk_reaction_display.h`, `unit_energy_price_conversion.h` | Order-dependent reaction and per-energy-price conversion | **W** | Reaction order and currency-denominator scenarios (steps 6–7) |
| `src/config/unit_table_presentation.h` | Explicit `ColumnUnits` quantity/kind metadata; display-time conversion separate from sortable numeric value | **W** | Check every heterogeneous column/fallback and row-specific quantity (step 7) |
| `src/settings/units_settings_widget.cpp`, `src/app/top_control_bar.cpp` | Built-in/custom unit profiles, edit/selection UI, active-profile switch | **W** | Persistence/profile deletion and UI responsiveness tests (step 7) |
| `src/config/unit_profile_storage.cpp` | Storage of GUI display-profile preferences | **B** | Check old settings migration, corrupted profiles, user/profile lifetime (step 7) |
| `include/aowis/model/units/conversion.h` | GUI compatibility **forwarder** to Model submodule's authoritative converter (step 4); no GUI-local physical scales | **W** | Remove forwarder when old tracked file can be deleted, check actual include resolution across build variants (step 9) |
| `src/import/epanet_js_units.cpp`, Model's unit registry | Central physical conversions with source-specific EPANET semantics | **B** | Compare against *matching* Model commit and EPANET/MSX adapter (step 6) |

## 2. Inspectors and engineering input/output (original phase 8)

| User-visible surface | Quantity and stored/source representation | Implementation / refresh | Status | Required regression / remaining check |
| --- | --- | --- | --- | --- |
| Shared node and Demand Point positions — `src/entity_inspector/entity_inspector_widget.cpp`, `coordinate_property_binding.h`, `coordinate_numeric_spin_box.h` | WGS84 latitude/longitude (degrees), optional altitude | Independent axis bindings; commit merges changed axis with current Model pair | **W** | Editing one coordinate must preserve the other axis and altitude bit-for-bit; show/hide must not write (step 7) |
| Node/reservoir/tank elevation group — `entity_inspector_widget.cpp` | Terrain elevation, offsets, hydraulic elevation/head in metres as declared | `UnitNumericSpinBox` + `UnitAwareView`; derived values reread source | **W** | Check derived total, terrain fetch, changing entity during edits (step 7) |
| Junction and Demand Point base demands — shared inspector | Canonical flow (`m³/h`), demand categories and derived totals | Bound demand inputs and view-scoped demand-dialog/summary refresh | **W** | Row recreation, hidden demand dialog, editing category after profile change (step 7) |
| Junction emitter — `emitter_coefficient_binding.h`, `emitter_numeric_spin_box.h`, shared inspector | Coefficient stored as `(m³/h)/m^n`; exponent `n` dimensionless | Compose flow/head scales using *current exponent*; invalidate coefficient draft if exponent changes | **W** | Concurrent exponent edits, rejection, US/metric conversion and exact untouched value (steps 6–7) |
| Node quality input fields — shared inspector | Chemical concentration `mg/L`; water age `h`; source concentration / source mass-flow according to selected quality mode | Unit-aware spin boxes with mode-dependent refresh | **M** | Validate every chemical/age/trace/source mode, including disabled fields and `mg/min` mass source (step 7) |
| Pipe inspector — `src/entity_inspector/entity_inspector_pipe.cpp` | Pipe diameter `mm`, length `m`, Hazen-Williams roughness (dimensionless), Darcy roughness `mm`, loss coefficient, leakage, reaction rates | Typed bound controls, branch-specific refresh | **W** | Headloss formula switch, zero-/first-order wall reaction, leakage property boundaries (step 7) |
| Tank inspector — `src/entity_inspector/entity_inspector_tank.cpp` | Levels/diameter `m`, area `m²`, minimum/maximum volume `m³`, mixing/reaction values | Unit-bound editors; step-4 cleanup removed conflicting metric spin-box presentation | **W** | Check each calculated/fixed tank geometry option and volume curve, round trips (step 7) |
| Pump inspector — `src/entity_inspector/entity_inspector_pump.cpp` | Power `kW`, speed ratio, energy price per kWh, control thresholds/time/flow/head | UnitNumericSpinBox/energy price editor, demand-driven control table | **W** | Each control condition, time input, cost currency/energy denominator (step 7) |
| Valve inspector — `src/entity_inspector/entity_inspector_valve.cpp` | Diameter `mm`; valve setting in pressure head/flow/loss/percentage by valve type | Separate typed editors, active stack refresh | **W** | PRV/PSV/PBV/FCV/TCV/GPV/PCV switching and no stale last-value writes (step 7) |
| Quality simulation readouts / hydraulic simulation readouts — shared inspector | Hydraulic head/pressure, demand/flow, tank volume, velocity, head loss, power/efficiency, concentration/water age | `SimulationInspectorUnitPresentation` + view refresh for active result region | **M** | Verify quantities for every result field/quality mode; a canonical suffix in caller metadata isn't necessarily shown unchanged (step 7) |
| Per-entity special pages — `entity_inspector_{junction,reservoir,demand_point}.cpp` | Shared base property editors and extra entity-specific displays | Most numeric editing delegates to shared inspector | **M** | Follow the actual dispatch/selection paths. Current Water Meter page is identification/installation metadata, not a numeric consumption-reading UI; future meter readings/device data require their own quantity coverage (steps 7/9) |
| Inspector host — `src/entity_inspector/entity_inspector_dock.cpp` | Widget lifecycle / selection, not a separate physical quantity | Hosting surface | **I** | Ensure selected page receives generation changes once; no duplicate subscription |

## 3. Entity tables, filters and list containers (original phase 9)

| User-visible surface | Quantity and numeric authority | Implementation | Status | Remaining check |
| --- | --- | --- | --- | --- |
| `src/network/hydraulic_entity_table_widget.cpp` — all entity-type columns | Demand/flow, elevation/head, pipe size/length/leakage, quality, tank volume, pump power/energy, valve setting | `TableColumn` contains typed `ColumnUnits` and `ColumnFilterKind`; `QAbstractTableModel::data(DisplayRole)` converts canonical `SortRole` values on request | **W** | Verify each column declaration and result-mode switch against a live profile (step 7) |
| Same model — heterogeneous valve setting, reactions and energy price | Per-row quantity, reaction order and special price denominator | `TableCell::quantity`, reaction metadata, `ColumnKind::EnergyPrice` | **W** | Correct tooltip and column heading in each type/order (step 7) |
| Table numerical filters | Stored canonical min/max, active display unit in spin boxes; text and enum filters independent | Explicit `columnUnits` metadata; profile-switch signal blocking; canonical sort/filter | **W** | Edit just one bound, switch profile during pending edit, verify range semantics (step 7) |
| Table title and localized labels | Non-authoritative visible text; may contain bracketed canonical-unit metadata | `formattedTitle` changes displayed symbol; quantities no longer inferred from a column title | **W** | Localize/rename misleading title, still get same numerical conversion (step 7) |
| Table output fallback (`cell.display`) | Original preformatted string for cells with no typed quantity or failed conversion | Returned only when typed path cannot convert / isn't numeric | **M** | Review all untyped numeric columns and conversion failures; don't silently show canonical text under a selected noncanonical heading (step 7/9) |
| `src/tabs/tab_{junctions,reservoirs,tanks,pipes,pumps,valves,demand_points}_widget.cpp` | Hosts for network tables and inspectors | Container/navigation, not independent physical conversion | **I** | Ensure no bypassed secondary table view (step 7) |
| `src/tabs/tab_{customers,energy}_widget.cpp` | Customer and energy interfaces | No separate quantity conversion inferred from these container sources | **V** | Check current screens and future meter/energy additions before declaring coverage (step 7) |
| Clipboard, CSV-like user exports and future editable table cells | Human-facing display vs explicit machine-format data | No end-to-end clipboard/export provenance check completed in this GUI-only source audit | **V** | Check whether copying reads `DisplayRole` vs `SortRole`, includes units and preserves precision (steps 6–7) |

## 4. Map Monitor, Map Editor and browser presentation (original phase 10)

| User-visible surface | Quantity / ownership | Implementation | Status | Remaining check |
| --- | --- | --- | --- | --- |
| Qt scale bar — `src/map/core/map_scale_distance.h`, `map_scale_renderer.h` | Canonical viewport distance `m`; active display length profile | Nice display-scale selection `m/km/ft/mi` as appropriate, geometry still metres | **W** | Large/small zoom, 2D/3D, viewport resize (step 7) |
| Map scale bar (desktop and WASM) — `src/map/core/map_scale_distance.h`, the bgfx HUD scale panel (`src/map/bgfx/map_bgfx_hud.cpp`), the globe scale (`src/map/bgfx/map_bgfx_globe_hud_controller.cpp`) | Same canonical metres; C++ chooses `1/2/5` steps in the profile's distance unit and localized labels | Drawn by the bgfx HUD on every platform; no JavaScript involved | **W** | Profile change without map reload, active owner, resize/zoom, non-metre profiles, WASM runtime (step 7) |
| Monitor camera HUD — `src/map/monitor/map_hud_length_units.h`, `src/map/bgfx/map_bgfx_globe_hud_controller.cpp` | Camera distance, above-ground offset and derived metres | Format from profile; navigation slider positions/map state not rewritten | **W** | Hidden HUD, profile change during map gestures, coordinate view-mode switches (steps 7–8) |
| Monitor symbology — `src/map/core/map_symbology_length_units.h`, `map_navigation_widget.cpp`, `map_network_overlay_widget.cpp` | World-space icon/node/link size in `m` vs intentional `px` sizes | Explicit size-unit choice and display-only length labels/tooltips | **M** | Rendering geometry must not rebuild from a *display* unit change alone; world m vs px must stay distinct (steps 7–8) |
| Map Editor canvas — `src/map/editor/map_canvas_widget.cpp` | Viewport scale and editor geometry in mapped metres/coordinates | Unit-aware redraw of scale label; geometry remains source-native | **W** | Editor/Monitor switching, measurement/placement widgets, offscreen nested visibility (step 7) |
| Map legend dock and overlay legend — `src/entity_inspector/entity_map_legend_dock.cpp`, `map_legend_unit_presentation.h` | Node/link/heatmap ranges; quantities depend on selected simulation metric | Unit-aware ranges, titles, palettes; input metric descriptors still carry canonical symbols | **M** | Validate every metric's quantity mapping and range/legend consistency; no simulation recomputation (steps 7–8) |
| `src/map/render/*`, `src/map/bgfx/*`, tile/terrain repositories | Render geometry, DEM, camera transforms, picking, shader variables; declared metre/pixel/CRS conventions | Physical/graphics spaces, *not* active human display units | **I** | Assert display-only changes don't alter terrain/network simulation or geometry caches (step 8) |
| `src/app/footer_statusbar.cpp` WGS84, UTM/UPS | WGS84 coordinate degrees; projected easting/northing metres per CRS; map zoom is a dimensionless scale parameter | Fixed coordinate readout precision and CRS-unit labels | **I** | A separate coordinate-format policy would be new scope; coordinate readout isn't pressure/length-profile I/O |

## 5. Simulation, diagnostics and reports (original phase 11)

| User-visible surface | Quantity / data contract | Implementation | Status | Remaining check |
| --- | --- | --- | --- | --- |
| `src/entity_inspector/entity_inspector_widget.cpp` simulation/quality sections | Multiple result quantities in canonical solver-to-GUI values | Central `setSimulationValue()` quantity mapping; quality-mode readouts and view-scoped invalidation | **M** | Exhaustively test node/link subtype, stale results, water-age/trace and numeric extremes (step 7) |
| `src/simulation/simulation_inspector_unit_presentation.h` | Read-only canonical simulation values | `UnitTablePresentation::toDisplay` and selected unit symbol | **W** | Display-unit change must never mutate result timelines (steps 7–8) |
| `src/simulation/simulation_statistics_dialog.cpp`, `simulation_statistics_unit_presentation.h` | Hydraulic head error (m) and flow change (m³/h), time metadata | Unit-aware statistics formatter, visible-page refresh | **W** | Dialog remains open during unit switch; timeline/time format doesn't accidentally become length (step 7) |
| `src/simulation/simulation_manager.cpp` | Solver invocation and native EPANET textual reports | Orchestrates solver contract rather than recoding report text | **B** | EPANET-native reports retain their explicitly configured units; don't relabel report values without converting (step 6) |
| `src/simulation/simulation_diagnostics_{dialog,widget}.cpp` | Diagnostic messages and codes supplied by solver | Mostly opaque text, not structured unit-bearing measurements | **B** | Future structured measurement diagnostics need declared units (step 6) |
| Future charts/time-series visualizations (if present outside audited display paths) | Axes, legends, plot cursor, quality/rate/time quantities | No universal plot presentation binding established in this snapshot | **V** | Before closure, inspect any instantiated chart or additional report outputs; no claim of absent functionality (step 7) |

## 6. Imports, exports, persistence and databases (original phase 12)

| Boundary | Unit authority | Status | Required cross-boundary check |
| --- | --- | --- | --- |
| `src/import/epanet_js_units.cpp`, `epanet_js_project_converter.cpp`, source-specific helper files | EPANET-JS/EPANET schema units → AOWIS property storage conventions; independent of GUI selection | **B** | Repeat source-unit vocabulary/pressure/emitter/reaction/quality comparisons on current matching Model/EPANET revisions (step 6) |
| `src/import/epanet_js_unit_profile.h`, import/profile UI | Source-derived *optional* custom display profile, separate from import conversion | **B** | Create/name/activate checkbox, persisted custom mapping, no physical Model rewrite (step 6) |
| `src/simulation/simulation_manager.cpp` EPANET export | Explicit EPANET engine/file units, not GUI display profile by implication | **B** | Export same network with different GUI profiles and compare equivalent network/solver semantics (step 6) |
| Project/network serialization, Model/EPANET/DB repositories | AOWIS canonical storage and external protocol semantics | **V** | Matching Model, EPANET wrapper and DB snapshots not supplied. Verify SQLite **and** PostgreSQL; if a shared representation is impossible, document the incompatibility (step 6) |
| `src/config/unit_profile_storage.cpp` | Personal display-profile selection, not physical project measurement storage | **B** | Changes must not create model network revisions/solver invalidations (step 7) |

**Step-6 boundary verification:** See [unit-external-io-verification.md](unit-external-io-verification.md) for the exact EPANET INP/EPANET-JS test contracts, declared export limitations, and the unverified SQLite/PostgreSQL/REST and EPANET/MSX repository boundaries. The GUI's SQLite `.ejsdb` *input format* must not be confused with AOWIS persistence backends.

## 7. Numerical controls intentionally outside engineering unit switching

| Source | Quantity | Disposition |
| --- | --- | --- |
| `src/widgets/slider_number_control.{h,cpp}`; four controls in `src/settings/map_settings_widget.cpp` | Mouse zoom/pan/orbit multipliers, terrain detail reduction | **I.** Independent precise value now exists; slider/spin box are lossy presentations. Covered by `tests/slider_number_control_test.cpp` (within editor-adapter test target). |
| `src/app/footer_statusbar.cpp`, `src/tabs/tab_map_editor_container.cpp` | Map zoom (floating/integer index) | **I.** MapModel owns zoom. Display is not a length profile. |
| `src/map/core/map_navigation_widget.cpp`, `src/tabs/tab_map_monitor_container.cpp` | Icon px sizes, alpha/opacity percentages, symbology controls | **I/M.** Treat `m` world-size alternatives as physical and `px`/opacity as UI settings. |
| `src/map/bgfx/map_bgfx_globe_hud_controller.cpp` | Camera tilt degrees, vertical exaggeration multipliers, percentages | **I.** Not hydraulic pressure, flow or volume. Physical camera distance is separately unit-aware. |
| `src/map/bgfx/*` (including its shaders), `src/map/render/*` | Pixel size, world-space geometry, shader scales | **I.** Do not convert renderer or CRS internal data because of display profile. |
| Time-of-day, identifiers, flags, WGS84/UTM identifiers | Domain-specific time and geographic reference fields | **I/M.** Elapsed water age/time measurements may be configurable; clocks/IDs are not automatically convertible physical quantities. |

## 8. Test and evidence inventory

**Reported Linux validation:** 21/21 CTest targets pass. This audit has not independently built Qt, Windows, WASM or the application binary. These source-level test targets explicitly exercise unit behavior:

| Tests/files | Verified target area in source | Missing evidence |
| --- | --- | --- |
| `unit_conversion_test.cpp`, `unit_model_header_authority_test.cpp` | Conversion semantics and forwarding to Model authority | Matching external repository revisions, all import/export contracts |
| `unit_profile_storage_test.cpp`, `unit_aware_view_test.cpp` | Profiles, lazy-generation lifecycle; unconditional checks in Release | Complex application/detached-window lifecycles |
| `numeric_property_binding_test.cpp`, `numeric_editor_adapters_test.cpp`, `unit_numeric_spin_box_test.cpp`, `numeric_text_test.cpp` | Precision/no-op edits, controls, text formatting | Full inspector interaction for every entity type |
| `coordinate_property_binding_test.cpp`, `coordinate_numeric_spin_box_test.cpp` | Independent geographic axis edits | Production node/Demand Point navigation; Undo/Redo is deferred to the History subsystem |
| `emitter_coefficient_binding_test.cpp`, `emitter_numeric_spin_box_test.cpp` | Exponent-dependent coefficients; canonical value preservation | Whole-network emitter constraints and solver export |
| `unit_table_presentation_test.cpp` | Explicit quantity metadata, filtering and localized headings | All production columns, clipboard and user export |
| `map_render_neutral_tests.cpp` ("map scale unit presentation", "map HUD length presentation") | Profile-defined readable map scales and HUD lengths | Actual WebAssembly browser runtime / ownership |
| `slider_number_control_test.cpp` | Legacy UI-only slider precision, signal behavior | End-to-end saved settings and application UI |

Step-7 regression/Debug–Release and cross-platform test instructions are tracked in [unit-regression-platform-verification.md](unit-regression-platform-verification.md). **Do not count WebAssembly as passed without actual runtime results.** Windows: reported working by the user, 14 November 2026.

**Step-8 performance verification:** The CTest-registered synthetic benchmark and pending production acceptance matrix are documented in [unit-presentation-performance-verification.md](unit-presentation-performance-verification.md). **Final nine-step status and unverified gates:** [unit-system-final-acceptance.md](unit-system-final-acceptance.md). **No actual performance acceptance evidence** is contained in this snapshot. Steps 7–8 should measure visible-only table repainting, hidden-region generation coalescing, map geometry cache rebuild counts (ideally zero), simulation rerun count (zero), and model property writes (zero) during profile switches. Test small and large networks in both 2D/3D and WASM as appropriate.

## 9. Closure matrix for the nine-step finishing roadmap

| Step | Current snapshot status | Basis / needed to close |
| --- | --- | --- |
| 1. WASM scale bar | **Source-implemented** | C++ selection and browser JS control + two scale tests; browser runtime acceptance outstanding |
| 2. Table metadata | **Source-implemented** | Explicit column/row quantity and filter kind + table tests; production-column/runtime audit still needed |
| 3. Precision-safe legacy sliders | **Source-implemented** | Independent precise value and control tests; manual saved-settings scenario pending |
| 4. Remove duplicate/obsolete unit infrastructure | **Source-implemented** | Forwarder, corrected Settings copy, tank control cleanup, Model-header-authority test |
| 5. Coverage inventory | **Documented in this file** | GUI source coverage established; multi-repository reconciliation belongs to step 6 |
| 6. External I/O verification | **Partially evidenced** | EPANET-JS tests and source-specific semantics exist; current Model/EPANET/DB/REST export round trips not certified |
| 7. Regression/platform acceptance | **Linux CTest reported passing** | Release + WASM runtime and UI interactions outstanding (Windows: reported working by the user, 14 November 2026) |
| 8. Performance verification | **Not evidenced** | Profile-switch throughput, large tables, map cache rebuilds, repeat-switch tests |
| 9. Final sign-off | **Final audit recorded; acceptance still open** | See [final acceptance ledger](unit-system-final-acceptance.md): Linux suite reported 21/21, but step 6 cross-repo, step 7 platforms, step 8 measured performance remain pending |

### Audit workflow for new UI quantities

1. Name the **semantic quantity** and its **stored Model representation**; don't guess from suffixes or variable names.
2. Choose **Model registry unit conversion**, a specialized source/derived-dimension conversion, or **explicitly no display conversion** (CRS, pixel, dimensionless UI, opaque report).
3. Declare formatter/column metadata and **canonical read/write boundary**. Never parse the view's rounded value to reconstruct unrelated properties.
4. Attach **one appropriate view-level lazy invalidation** or demand-driven renderer rather than subscribing every field; prevent refresh writes.
5. Add a unit-switch/no-op/full-precision test, plus relevant import/export and runtime acceptance coverage.
6. Append a row here. Don't mark all of a whole file or directory complete based only on a string search.

### Useful search commands (from the GUI repository root)

```sh
rg -n 'UnitAwareView|UnitPresentationContext|UnitNumericSpinBox|NumericPropertyBinding|UnitTablePresentation|UnitProfileManager' src
rg -n 'new (QDoubleSpinBox|QSpinBox|QSlider|SliderNumberControl)|setSuffix\(' src
rg -n 'm³|mg/L|m/s|kPa|psi|kWh|mm|\[m\]| km| ft' src tools/qt-emscripten
rg -n 'toDisplay|formattedTitle|DisplayRole|SortRole|setScaleChoices' src/network src/map tools/qt-emscripten
```

A literal canonical suffix in a hydraulic table cell constructor, source-unit parser, or EPANET report is **not** sufficient to conclude the final visible value is unit-unaware. Conversely, a typed formatter's existence does **not** prove every live GUI path uses it. This matrix records both distinctions.
