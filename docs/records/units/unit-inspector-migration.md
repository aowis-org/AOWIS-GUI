# Unit inspector migration — Step 8a

Migrated junction controls: total elevation, terrain elevation, elevation offset,
demand-category base values in the Demands dialog, and the total demand summary.
UnitNumericSpinBox owns its binding and converts explicit property storage units
through the Model converter. Elevation uses metres; demand uses m3/h. This avoids
assuming that a property representation equals the registry canonical unit.

Writers retain the existing HydraulicData setters and network-editor mutation
path. Refreshes, profile selection, clamping, and no-op focus changes cannot write
these properties. Calculated total elevation reads authoritative terrain/offset
values rather than adding rounded controls. Pending edits retain their displayed
unit, precision and locale; commit/cancel adopts the latest presentation.

UnitAwareView subscriptions are attached to the elevation group, demand summary,
and demand dialog. Profile updates skip hidden demand rows. Structural demand
changes invalidate indexed rows until they are rebuilt; old row edits are
canceled before replacement. Network loads also invalidate these rows.

Included build correction: NumericSlider::refresh now explicitly calls
QSlider::value(), avoiding the local optional named value.

Validation: a new offscreen Qt target exercises the reusable production control,
metres/feet, m3/h versus L/s, hidden profile changes, exact value preservation,
explicit commits, and unavailable properties. Full inspector/application runtime
verification is still required. The target is included in aowis-tests (13 tests).

Emitter follow-up: the coefficient editor now binds canonical m3/h/m^n without
round-tripping its displayed value. Conversion composes the selected volumetric
flow and pressure-head units using the actual emitter exponent; no fixed unit
identifier is invented for the exponent-dependent coefficient. The editor
shows the resulting composite unit beneath its field. Exponent is a separate
dimensionless numeric binding. Changes to the exponent invalidate the meaning
of pending coefficient edits, so stale edits are rejected. A lazy view-level
subscription refreshes both fields and the demand summary together.

Remaining Step 8 work: other entity inspectors, DEM retrieval status text,
and remaining hydraulic editors. The emitter implementation has independent
non-Qt conversion/binding tests and an offscreen Qt widget test; full GUI
runtime verification remains necessary.
Spin boxes use fixed decimals (explicit fixed policy, or the existing field's
decimal count); significant/scientific policies are supported by text displays.
