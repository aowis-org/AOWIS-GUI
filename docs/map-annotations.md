# Map annotations — Distribution

Map annotations are **revisioned project content**, separate from hydraulic
solver data and from automatically generated network labels (IDs, pressures,
flows, etc.). They are not personal GUI preferences or unversioned notes.

## Use in the GUI

1. Open a verified, writable personal working copy of a Distribution network.
2. In **Maps → Distribution Editor**, select **Annotation**, then right-click
   an empty map position in 2D or 3D. An empty, GPU-drawn annotation marker
   appears on right-button press; a short click opens the **Annotation Inspector**
   with the text field focused. Right-button dragging cancels the placement. Type normally: the first nonblank text is
   automatically created as a canonical MODEL edit after a short debounce.
   All subsequent edits autosave; there is **no Create or Save button**.
   Each right-click leaves its own visible placeholder on the map. You can
   place several blank annotations, select any of them later by clicking its
   callout (or choosing it in the Inspector), reposition them, and enter text
   in any order. Switching Inspector selection **does not discard** other drafts.
   A still-blank outline is a temporary GUI draft, not a DVMDB record: MODEL
   rejects blank persisted text. Drafts remain visible until discarded or the
   network/revision is replaced; they do not survive restarting the GUI.
   Without a verified writable working copy, the draft text field stays
   editable for local preparation, but the Inspector explicitly labels it
   **Unsaved draft** and nothing is written to MODEL or DVMDB. Once a writable
   working copy is available, selecting the draft allows it to autosave.
3. Select an existing annotation on the map (or use the Inspector's annotation
   selector). Edit text, WGS84 coordinates, text size, visibility and background.
   Move it by dragging in the map, or attach it to a junction, reservoir, tank
   or demand point in the Inspector.
4. An attached annotation follows its target's geographic position using the
   stored east/north offset. Dragging updates the offset; deleting the target
   detaches the annotation at its last resolved position **within the same
   reversible edit event**.
5. **Distribution Monitor** displays annotations in 2D/3D without allowing
   edits, including local unfinished drafts from the active Editor session.
   Opened immutable revisions and unverified History positions are also
   read-only. Committing a working-copy HEAD creates the immutable revision.
6. **Annotation display size** is controlled by the **Annotations [%]**
   slider in the **Distribution Monitor left panel only**. The extreme left
   position means **Off**; moving right shows Monitor annotations from 25%
   to 250% of their normal screen size. Distribution Editor always renders
   annotations at a fixed, selectable 100%, independently of that slider.
   This is a display preference, not revisioned MODEL data. Right-click
   the Monitor slider to reset it to 100%.
7. In the **Annotation Inspector**, **Visible on monitor map** controls only
   whether an annotation appears in Distribution Monitor. **Distribution Editor
   always displays it**, even with this flag turned off, so it remains
   selectable, movable and recoverable. The Monitor size slider's **Off**
   position never suppresses Editor visibility or selection.
   **Show background** toggles the text callout's filled background;
   a selected annotation can still have an outline without a fill. These
   style changes are revisioned when editing a writable working copy. For
   unfinished local drafts, the preview follows the checkboxes immediately.

8. The regular **Select** tool also handles annotations: left-click selects,
   left-drag moves, and right-drag rectangle-selects an annotation only when
   its **central map-position dot** is inside the rectangle (in 2D and 3D),
   matching hydraulic entities. Merely overlapping its text does not select it.
   Right-click directly on an annotation (with Select or Annotation active) opens
   the same **Move / Delete** context menu as other map entities. Move arms a
   floating preview and the next right-click commits the new position; Escape
   cancels the move. For a selected annotation group the menu includes group
   actions. Context-menu deletion requires confirmation.
   Direct click/context-menu hit-testing uses the visible bgfx callout. Rectangle
   selection instead uses the dot's projected screen position recorded with the
   same GPU overlay (including pan offsets). The standard
   **Delete Selected** action removes the selected annotations, including
   unfinished local placeholders. A group move or deletion of persistent
   annotations is recorded as **one reversible MODEL transaction**, not one
   transaction per annotation. Selecting a group draws a visible blue glow
   around every selected callout, in both map projections.

