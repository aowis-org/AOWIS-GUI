# Legacy numeric controls — precision audit

## SliderNumberControl

The four callers are GUI settings in `map_settings_widget.cpp`:

| Control | Meaning | Slider edit quantum |
| --- | --- | --- |
| Mouse wheel zoom sensitivity | Navigation multiplier | 0.01 |
| 3D pan sensitivity | Navigation multiplier | 0.01 |
| 3D orbit sensitivity | Navigation multiplier | 0.01 |
| Terrain detail reduction | Integer DEM levels below the imagery (0..3) | 1 |

These controls do not use physical unit profiles. Saving one group reads all its
controls, so an untouched control must return its precise supplied value even when
its spin box displays fewer decimals.

`SliderNumberControl::value()` now returns independent authoritative storage.
`setValue()` silently replaces that storage and supersedes pending input. Finite
values outside the presentation range are retained; the controls show a clamped
view. Invalid nonfinite values/ranges are ignored. `setRange`, `setDecimals`,
`setSingleStep`, `setSuffix` and refreshes never clamp or round stored values.

The existing `NumericDoubleSpinBox` and `NumericSlider` adapters each have their
own `NumericPropertyBinding` referencing the same storage. They recognize genuine
user edits, preserve unchanged rounded input, and prevent programmatic child
updates from becoming writes. Accepted edits refresh the peer and emit one
`valueChanged` notification. Display changes preserve spin-box drafts; an explicit
owner update cancels them. An accepted slider edit replaces a competing spin draft.

`setSliderStep()` specifies intentional slider quantization independently from
spin-box decimals and arrow step size. The six callers explicitly preserve their
previous slider granularity. Keyboard/page increments cross at least one quantum.
Bindings outlive their adapters, including during destruction.

## Other reviewed control families

| Family | Authoritative value / disposition |
| --- | --- |
| Hydraulic inspector measurements | Existing unit-aware property bindings, including coordinates and emitters; no legacy slider pair |
| Table numeric filters | Separate canonical selected bounds; refresh blocks signals and editing one bound retains the other |
| Map Monitor distance, tilt, ground offset, exaggeration HUD | MapModel owns values; slider synchronization blocks signals; physical distance labels already use unit presentation |
| Monitor icon sizes | Separate integer metre/pixel configuration; display-unit updates change tooltips only |
| Map opacity and Editor icon size | Intentional integer percentage UI controls |
| Footer zoom spin box | MapModel owns zoom; `setMapZoom` blocks signals, and the displayed value is not used to refresh/save unrelated settings |

No physical quantity was invented for pixels, zoom or navigation multipliers.

## Regression coverage

`slider_number_control_test.cpp` runs inside the existing
`aowis-numeric-editor-adapters` target. CMake includes the production control and
its Q_OBJECT header for AUTOMOC. The suite covers exact-value retention, reduced
precision, range clamping/restoration, collapsed ranges, unchanged focus/text,
programmatic child updates, user typing/stepping, slider edits and integer keyboard
steps, signal counts, invalid setter input and draft lifecycle.
