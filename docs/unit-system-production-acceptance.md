# Unit-system Step 5 — production acceptance

**Baseline for this Step 5 patch:** `gui(20261011-204355).zip`.
**Status:** Automated cross-surface Qt regression added; **real production acceptance pending**.

This is the production acceptance checklist for the existing AOWIS GUI. It is
not another test runner, and it does not claim that a synthetic Qt widget
is the actual Map Monitor, the hydraulic Model or the EPANET solver.

## A. Automated precondition

Run the existing build/test workflow; do not add or use a separate verification script:

```sh
./compile_linux_tests.sh --both
```

The new `aowis-unit-cross-surface-acceptance` test (labeled `aowis;acceptance`)
uses actual `UnitNumericSpinBox`, `UnitAwareView`, `UnitTablePresentation`,
`MapHudLengthUnits`, `MapLegendUnitPresentation`, and simulation presentation
helpers in one Qt widget/test process. It asserts:

- A real Qt editor can be reformatted and focused/committed without changing
  a higher-precision canonical value or invoking its writer.
- Changing display units refreshes only the active tab; hidden tables, map-label
  pages and simulation-label pages catch up once when shown.
- A 25,000-row `QTableView` uses canonical numeric roles and viewport-limited
  formatting instead of formatting every record on a profile switch.
- Map HUD/legend and simulation labels switch units without changing their
  source numerical inputs.
- An explicit spin-box step is the **only** action in the scenario that writes
  a new canonical value.

**Limit:** This test uses a deterministic in-memory model and text presenters.
It does *not* instantiate the production hydraulic inspector, table data source,
2D/3D renderer, map terrain cache, real solver or network revision mechanism.
Those require the following live checks.

## B. Live application protocol

For every run, record GUI and Model commit IDs, OS, Qt version, profile names,
map renderer backend (bgfx with Vulkan, OpenGL, Direct3D 11, Metal or WebGL 2,
as logged at startup) and source project/fixture. Use one small
project for correctness and one representative **large network**, recording
actual nodes, links, demand points and result time steps. Do not fabricate a
required network size or a machine-independent response-time threshold.

| Gate | Procedure in the actual GUI | Pass observation | Status |
| --- | --- | --- | --- |
| Inspector precision and edits | Set a WGS84 coordinate with more digits than its editor shows. Select the entity, switch profiles repeatedly, edit **only** latitude, then reopen. Undo/Redo is a future History test. Repeat for Demand Point, and change emitter exponent during an unfinished coefficient edit. | Untouched coordinate/altitude retain original full precision; zero changes/revisions from display-only actions; explicit edits and reopening preserve intended values; future History tests must cover Undo/Redo separately. | **Not run here** |
| Active editing | Type a pending unit-bearing measurement, switch profiles before committing; also try invalid text and Escape. | Original unit is retained for the pending candidate; no silent reinterpretation or write. | **Not run here** |
| Tables | With a large entity table open, switch metric/customary, sort head/flow numerically, use unit-dependent filters, inspect valve-setting types, copy a visible selection, switch back. | Headers, display and copied text reflect selected units; canonical sort/filter semantics and selection/scroll remain stable. Heterogeneous valve settings do not get an invalid numeric filter. | **Not run here** |
| Map Monitor 2D | Run with actual network, overlay legend and HUD visible. Switch profiles while zooming and panning. | Distances/heads/flows/legends change labels; no unit-only network rebuild, terrain reload, camera jump or solver rerun. | **Not run here** |
| Map Monitor 3D | Repeat with globe, underground view, terrain labels, X-ray/flow overlays where available. | Same presentation-only behavior; geometry/spatial indexes and terrain remain unchanged. | **Not run here** |
| Map Editor | Switch profiles while selected network geometry, length readouts, map scale and edit controls are visible. | Geometry and canonical measurements unchanged; labels/readouts update. | **Not run here** |
| Simulation | Run hydraulic and water-quality simulation. Compare visible result table, statistics, map legend and any chart after profile changes. | Values represent the same canonical results; no unit-only solver rerun; water quality and units remain consistent. | **Not run here** |
| Hidden regions | Hide inspector, result panel and map sections; switch profiles many times, then reveal them one by one. | No expensive work for hidden regions during switches; each catches up once to the latest settings when shown. | **Not run here** |
| Profile persistence | Save active custom display profile, restart, reopen the same project and compare canonical data. | Profile and GUI units restored; project measurements unchanged. | **Not run here** |

For production write/rebuild guarantees, collect **actual instrumentation** or
source-level counters (hydraulic Model revision/writer, EPANET invocation count,
terrain/geometry generation counter). A visual impression of "no change" is not
evidence of zero writes or rebuilds. If those counters are unavailable, record
"not instrumented", not "passed".

## C. Performance evidence to record

Use the existing benchmark (`./compile_linux_benchmarks.sh --both`) for a
repeatable synthetic baseline. Separately record for the real application:

| Configuration | Network nodes / links / demand points | Result steps | Renderer/backend | Visible regions | Profile switches | Elapsed milliseconds | Model writes | Geometry/terrain rebuilds | Solver calls | Outcome |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Small correctness fixture | Pending | Pending | Pending | Pending | Pending | Pending | Pending | Pending | Pending | **Not run** |
| Large production network | Pending | Pending | Pending | Pending | Pending | Pending | Pending | Pending | Pending | **Not run** |

Do not compare Debug and Release elapsed times as if they were direct
performance regressions. Compare repeated runs on the same machine, workload,
backend and build type. Structural invariants (no Model writes, no hidden work,
viewport-only formatting) are hard pass conditions; timings are observational.

## D. External boundaries

This GUI-only acceptance cannot certify Windows, live browser WebAssembly,
EPANET/MSX wrapper revisions or REST/database persistence. SQLite and
PostgreSQL must be verified **separately** against the same canonical data
contract before marking DB/REST parity as passed. See
[unit-system-final-acceptance.md](unit-system-final-acceptance.md).
