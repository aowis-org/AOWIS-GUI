# History dock and cross-Commit Undo/Redo — implementation roadmap

**Status: design direction agreed; subsystem not implemented.** This page
specifies the requested future product, not current GUI behavior.
`TopControlBar::signalProjectHistoryRequested()` currently calls
`ProjectsWidget::showRevisionHistory()`, which navigates to the Projects
revision tree/Inspector. There is **no History dock**, universal Undo/Redo
controller, persistent edit-history cursor or jump-to-action implementation.
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

## Implementation stages (all future work)

### HIST-00 — Design persistent semantics first

Specify identities for action groups, origin events, inverse/redo events and
commit markers; current-state cursor, branching/redo invalidation, partial
history availability, branch-switch behavior, journal retention and
recovery. Determine how metadata actions that are not `NetworkHydraulic`
MODEL edits (branch name, Commit message, project metadata) appear: first
release should distinguish their informational markers from model Undo.
Do not pretend current draft events or immutable revisions contain action
history that was never recorded.

### HIST-UI — Add the left dock without faking engine support

Create a closable/dockable **History** panel integrated with the main window.
Connect the existing toolbar action to open/focus it. Initially show the
current project/branch and actual available **committed revision markers**,
plus clearly labeled pending/current-state information. Undo/Redo and
jump controls must remain disabled until backed by authoritative history.
Do not replace the Projects revision graph: it remains the DAG inspector.
This UI stage can be delivered independently while the backend is built.

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

## Decisions deliberately still open

- Exact retention and UI for abandoned redo branches and non-reconstructable
  pre-history commits.
- Whether independent metadata changes (project/branch labels and mutable
  revision annotations) are undoable or merely shown as timeline events.
- How user-selectable retention quotas and history pruning are exposed.
- The precise choice for navigating a merge: the first-parent narrative is
  the default, but switching to a merged-in path requires explicit context.

**Delivery order:** finish the latest mixed-event Save regression, document
HIST-00 semantics, implement the modest HIST-UI dock, then build durable
HIST-01/02/03 behavior before advertising Undo/Redo as functional. The
broader GUI integration roadmap can continue in parallel.
