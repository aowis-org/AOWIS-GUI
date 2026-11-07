# Unit conversion inventory (step 1)

> Historical EPANET-JS/source-conversion inventory. For the current GUI-wide measurement display/edit coverage matrix (snapshot `gui(20261011-162011).zip`), see [unit-gui-coverage-matrix.md](unit-gui-coverage-matrix.md).


Scope: supplied snapshots `gui(5).zip`, `model.zip`, and `epanet(20261009-154953).zip`. This is an inventory only; no runtime conversion behavior is changed. Locations are relative to their respective repository roots.

## GUI: EPANET-JS boundary

| Location | Responsibility / conversion | Observation |
|---|---|---|
| `src/import/epanet_js_units.h`, `.cpp` | `normalizedUnit`, `flowToM3PerH`, `lengthToM`, `volumeToM3`, `diameterToMm`, `darcyRoughnessToMm`, `chemicalConcentrationScaleToMgPerL`, `pressureToHeadM`, `emitterCoefficientToCanonical` | Main existing conversion adapter; combines unit parsing, physical scales and EPANET-JS conventions. |
| `src/import/epanet_js_units.cpp:40` | MGD to m³/h `157.725491` | Rounded versus US gallon definition; replace in later step, with regression tests. |
| `src/import/epanet_js_units.cpp:164-169` | psi/kPa/bar to pressure head using `0.4333`, `6.895`, `0.068948` | Approximate chained factors; requires decision about EPANET compatibility versus physical conversion before changing. |
| `src/import/epanet_js_units.cpp:34-45` | CFS, GPM, MGD, IMGD, acre-ft/day to m³/h | Mix of exact-derived decimal factors and rounded constants; centralize. |
| `src/import/epanet_js_units.cpp:53-92` | length, volume, diameter | Exact-defined feet, inches, US gallons are repeated as literals. |
| `src/import/epanet_js_units.cpp:116-149` | Darcy roughness and inferred unit from flow-unit family | Unit inference is **format-specific**, should remain in adapter. |
| `src/import/epanet_js_units.cpp:174-204` | emitter coefficient | Exponent-dependent composite conversion, not a simple unit factor; retain format-specific logic, delegate primitive scales. |
| `src/import/epanet_js_settings.cpp:455` | Source feet-to-meters `0.3048` | Duplicated outside units adapter. |
| `src/import/epanet_js_water_quality.cpp:87` | Source feet-to-meters `0.3048` | Duplicated outside units adapter. |

## EPANET integration

| Location | Responsibility / conversion | Observation |
|---|---|---|
| `src/lib/internal/epanet_inp_report_importer.cpp:19-32` | Feet, pressure, CFS and EPANET flow-unit family | Parallel conversion table; contains rounded pressure factors `0.4333`, `6.895`, `0.068948` and approximated flow ratios. |
| `src/lib/internal/epanet_inp_importer.cpp:28` | Feet-to-meters | Duplicated `0.3048`. |
| `src/lib/internal/epanet_inp_geometry_importer.cpp:23` | Feet-to-meters | Duplicated `0.3048`. |
| `src/lib/internal/epanet_result_reader.cpp:335-337` | Feet, CFS to m³/h and tiny-flow threshold | Conversion literal duplicated; threshold is an algorithmic policy, not a unit constant. |
| `src/lib/internal/epanet_hydraulic_solver.cpp:86` | Minimum energy flow threshold expressed via CFS conversion | Separate algorithmic threshold from reusable unit scale. |
| `src/lib/internal/epanet_msx_units.h:8-10` | Square feet/cm to m², reference diffusivity | Area scales are unit conversions; diffusivity reference is model-specific. |
| `src/lib/internal/epanet_coordinate_reference_transform.cpp:19` | US survey foot `1200/3937` | **Distinct** from international foot `0.3048`; must remain explicitly distinguished. |

## Model repository

No dedicated, general-purpose units/conversion module was identified in the supplied model snapshot. Before moving implementations into the model repository, validate its exported API, dependency direction and internal canonical units per quantity; do not assume all stored values are base SI (e.g. GUI adapter targets m³/h and mm).

## Migration boundaries and priorities

1. Introduce precise physical definitions and named conversions in a dependency-free shared module; preserve explicit canonical target units (`m³/h`, `mm`, `m`, `m³`, `mg/L`) as required by the model.
2. Keep source-format aliases, missing-unit inference, pressure-head semantics, emitter exponent conversion and EPANET compatibility policy in format adapters.
3. Replace the GUI adapter's rounded MGD and pressure conversions only after targeted regression tests. Pressure-head conversion needs explicit density/gravity and/or EPANET-convention decision; merely replacing numbers can change results.
4. Consolidate EPANET INP/report/result-reader conversions after verifying equivalence with the EPANET engine and existing fixtures.
5. Preserve international foot versus US survey foot distinction and do not reclassify physical/solver tolerances as unit conversion constants.

### Suggested regression matrix for step 2/3

- Flow: L/s, L/min, m³/s, m³/day, MLD, CFS, GPM, MGD, IMGD, acre-ft/day → m³/h.
- Length/diameter/volume: m, ft, in, mm, m³, ft³, US gal; separate US survey foot.
- Pressure head: m, ft, psi, kPa, bar; valid/invalid specific gravity; explicit compatibility expectations.
- Roughness: mm, millifeet, feet and missing unit inferred from flow family.
- Emitters: exponent-sensitive conversion, zero coefficient, invalid inputs.
- Full import equivalence: metric and US customary EPANET-JS and INP fixtures, plus MSX where applicable.

**Audit limitation:** This inventories the conversion sites and obvious duplicated factors in the supplied repositories; it is not a proof that every dimensional arithmetic expression in the full codebase has been found. A later migration should review call sites and additional implicit conversions before declaring complete coverage.
