# Hydraulic editing and canonical MODEL transactions

This is the **current implementation**, not a list of older capture stages.
See [autosave and recovery](personal-working-copy-save.md) for transport and
[the remaining roadmap](dvmdb-integration-roadmap.md) for qualification work.
The private History dock supports adjacent DVMDB-verified single-network
Undo/Redo **and direct jumps** to eligible saved actions, archived actions
and Commit checkpoints. MODEL transactions carry reversible before/after
images; DVMDB persists original actions permanently at Commit. Unborn-base
navigation, full timeline paging and GUI end-to-end qualification are still
outstanding. See [History](history-contract.md) and the
[remaining History roadmap](history-roadmap.md).

## Model scope

`NetworkHydraulic` has 18 top-level typed collections, plus persistent root
metadata (network identity, titles, simulation options and timing,
energy/quality/report settings, MSX configuration and map backdrop). The
single registry `src/projects/hydraulic_draft_entity_registry.h` maps
collection ordinal to MODEL type and DVMDB native record kind:

| Ordinal | Collection | MODEL entity | Kind |
|---:|---|---|---:|
| 0 | `pipe_materials` | `HydraulicPipeMaterial` | 8 |
| 1 | `patterns_time` | `HydraulicPatternTime` | 9 |
| 2 | `curves_tank_volume` | `HydraulicCurveTankVolume` | 10 |
| 3 | `curves_pump_head` | `HydraulicCurvePumpHead` | 11 |
| 4 | `curves_pump_efficiency` | `HydraulicCurvePumpEfficiency` | 12 |
| 5 | `curves_valve_headloss` | `HydraulicCurveValveHeadloss` | 13 |
| 6 | `curves_valve_characteristic` | `HydraulicCurveValveCharacteristic` | 14 |
| 7 | `curves_generic` | `HydraulicCurveGeneric` | 15 |
| 8 | `nodes_reservoirs` | `HydraulicNodeReservoir` | 2 |
| 9 | `nodes_junctions` | `HydraulicNodeJunction` | 1 |
| 10 | `nodes_tanks` | `HydraulicNodeTank` | 3 |
| 11 | `demand_points` | `HydraulicDemandPoint` | 7 |
| 12 | `links_pipes` | `HydraulicLinkPipe` | 4 |
| 13 | `links_pumps` | `HydraulicLinkPump` | 5 |
| 14 | `links_valves` | `HydraulicLinkValve` | 6 |
| 15 | `controls_simple` | `HydraulicControlSimple` | 16 |
| 16 | `controls_rules` | `HydraulicControlRule` | 17 |
| 17 | `map_labels` | `HydraulicMapLabel` | 18 |

MODEL owns canonical `ModelCodec<T>`, `ModelEditTransaction`, and reversible
entity/root images. The GUI's `HydraulicEditTransactionCapture` watches model
UUIDs around a logical edit and records create/update/delete images; no-op
operations do not produce an edit. The GUI adapter verifies images and
collection ordering against the previous staged candidate and emits typed
record changes and membership intents. DVMDB alone materializes native
network manifests and membership chunks; the GUI never authors those bytes.

A new AOWIS logical object has UUIDv7; valid imported MODEL entity IDs can
have other UUID versions, such as deterministic EPANET-import UUIDv5.
Network membership preserves canonical collection order. The same event
can contain network-root metadata and typed entity/membership changes;
this **mixed-event** path exists in source and must be qualified with real
HTTP/Standalone acceptance tests before it is considered finished.

## Current editor mutation paths

`HydraulicData` owns changes to the in-memory network. Node/pipe placement,
movement, split/rejoin and dependency cascades, simple/rule controls,
anchored map labels, and eight library collections have logical capture
paths. **Hydraulic Libraries** and **Controls & Map Labels** pages edit their
corresponding model collections with guarded references and canonical
transactions. Root settings use `setNetworkRootMetadata()` and emit a root
MODEL event; mixed root/entity events are accepted by the current adapter.

The 2D map and 3D terrain editor share the same hydraulic model and editing
state. An invalid or unavailable terrain sample blocks edits that require
terrain height; solver infeasibility, no supply source or disconnected
hydraulics are not automatically Save/Commit errors. Current MODEL/native
reference rules still reject absent/wrong-type model UUID references and
invalid canonical values; permitting dangling references is **not** an
agreed integration requirement.

## Session and transport

1. Load a selected saved personal draft pinned to repository/branch,
   generation and base; reconcile any pending local queue entries.
2. Acquire a fenced writer lease and install the canonical model into the
   editor, then enable logical edit capture. Loading itself is not an edit.
3. Append the canonical event to `PersonalEditQueue` **before** an HTTP Save.
4. `HydraulicDraftEventAdapter` creates changes and membership intents from
   the previous state; DVMDB validates and applies them atomically with
   expected generation, session and fencing token.
5. Acknowledge only the event verified as persisted. For a lost response,
   inspect authoritative candidate/journal before removing the queue entry.
6. If in-memory and staged canonical networks diverge, block the session
   rather than display a false Autosaved state.

One Save is bounded (currently up to 128 changed entities, with finite
payload/journal limits). Bulk edits exceeding the limit fail; batching one
user operation while preserving atomicity remains planned. Full persistence
acceptance also requires real process restarts, not merely queue tests.

## What the current tests actually establish

`aowis-hydraulic-edit-transaction` tests canonical capture/replay for the
18 collections, root metadata, durable local queue operations and mixed
transaction staging. Related widget tests cover libraries and controls.
DVMDB native and HTTP tests exercise server-owned manifests, leases,
generations, commit/reopen and mixed root/member events. The newest changes
in the reviewed source are **not asserted to pass** unless their test
execution has been reported. For the missing end-to-end tests, including operational Undo/Redo
acceptance, see the two roadmaps linked above.
