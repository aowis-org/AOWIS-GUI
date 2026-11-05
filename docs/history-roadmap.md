# History — remaining acceptance and usability work

The History implementation is present: original MODEL action archives,
server-owned cursor, cross-Commit Undo/Redo, history forks, sparse HTTP
navigation, hash/cursor reconciliation, and **durable GUI-restart navigation
intents**. This document contains only work **remaining to qualify or improve**
that product. See [behavior](history.md),
[interaction rules](history-contract.md) and
[recovery design](history-recovery.md).

## Real editor and independent DVMDB validation

Exercise a real distribution network with property, root, library, controls,
map-label, structural and terrain-driven edits. Save, Undo/Redo, jump across
at least two local Commits, then edit from an archived boundary and Commit
again. Verify exact canonical network bytes, valid UI selection/renderer
state, the abandoned forward Redo path, and unchanged immutable revisions.
Test long branch ancestry pages and archived action pages in the live dock.

## Crash, power-loss and storage failure testing

Run genuine **process termination** of the GUI before sending navigation,
while the POST is in flight, after server publication but before response,
and while the local intent is being written or cleared. Include independent
DVMDB restart, powercut/ENOSPC and writer-lease rotation. On recovery, verify
original operation UUID, generation, cursor, `NetworkHydraulic` SHA-256 and
absence of a duplicate Undo. Assert no full-network GET when a valid pending
intent allows sparse recovery. Corrupt, wrong-scope and expired/stale
reconciliation must block rather than overwrite.

These are acceptance tests for the **implemented** restart-recovery mechanism,
not a request to implement another Undo stack or another persistent DVMDB
history format.

## Platform qualification

Test native Linux, embedded Standalone and browser/WebAssembly separately.
For WebAssembly, prove that the client intent/queue directory is backed by
persistent browser storage and survives an actual page/browser restart.
Document storage eviction and unavailable remote REST-server behavior.
An unreachable personal REST server is not an offline local database.

## Remaining usability and bounded-scope features

- Context-sensitive application History shortcuts that do not intercept
  in-progress `QLineEdit`/Qt text Undo.
- Improve reasons shown for ineligible cursor, checkpoint-only revisions,
  stale writer leases and unsupported membership-order edits.
- Qualify unborn initial-network seed actions, including the first tank,
  imports and unsupported deletion ordering; extend exact ordering provenance
  only if tests show it is required.
- Keep native DVMDB multi-network **storage** separate from current
  single-unambiguous-network History navigation. Broader navigation is a
  future product decision, not part of restart recovery.

Full-project checkout, Fetch, Publish, conflict handling and merge belong to
the separate [GUI–DVMDB roadmap](dvmdb-integration-roadmap.md).
