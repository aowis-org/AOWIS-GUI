# History dock and cross-Commit Undo/Redo — implementation roadmap

**Status: HIST-UI read-only dock implemented; HIST-00 architecture specified.**
Undo/Redo and history navigation are **not implemented**.
The detailed [GUI interaction contract](history-contract.md) and
[backend persistence contract](../../AOWIS-SERVER-DVMDB/docs/history-contract.md)
are the authoritative design decisions for subsequent milestones.
The toolbar's **Open revision history in Projects** button now opens a closable
**History** dock on the left of the current workspace without leaving the
hydraulic editor. Its read-only list displays first-parent commit checkpoints
from the loaded Projects revision DAG and, for an active private editor,
accepted/pending MODEL events from the current draft's bounded journal and
local queue. It marks the current working state and warns about missing
individual pre-Commit actions. Its Undo, Redo and jump controls are disabled:
there is **no** persistent edit-history cursor or inverse-event engine.
See the corresponding [DVMDB history roadmap](../../AOWIS-SERVER-DVMDB/docs/history-roadmap.md).

## Intended user experience

A dock on the **left side** of the active workspace, named **History**:

- Opens or focuses from the existing **Open revision history in Projects**
  control. It should be usable while editing the hydraulic network; opening
  it must not force the user to abandon the editor for the Projects tab.
- Displays the selected project/branch, the complete available action
  timeline (paged as needed), and **Undo**, **Redo**, and jump-to-state
  navigation. Describe logical actions, not raw UUID/chunk writes.
- Clearly distinguishes the **current state** from past and redo-eligible
  states. Mark **Commit** checkpoints with message, revision UUIDv7,
  author/time where available. Allow movement **across** those markers.
- Shows pending Save, uncertain/recovering, unavailable, truncated or
  non-reconstructable history explicitly; never offer an enabled action that
  cannot be safely applied.
- Changes context when the user switches branches/networks. Do not mix
  histories from different private draft IDs. A merge revision has two
  parents; show a first-parent narrative with an expandable route to the
  second parent's history rather than fabricating a single total DAG order.

## Meaning of navigating across a Commit

Commit is a **checkpoint**, not a stop sign. Example:

```text
Add reservoir → Add pipe → [Commit A] → Change diameter → [Commit B]
                                                → Move junction (current)
```

Undo may take the current private working network back through Commit B and
Commit A; Redo may traverse forward again. **Immutable revisions A/B and the
committed branch HEAD are never moved or edited by history navigation.** The
requested historical contents are materialized as durable inverse/forward
changes in the **private working copy** using the same fenced Save pipeline.
If the user subsequently Commits, that creates a **new** immutable successor
from the currently committed local HEAD; it does not change the ancestry of
Commit A or B. Creating a branch whose ancestry truly starts at Commit A
remains a separate **Create branch from revision** action.

A new edit after moving backward invalidates the ordinary redo path from that
cursor, but does **not** erase already committed revisions or the historical
audit. The exact display of abandoned redo branches must be decided in the
history architecture (they may remain inspectable without being ordinary
Redo targets). A Commit marker is not an executable MODEL edit; it is a
reference to an immutable snapshot and an action-history checkpoint.

## Current reusable foundation — not yet Undo/Redo

- GUI `HydraulicEditTransactionCapture` and `HydraulicDraftEventAdapter`:
  UUIDv7 transactions containing before/after images for changed MODEL
  entities and root metadata; create, update and delete semantics.
- GUI `PersonalEditQueue` / `PersonalDraftEditStage` /
  `PersonalDraftEditSession`: durable pending event transport, canonical
  replay, fencing, generation CAS and recovery. The **local queue removes
  acknowledged entries**, so it cannot serve as the full history.
- DVMDB stored working-copy journal: accepted MODEL event bytes and
  generation; `rebaseCommittedDraft()` currently **clears this journal
  after Commit**. Thus action records across Commit markers are not currently
  available from this API. Historical revisions still retain their
  immutable snapshots, but may not retain every individual edit action.
- Generic MODEL edit transaction inversion and user-facing undo/redo cursor
  do **not** exist. A specialized geometry helper named `undoPipeSplit()`
  is not a general history system.