The Inspector debounces edits while typing: each pause updates the live
annotation (the map follows), but nothing is recorded yet. One editing session
— from the first change until keyboard focus leaves the Inspector, the
selection changes, writable editing closes or the network is replaced — is
recorded as **one** MODEL event, so one History item (a new annotation and the
text typed after it are one creation). `HydraulicData::beginMapAnnotationEdit`
/ `finishMapAnnotationEdit` hold the session; any other edit records it first.
The pending edit is flushed **before** the original writer/network is
relinquished. A write is still
subject to canonical MODEL validation and durable local queue staging: the
on-screen change is not itself a server acknowledgement.

**Drainage:** Its map modes are placeholders. Do not create a separate
pre-network Drainage annotation store just to support a placeholder page.
The position resolver and MODEL schema are domain-neutral, so Drainage can
integrate once its engineering network and storage lifecycle exist.

## Data and ownership

- MODEL: `MapAnnotation` / `MapAnnotationLayer` and reversible `map.annotation`
  entity edits. Anchor type keys are canonical, never translated captions.
- GUI: `HydraulicData` holds the loaded Distribution network and its annotation
  members; `map_annotation_distribution_targets.h` resolves typed point
  anchors through domain-independent `MapAnnotationPosition` geodesy.
- DVMDB: a Distribution annotation layer belongs to its network UUID; it has
  separate native layer/member records. Hydraulic collections remain **17**.
  Draft reads must verify layer identity and generation against the network.
  The layer is read in pages of 39 membership chunks (`fromChunk`,
  `chunkLimit`); every page must have the same generation and chunk count,
  and no annotation may appear in two pages.
- Persistence: one local durable MODEL event is staged before fenced
  `/working-copies/{draftId}/save-events`; when a node is removed, its hydraulic
  deletion and annotation detaches are one mixed event with one layer ID.
  DVMDB materializes its own membership records. Undo/Redo, History and
  committed revisions operate on the combined candidate, not two independent
  histories.
- Rendering: a derived, resolved copy is passed to the bgfx overlay;
  rendering must never overwrite canonical coordinates or offsets. In both
  Monitor and Editor, switching to Drainage or Map Management must never leak
  Distribution annotations into those modes.

## Regression tests and qualification

`./compile_linux_tests.sh` compiles and runs the GUI's registered suite. Its
annotation checks include canonical local CRUD/mixed-event replay, queue
restart/recovery, anchor geodesy and deletion handling, 2D/3D overlay drawing,
and Inspector edit handoff (the `aowis-entity-inspector-live-language` test
also covers the actual annotation QWidget). These **do not by themselves
prove** live DVMDB crash consistency or complete cross-repository behavior.

Before declaring the feature production-qualified, exercise this acceptance
matrix against a **separate real DVMDB process** and against Standalone's
embedded DVMDB, not only an HTTP test stub:

| Scenario | Expected result |
| --- | --- |
| Create → Save → close GUI → reopen draft | Text, position, UUID and style unchanged; layer owner verified |
| Update/drag/attach → Save → restart GUI/server | Canonical anchor and offsets reconstruct the same visible position |
| Delete target with several attached annotations | One durable mixed event; all annotations detached at last positions |
| Undo/Redo across detach, then Commit → reopen immutable revision | Correct hydraulic **and** annotation state at each boundary |
| Branch from archived revision → edit annotation → Commit | Source revision unchanged; new branch owns the edit |
| Concurrent writer, stale generation or expired fencing token | Request rejected; GUI cannot report a false Autosaved state |
| Lost Save response or process termination during staged event | Queue recovery verifies journal/candidate before acknowledgment; no duplicate action |
| Invalid anchor, wrong network, corrupt member or malformed API response | Fail closed; no silent cross-layer write or dropped pending event |
| Switch Distribution Editor/Monitor and 2D/3D | Same annotations visible, editing only in writable Editor |
| Switch to Drainage/Management | No Distribution annotations or editable annotation tools |
| Switch Inspector selection/close writer before debounce expires | Pending edit recorded against original UUID before state is replaced |

Also run the relevant native DVMDB annotation/history/restart test suites in
its own repository, and verify live WebAssembly and Windows Standalone paths
as those deployments are exercised. **No live server, OS restart, or physical
power-cut test result is asserted here.** Record those results before closing
cross-repository durability qualification.

Related docs: [Distribution Editor](distribution-editor.md),
[hydraulic edit transactions](hydraulic-edit-transactions.md),
[private working-copy saves](personal-working-copy-save.md),
[History](history-contract.md), and DVMDB's
`docs/map-annotation-persistence-design.md`.
