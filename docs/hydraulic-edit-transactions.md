# Hydraulic editing and canonical transactions

**Checkpoint: 2026-11-01.** This document describes the implemented GUI
capture and editing paths, their limits, and the remaining HC work. It does
not treat unit coverage as complete user-facing autosave coverage.

## 1. What is persisted

`NetworkHydraulic` contains **18 persisted entity collections** plus
persistent network-root fields (names, options, settings and map backdrop).
DVMDB can serialize/reconstruct a complete network, but the current GUI
**draft-edit event path handles entity changes only**. Network-root metadata
changes are not yet submitted as atomic logical edits (HC-03 through HC-05).

MODEL owns `ModelCodec<T>`, the canonical `ModelEditTransaction` format,
and UUID identity. GUI captures a before/after image of each changed entity
and emits one MODEL edit transaction per logical operation. DVMDB's
server-owned root and membership records must **not** be synthesized by GUI.

## 2. The 18 collections

The single GUI registry
`src/projects/hydraulic_draft_entity_registry.h` is used by both
`HydraulicEditTransactionCapture` and
`HydraulicDraftEventAdapter::prepareWithMembership()`:

| Collection | Entity type | Storage kind |
|---|---|---|
| `pipe_materials` | `HydraulicPipeMaterial` | 8 |
| `patterns_time` | `HydraulicPatternTime` | 9 |
| `curves_tank_volume` | `HydraulicCurveTankVolume` | 10 |
| `curves_pump_head` | `HydraulicCurvePumpHead` | 11 |
| `curves_pump_efficiency` | `HydraulicCurvePumpEfficiency` | 12 |
| `curves_valve_headloss` | `HydraulicCurveValveHeadloss` | 13 |
| `curves_valve_characteristic` | `HydraulicCurveValveCharacteristic` | 14 |
| `curves_generic` | `HydraulicCurveGeneric` | 15 |
| `nodes_reservoirs` | `HydraulicNodeReservoir` | 2 |
| `nodes_junctions` | `HydraulicNodeJunction` | 1 |
| `nodes_tanks` | `HydraulicNodeTank` | 3 |
| `demand_points` | `HydraulicDemandPoint` | 7 |
| `links_pipes` | `HydraulicLinkPipe` | 4 |
| `links_pumps` | `HydraulicLinkPump` | 5 |
| `links_valves` | `HydraulicLinkValve` | 6 |
| `controls_simple` | `HydraulicControlSimple` | 16 |
| `controls_rules` | `HydraulicControlRule` | 17 |
| `map_labels` | `HydraulicMapLabel` | 18 |

The same registry assigns the draft membership ordinal. The capture engine
finds entities by UUID across every collection and rejects ambiguous or
duplicate UUIDs; it distinguishes absent/create, before/after update, and
delete. No-op changes generate no event. Empty/invalid codec images fail
closed instead of being silently counted as successful saves.

The HC-01 focused `aowis-hydraulic-edit-transaction` test was reported
passing. It exercises generic canonical images, replay and membership
behavior for all 18 types. That is **generic capability**, not proof that
every user-interface method can mutate all 18 types or that every mutation
will trigger capture.

## 3. Actual GUI editing paths

`HydraulicData` is the owner of persistent in-memory hydraulic mutations.
Its logical edit scopes watch relevant UUIDs, perform the mutation and
publish the encoded before/after event. `MainWindow` accepts events only
when a fenced personal draft session is active, appends them to the durable
queue, and compares the visible network to the staged candidate.

Implemented additions to the original node/pipe editing paths include:

- Pump simple controls: create/update/delete watch **control UUIDs**, not
  only owning pump UUIDs; dependent control deletions are grouped with
  deleted nodes/links.
- Rule controls: referenced-node/link cascades on removal, including
  THEN/ELSE and premise references; unrelated rules are retained.
- Map labels: anchors to removed nodes are detached (label contents and
  coordinates retained), captured as updates.
- Pipe rejoin: deleting the now-redundant pipe also captures its simple and
  rule-control dependencies and reattached demand points.
- Library reference setters: validate UUIDs and the correct pattern/curve/
  material collection before mutation.
- Eight library collections: create/update/delete APIs, UUIDv7 creation,
  captured updates and guarded removal of referenced entries. A dedicated
  **Hydraulic Libraries** navigation page uses those APIs.
- Rule controls and map labels: dedicated transaction-safe CRUD APIs and a
  **Controls & Map Labels** page; rule premises and THEN/ELSE actions have
  structured editors, map labels have coordinates and optional anchors.

`aowis-hydraulic-libraries-widget` and
`aowis-hydraulic-controls-widget` are focused UI tests. The latter was
reported passing after its missing test-link source was corrected. The
former previously failed its confirmation test; a test fix was delivered,
but its later passing result was not supplied in this conversation.

## 4. Session, queue and Save

1. Select the **personal DVMDB** profile (♛) in Projects. Open a specific
   project/branch/draft, pinned to its immutable base or unborn root.
2. Verify the canonical network and draft generation. Reconstruct the
   visible model by replaying any persisted local pending events in order.
3. Install the verified candidate in `HydraulicData`; this **load** is not
   an edit. Only then enable logical edit capture for supported operations.
4. Capture complete before/after entity images. Append the encoded MODEL
   event durably to the local queue before issuing its Save request.
5. Prepare typed entity record edits and server-owned membership intents
   **against the previous authoritative/staged state**, never the latest
   edited GUI model.
6. Submit the oldest event with expected generation and lease fencing;
   accept success only after authoritative verification. Preserve the queue
   on uncertain results, stale state or errors.
7. On restart, reconcile the queue with the DVMDB journal and candidate
   without losing or duplicating an accepted event. A Save **never** creates
   a committed revision.

`PersonalDraftEditSession`, `PersonalDraftEditStage`, `PersonalEditQueue`,
`HydraulicDraftEventAdapter`, and `MainWindow` implement this path. A
canonical-model divergence guard blocks the editing session if the GUI
model differs from the staged MODEL transactions. This guard is not a
substitute for capturing an unimplemented edit.

## 5. Known gaps; do not claim 100% GUI autosave

- **HC-02.9:** complete the remaining mutation-path audit, especially
  EPANET/EPANET-JS import and whole-network replacement. Its proposed
  blanket import block was **rejected and not applied**. The behavior must
  be decided before implementation; see
  [network loading/import decisions](network-open-import-replacement.md).
- **HC-03:** define canonical MODEL transactions for persistent
  network-root metadata; not one of the 18 entity collections.
- **HC-04:** let DVMDB apply those logical metadata changes to its
  server-owned root atomically, under expected generation and fencing.
- **HC-05:** wire GUI setters (for example, hydraulic headloss formula)
  into the root-metadata edit path.
- **HC-06:** implement the agreed bulk import/replacement/merge semantics.
- **HC-07:** prove complete canonical equality across GUI and DVMDB
  restarts, errors, and both external HTTP and embedded Standalone.

Only after those gates, plus every supported GUI mutation path having a
recorded durable result, may the product claim 100% `NetworkHydraulic`
autosave coverage. The basic remote HTTP GUI edit/restart scenario was
successfully exercised; full coverage and Standalone acceptance were not.

See [Save/recovery](personal-working-copy-save.md),
[Projects](projects-workspace.md) and the
[integration roadmap](dvmdb-integration-roadmap.md).
