# Hydraulic edit transactions

## Product contract

One user operation in `HydraulicData` creates a **logical, reversible** MODEL
`ModelEditTransaction`. An operation may affect multiple typed entities, for
example deleting a junction together with its connected pipes and dependent
attachments. Its before/after images and UUIDv7 transaction identity are
preserved in the durable journal. The event groups are designed for future
undo/redo, but **a complete undo/redo UI and history engine are not yet
implemented**.

AOWIS-SERVER-MODEL owns the canonical binary codec, schema and validation.
DVMDB owns immutable repository storage and the native hydraulic membership
manifest/chunk implementation; the GUI owns interactive edit capture,
`HydraulicData`, offline event queue, and communication to the private DVMDB.
Do not add JSON snapshots or a duplicate GUI serializer.

## Current implementation

- `HydraulicData` captures supported entity edits as MODEL transaction frames.
  Capture is deliberately not enabled globally while network-wide settings,
  bulk import and all mutation paths lack equivalent support.
- `PersonalEditQueue` durably spools transactions by private repository and
  draft. Queue removal is conditional on exact DVMDB acknowledgement.
- `HydraulicDraftEventAdapter::prepareWithMembership` checks canonical
  before/after state, builds kind-typed model records, validates the staged
  network, derives sorted UUID references, and emits membership intents for
  creation/deletion. A failed event leaves caller data unchanged.
- DVMDB's `/save-events` can atomically materialize those membership intents
  in existing hydraulic networks with the model changes and event journal.
  `PersonalWorkingCopyClient::saveEventsRecovering` / `flushOldestQueuedEvent`
  forward them as the `membership` array and first check, as DVMDB does, that
  every created/removed entity has exactly one matching intent. DVMDB rejects
  a creation or removal without membership (400), so no candidate can hold an
  entity record that its network does not list.
- The existing record-only `prepare()` rejects creation/deletion so that an
  incomplete candidate cannot be presented as a successful saved model.

## HC-01 — Complete 18-collection image capture

`HydraulicEditTransactionCapture` and `HydraulicDraftEventAdapter` now share
`hydraulic_draft_entity_registry.h`: the complete set of 18 canonical
`NetworkHydraulic` entity collections, MODEL type identifiers, native DVMDB
record kinds and membership ordinals. Captured creation/update/deletion events
carry canonical MODEL entity bytes, not GUI-specific JSON or whole-network
snapshots. Capture refuses ambiguous UUIDs found multiple times within or
across collections rather than selecting one arbitrary matching entity.

The `aowis-hydraulic-edit-transaction` regression now runs each of the 18
collections independently through creation, no-op, before/after update, stale
before-image rejection and deletion. It verifies canonical staged replay,
DVMDB record kinds, insertion/removal membership intents and unchanged network
content after reversing an edit sequence. Existing live `HydraulicData`
operation tests remain in place.

**Scope boundary:** HC-01 completes the *generic entity image capture engine*.
It does not mean every UI mutation path produces a transaction (HC-02), nor
that persistent network-level metadata is saved (HC-03 to HC-05). Do not
interpret 18/18 registered entity types as 100% end-to-end editor autosave.

## HC-02 — Auditing actual editor mutation paths (in progress)

The pump simple-control workflow is the first closed gap: `addPumpSimpleControl`,
`setPumpSimpleControl` and `removePumpSimpleControl` now capture the **control's**
UUID instead of incorrectly watching its owning pump. Newly created control IDs
use UUIDv7. Removing a link or node cascades simple controls referencing that
link or trigger node; the GUI watches all dependents before applying the edit,
so one logical MODEL event contains the entire change. A dedicated test checks
create/update/no-op/delete, pump and node cascades, membership replay and the
reconstructed network's canonical bytes.

**HC-02.2 — rule and map-label reference cascades:** Deleting pipes, pumps or
valves removes any rule controls referencing the link in either premises or
THEN/ELSE actions. Deleting junctions, reservoirs or tanks applies the same
rule to the node and every link removed with it. The GUI watches each rule UUID
before deletion, so the dependent rule and owner are one reversible MODEL
transaction with server-side membership intents. Map labels anchored to a
deleted node are **detached**, not deleted: their text and coordinates remain
unchanged, and their before/after images are saved in that transaction.
A shared rule-reference predicate keeps the edit and capture paths consistent.
The HC-02.2 test checks all six deletion entry points, unrelated-rule retention,
anchored-label detachment and exact canonical draft replay.

**Still to audit in HC-02:** actual editing surfaces for rule controls, labels,
patterns, curves and materials, link split/rejoin dependencies, remaining import
and bulk-edit paths, and references from network-root settings. HC-02 is **not
yet complete**; network-root settings require HC-03/04 and are outside this patch.

## Required editor synchronization

1. Checkout a known immutable branch base (or unborn branch) into a personal
   working repository; create/reopen the precise draft and acquire its lease.
2. Hydrate `HydraulicData` from the pinned base plus the draft's candidate
   overlay. Verify network UUID, canonical records, dependencies and generation.
3. Enable capture only for supported edit paths. Immediately persist complete
   logical transactions to the local queue before treating them as recoverable.
4. Replay oldest queued events in order against a **staged** network, never the
   latest editor state. Materialize records and membership intents atomically.
5. Submit using expected generation and lease fencing. Accept success only
   after authoritative acknowledgement/reopen; otherwise retain the queue.
6. On restart/offline recovery, reconcile already-accepted journal frames and
   reconstruct the same staged model without duplicates or lost edits.
7. Expose saved/unsaved/conflicted state in Projects. Validate the complete
   candidate before separate **Commit locally**; Save must never commit.

## Acceptance evidence still required

Prove create/update/cascading delete, multiple queued operations, reconnect,
dropped acknowledgement, process crash, stale lease, wrong draft target, and
reopen of a complete hydraulic network. Test actual GUI wiring to a real
private DVMDB, not only an in-process codec or mocked HTTP endpoint. Preserve
transaction grouping for eventual undo/redo and do not enable unsupported
mutation paths through a misleading generic autosave switch.

Related documents: [Save and recovery](personal-working-copy-save.md),
[Projects workspace](projects-workspace.md), and the
[implementation roadmap](dvmdb-integration-roadmap.md).
