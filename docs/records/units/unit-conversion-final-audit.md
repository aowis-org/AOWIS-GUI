# Unit conversion consolidation — final boundary audit

Verified against `epanet-js/epanet-js` @ `fa898b5` (2026-10-08) and OpenWaterAnalytics EPANET `v2.3`–`v2.3.5` and `dev` @ `47c5b8a`.

## Ownership

- `AOWIS-SERVER-MODEL/include/aowis/model/units/conversion.h`: physical definitions (length, volume, flow, pressure, time, standard gravity, reference water density, mechanical horsepower) and explicit canonical conversions. Header-only; no Qt or EPANET dependency.
- `AOWIS-SERVER-GUI/src/import/epanet_js_units.cpp`: epanet-js unit spellings, EPANET 2.3 interpretation (pressure, emitter basis, Darcy-Weisbach roughness, water age), conversion into AOWIS canonical representations. Physical scales delegate to MODEL; the only literals left are metric prefixes (`10.0`, `1000.0`, `0.001`).
- `AOWIS-SERVER-GUI/src/import/epanet_js_project_converter.cpp` (`resolveProjectUnits`): the single place where missing unit keys are resolved. Conversion stages read exactly one unit key each and apply no fallbacks of their own.
- EPANET wrapper and bundled upstream EPANET: keep their own format and engine compatibility semantics until a separate compatibility change with reference comparisons. Do **not** mechanically replace historical EPANET factors with physical constants.

## epanet-js unit vocabulary

Source: `libs/quantity/src/index.ts`, `libs/project-settings/src/quantities-spec.ts`, `libs/ejsdb/src/schema/project-settings.ts`.

| Quantity | Spellings epanet-js writes | EPANET unit system |
| --- | --- | --- |
| Flow (SI) | `l/s`, `l/min`, `Ml/d`, `m^3/h`, `m^3/d` | LPS, LPM, MLD, CMH, CMD |
| Flow (US) | `gal/min`, `ft^3/s`, `Mgal/d`, `IMgal/d`, `acft/d` | GPM, CFS, MGD, IMGD, AFD |
| Pressure | `mwc`, `fwc`, `psi`, `kPa`, `bar` (independent of flow system) | METERS, FEET, PSI, KPA, BAR |
| Length / elevation / head / level | `m` / `ft` | follows flow system |
| Diameter | `mm` / `in` | follows flow system |
| Volume | `m^3` / `ft^3` | follows flow system |
| Power | `kW` / `hp` | follows flow system |
| Concentration | `mg/L`, `ug/L` | — |
| Water age | `h` | — |
| Roughness, emitter coefficient, minor loss | `null` (EPANET native) | — |

The flow unit alone selects the EPANET unit system (`build-inp.ts chooseUnitSystem`); all other quantities follow the presets, except pressure, which is chosen separately. `l/h`, `l/d`, `gal/d`, `ft^3/d` and `km` exist in the schema enum but are not used by any quantity AOWIS imports (`customerDemandPerDay` only). `Ml/d` and `Mgal/d` are matched case-sensitively before case folding, because `ml/d` would be millilitres.

epanet-js writes stored values unconverted into the INP together with `Units`, `Pressure` and `Specific Gravity`, so EPANET's own interpretation is authoritative.

## EPANET 2.3 semantics the importer follows

Source: `src/input1.c` `initunits` / `convertunits`, `src/qualreact.c` `wallrate`, `src/input3.c` reaction options.

- **Pressure.** `METERS` (`mwc`) and `FEET` (`fwc`) are head of the simulated fluid: `pcf = MperFT`, `pcf = 1.0`. Specific gravity does not apply. `PSI`, `KPA`, `BAR` are true pressures scaled by `SpGrav`. EPANET 2.2 still used `MperFT * SpGrav` for METERS; epanet-js offers FEET and BAR and therefore runs 2.3.
- **Emitters.** Independent of the `Pressure` option: flow units per psi^n (with `SpGrav`) for US systems, per metre of head^n for SI systems (`ecf = US ? PSIperFT * SpGrav : MperFT`).
- **Darcy-Weisbach roughness.** millifeet (US) or millimetres (SI): `Kc /= 1000 * Ucf[ELEV]`.
- **Wall reactions.** Order must be 0 or 1 (`input3.c` error 213; `EN_setoption` also rejects other values). First order is length/day; zero order is mass/area/day with the declared chemical mass unit (`kw * Ucf[ELEV]^2`).
- **Bulk reactions.** Per day; order < 0 selects Michaelis-Menten kinetics.
- **Chemical mass unit.** `simulation_settings.qualityMassUnit` is what epanet-js passes to `QUALITY`; `units.chemicalConcentration` is only copied from it when a project is created and can go stale. The importer uses `qualityMassUnit` when present.

## API and semantics

- `feetToMetres` uses international feet. `usSurveyFeetToMetres` is distinct.
- `usGallonsToCubicMetres` means US liquid gallons; imperial gallons are used only in their explicitly named flow conversion.
- Canonical flow is m³/h, canonical diameter is mm, canonical pressure is head of the simulated fluid in m; this is not a general SI-base-units type system.
- `pascalsToMetresHead` requires explicit density (kg/m³) and gravity (m/s²); it does not assume a fluid or EPANET pressure convention.
- Physical conversion functions operate on `double` and do not validate nonfinite inputs; format boundaries reject inappropriate inputs.

## Regression policy

`unit_conversion_test.cpp` checks independent numerical references and round trips, and must pass in Release/NDEBUG builds (no `assert`). The GUI EPANET-JS converter tests cover every epanet-js flow-preset spelling, pressure and emitter semantics with specific gravity ≠ 1, unit inference, the quality mass-unit precedence, and the metric/US fixtures. Windows and WASM builds remain unverified here.

## Scope closure

No solver-conditioning, physical uncertainty, or hydraulic-confidence work belongs to this unit-conversion cleanup. The EPANET wrapper's INP/report import, geometry, CRS transforms (US survey feet), energy thresholds and MSX diffusivity are not part of this audit; review each with independent compatibility fixtures before migration.
