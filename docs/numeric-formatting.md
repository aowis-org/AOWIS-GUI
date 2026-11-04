# Numeric formatting and parsing

NumericText operates on values already converted to the selected display unit.
Model unit conversion remains authoritative and never rounds. A formatting
policy chooses fixed fractional digits, adaptive significant digits (Qt g), or
scientific notation, grouping, and negative-zero normalization. No formatting
operation writes model data.

UnitPresentationContext stores a default policy, optional per-quantity overrides,
and a locale. Its existing generation changes when effective presentation
settings change. UnitAwareView therefore refreshes visible views and coalesces
hidden updates for formatting as well as unit changes. A field may override the
quantity policy explicitly when building its presentation. Persistence and the
settings UI for these policies belong to the later settings phase.

Pass registry-provided display symbols to NumericText::measurement, suffix, and
heading; the utility does not introduce another unit registry. Empty symbols
produce dimensionless output. Percentages must first be converted from their
canonical ratio by the Model converter; formatting/parsing does not multiply or
divide by 100. Values and symbols use a nonbreaking space.

Parsing uses only the specified locale and grouping policy. An optional exact
unit suffix may be accepted; wrong units and non-finite values are rejected.
There is no fallback to the C locale. ParseResult distinguishes valid values,
missing input, invalid text, non-finite input, and overflow reported by Qt.
Missing display data uses an em dash, NaN uses NaN, and infinity uses the infinity
symbol. Invalid format policies return nullopt. Those markers are never edits.

NumericLineEdit uses the shared formatter/parser and pins locale and policy for
pending edits. Existing refresh(unit, significant_digits) calls remain supported.
Spin boxes retain Qt's native fixed-decimal validation and rendering and expose
a grouping flag in NumericEditorPresentation. Callers derive suffixes using the
shared suffix helper and decimals from the appropriate fixed policy. Other
notations are supported by NumericLineEdit and read-only text surfaces.

External serialization must retain its format-specific number conventions and
units; do not use localized GUI text for databases, APIs, or solver input.
For current per-surface unit coverage and remaining runtime verification,
see the [unit-system inventory](unit-gui-coverage-matrix.md) and
[verification status](verification-status.md).
