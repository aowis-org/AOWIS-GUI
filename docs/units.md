# Measurement units and numeric editing

AOWIS keeps **canonical hydraulic MODEL values** separate from the user's
**display unit profile**. Changing a profile changes labels, formatted
values and editors; it must never rewrite a hydraulic network, create a
MODEL edit event, cause a Commit or rerun a simulation.

The source for physical quantity definitions and conversions is the MODEL
unit registry. The GUI owns the chosen display profile and Qt presentation.
The source format of an EPANET/EPANET-JS import or export is a separate
contract, not something inferred from a GUI dropdown.

## Selecting and customizing profiles

The GUI provides built-in and custom **Unit Profiles** in the Units settings
widget and an active-profile selector in the main toolbar. A profile maps
quantity types (length, pressure, flow, energy and others) to display units.
Custom profiles and the chosen personal display preference are stored as
GUI settings; they are not hydraulic-network data.

Profile changes notify visible unit-aware views. Hidden views defer the
refresh until they are next shown instead of traversing and rebuilding
all windows. Quantity values remain owned by their model or binding, never
by an already rounded text label.

## Numeric editing

A focused field can contain an unfinished edit in the units that were
visible when typing began. Changing the global profile **does not silently
commit or reinterpret that text**. A successful explicit edit converts from
its pinned input unit to the canonical MODEL unit; Escape cancels and
reloads the current value. Invalid or incomplete edits do not write the
model. Dependent quantities, such as emitter coefficients with an exponent,
require their own typed binding rules rather than a bare unit suffix.

Normal Qt text-field editing and application-level network History are
different concepts. The current History dock navigates completed, saved
MODEL editing actions; its proposed keyboard shortcuts must not steal
focused text-field Undo/Redo.

For implementation contracts see [numeric transactions](numeric-editor-transactions.md)
and [numeric formatting](numeric-formatting.md).

## Where units appear

| Surface | Rule |
|---|---|
| Hydraulic inspector | Explicit quantity binding; no hidden write from refresh or selecting an entity |
| Entity tables and filters | Format for display, but sort/filter using canonical typed values |
| Map Monitor and Editor | Scale/HUD labels follow length profile; terrain/network geometry stays in its own physical coordinate system |
| Simulation results | Format solver measurements by declared quantity; changing display profile does not rerun the solver |
| Imported/exported projects | Convert according to the external format's explicit units, independently of GUI preference |
| UUIDs, timestamps, pixels, zoom, CRS coordinates | Not automatically converted as hydraulic quantities |

Some less-common conditional fields and cross-platform display paths still
require targeted runtime verification. The detailed
[unit coverage matrix](unit-gui-coverage-matrix.md) and
[unit-system acceptance evidence](unit-system-final-acceptance.md) document
technical reviews performed on earlier snapshots; they must not be
mistaken for current GUI/EPANET/DVMDB release acceptance.
