# Numeric editor transactions

A profile change never commits an edit. Each editor retains the unit of its
current presentation. The first user change starts the binding transaction in
that unit; refresh stores the newest requested presentation without replacing
pending text, suffix, bounds, or precision. Repeated changes coalesce.

Enter attempts a commit. Focus loss attempts a commit for text editors. A valid
commit uses the original edit unit and then displays the latest requested unit.
Invalid input, rejected writes, unavailable properties, and conflicts leave the
transaction unresolved. Spin boxes preserve incomplete text across Qt focus-out
fixup. Escape cancels and reloads authoritative data in the latest requested
unit. Arrow stepping does not replace incomplete input.

During a slider drag, the conversion and numeric range stay fixed. Each movement
is an explicit edit; release applies the newest requested presentation. A
rejected movement does not write the property; release restores authoritative
presentation. Slider edits already committed during a drag are not canceled by
Escape; application undo remains responsible for reversing mutations.

Views continue to subscribe through UnitAwareView. There is no global traversal
of hidden editors or implicit pre-switch commit. This is a preservation policy:
active editors may temporarily show their original units. NumericLineEdit's
containing view must use displayUnit() for its adjacent unit label while an edit
is pending, and synchronize the label after commit/cancel. Spin boxes retain
their own suffix automatically.

Bindings must outlive controls. Before retargeting an accessor to a different
entity, the owning view must resolve or explicitly cancel its pending edit.
Writers use the application's MODEL mutation capture rather than writing
canonical data from presentation-only refreshes. Application History Undo/Redo
is separate from a focused text editor's in-progress local Qt Undo/Redo; the
History keyboard shortcut integration is not yet complete. See
[History](history-contract.md).

The numeric editor regression target covers profile changes via UnitAwareView,
hidden/show cycles, incomplete spin-box input across focus loss, Enter, Escape,
no duplicate focus-out write, line-edit cancellation, and changes during drags.