## Implementation status and remaining stages

### HIST-00 — Persistent action-history contract (specified)

[GUI interaction semantics](history-contract.md) and
[DVMDB storage/recovery contract](../../AOWIS-SERVER-DVMDB/docs/history-contract.md)
settle action/group identity, Commit markers, cursor and redo paths, safe
cross-Commit reconstruction, original MODEL v1 frame retention, page and
scope identities, and the current metadata/history gaps. This milestone is
**design only**: no inverse engine, permanent archive, cursor or navigation
endpoint exists yet.

### HIST-UI — Read-only History dock (implemented)

The left `HistoryDock` opens and focuses from the toolbar. It displays the
current project/branch, first-parent committed revision checkpoints and the
active private draft's accepted/pending events. It changes context with
project/branch selection and the active editing session; pending events never
masquerade as committed actions. Undo, Redo and jump remain disabled, so the
panel cannot corrupt a working copy by pretending to navigate history.
Widget regression tests cover reopen, markers, missing commits and disabled
controls. DVMDB's existing journal is **not** a permanent cross-Commit action
store, and the historical revisions still belong in the Projects DAG.

### HIST-01 — Canonical forward/inverse operations

Implement MODEL transaction inversion and application covering all 18
collections, network-root metadata, membership, dependent edits and
reordering rules. One user action corresponds to one logical history step;
a drag is not hundreds of steps, and a cascade must not undo partially.
Keep value/reference/storage integrity, but never require a successful
hydraulic simulation before an inverse event can be saved.

### HIST-02 — Durable, indexed history over Commit boundaries

Build a DVMDB action-history store separate from the bounded *active draft*
journal, link accepted events to their logical action groups and pinned
revisions, preserve Commit markers, and support paged reads and safe
reconstruction of intermediate states from immutable checkpoints + events.
Persist history cursor/redo eligibility or make them deterministically
reconstructable. Preserve exact once semantics for lost acknowledgement,
restart, process crash, and multiple session ownership. Limit size without
silently discarding still-navigable history; define compaction/retention.

### HIST-03 — Operational history controller

Connect Undo/Redo/jump requests to `HydraulicData` and the actual private
DVMDB draft. Applying an inverse/forward operation must produce a **new
fenced, durable event**, with idempotent identity and no accidental second
capture. Synchronize viewport, inspectors, selected network and autosave
status. Prevent concurrent navigation while a previous operation has an
unresolved result. Add shortcuts and meaningful action descriptions.

### HIST-04 — Full dock interaction

Populate the list with paged action records, commit markers, current cursor,
redo states, multiple-step navigation and background loading. Provide
accessible errors for missing/truncated history; keep actions consistent
through branch, project and editor switches. A merge's second-parent
history is browseable but not presented as a linear executable sequence.

### HIST-05 — Qualification

Test create/delete, drag, property edits, all 18 types, root metadata,
mixed transactions, library/control cascades, imports and bulk actions;
Undo→Redo cycles, new edit after Undo, navigation over multiple Commits,
creation of a branch from a checkpoint, restart/crash/ENOSPC, stale lease,
missing response, server disconnect, compaction and journal limits.
Include real external HTTP DVMDB and embedded Standalone sessions. Confirm
canonical model equality after each transition and that immutable
revision hashes/ancestry never change.

## Deferred details (not blockers for HIST-01)

- The future UI for browsing abandoned redo branches is optional; ordinary
  Redo follows only the active path, and the audit remains retained.
- Editing project/branch labels and mutable revision display metadata is
  **informational**, not Undoable model history, in the first release.
- The settings surface for explicit history quota/pruning is deferred; the
  backend must first define safe retention and fail-closed limits.
- First-parent merge history is the default. Alternate-parent browsing is
  read-only until a distinct, explicit context switch is designed.

**Delivery order:** verify mixed-event Save acceptance; build HIST-01 MODEL
inversion alongside HDB-01 durable archive/checkpoints, then HDB-02/03 and
HIST-03 controller before enabling navigation. HIST-04 and HIST-05 complete
the dock and real HTTP/Standalone acceptance.
