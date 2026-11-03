# Hydraulic Distribution Editor

The **Distribution Editor** edits a hydraulic distribution network directly on the
AOWIS map. It supports a flat **2D map** and a terrain **3D globe**. Both views
operate on the **same hydraulic network and the same editing tools**; switching
views does not create a copy of the network.

This page describes the current GUI and its source-level behavior. The in-app
**Map Editor Guide** in the right-hand dock contains the short mouse and tool
instructions. This document explains those workflows in more detail.

## Open the editor

1. Open **Maps → Distribution Editor**, or open a writable network from
   **Projects → HEAD → Open network**.
2. Select **2D** or **3D** in the map's view controls.
3. Select a tool in the editor's left-hand controls and work in the shared map
   viewport. You can switch views without opening another editor.
4. Use the **Map Editor Guide** checkbox to show or hide the right-hand guide.

The map camera, imagery, terrain, and GPU viewport are shared with Distribution
Monitor. Editor mode uses its own selection and drawing presentation, so
simulation overlays and monitor-only highlights do not replace editing
feedback. The editor displays objects at their model coordinates rather than
visually spreading coincident nodes for readability.

An editable network is needed to save model changes. A writable **HEAD** opened
from Projects uses the personal working DVMDB draft and its existing autosave
path. **Autosave/Save is not Commit**; the HEAD's Commit action creates a
revision separately. For repository behavior, see [Projects workspace](projects-workspace.md)
and [private working-copy save](personal-working-copy-save.md).

## Mouse and keyboard controls

| Action | Input |
| --- | --- |
| Pan the map | Left-drag empty map area |
| Zoom | Mouse wheel |
| Select an entity | Left-click its icon or network geometry |
| Add/remove an entity from selection | Shift + left-click |
| Rectangle selection | Choose **Select**, then right-drag a map rectangle |
| Place a node or a demand point | Select its tool, then right-click the map |
| Start / finish a pipe | With **Pipe** active, right-click the start node, optional bends, then the end node |
| Place a pump or valve | With its tool active, right-click the first and second connection nodes |
| Open editing actions | Right-click an existing entity or pipe vertex |
| Confirm a move | Right-click at the desired destination |
| Cancel an active action | **Esc**; otherwise Esc returns to Select |
| Delete selected entities | **Delete Selected** / configured Delete shortcut in **2D only** |

The **Select** tool is also the default mode after cancelling placement. The
left-hand tools include Pipe, Junction, Valve, Demand Point, Pump, Tank, and
Reservoir. **Power Source** and **Note** are present but disabled. The editor
supports the regular map navigation controls, fullscreen, icon size, and map
presentation controls without a separate editing camera.

### Selection and visibility in 3D

On the globe, selection uses the *displayed* icon rectangles first, then
renderer-backed network picking for geometry such as pipes and junctions.
Objects can be selected **through terrain**; the terrain surface does not need
to be in front of the object. Overlapping icons use their visible draw order.
Icon hit areas follow the icon-size setting. Rectangle selection uses projected
screen positions, not a requirement for every selected object to intersect a
terrain ray.

The **Delete Selected** control is currently **disabled in 3D**. Switch to 2D
if you need that action. Some entity-specific context actions are available in
3D, but the existence of a context menu should not be taken to mean that all
2D deletion paths are enabled in 3D.

## Create and connect network objects

### Junctions, tanks and reservoirs

Choose the corresponding tool and **right-click** the map. The floating entity
sprite shows the pending placement. On the 3D globe, creation requires a valid
terrain elevation sample: the new node uses **Terrain + Offset** with initial
offset **0 m**. An unavailable or invalid sample does **not** create a node with
an invented height. A valid height of exactly 0 m is allowed.

In 2D, creation continues through the ordinary editor/model path; it is not
subject to the 3D terrain-sample requirement.

### Pipes

Choose **Pipe**, right-click the first node, right-click any intermediate bend
locations, and right-click the destination node to finish. The same shared
placement state supplies the floating connection preview, snapped target, and
vertex markers in both views.

Pipe bends are stored as geographic coordinates. In the 3D view, intermediate
bends follow the **drawn terrain surface** rather than introducing independent
hydraulic elevation attributes. Endpoints continue to use their connected
nodes' actual elevations. A bend on terrain therefore does not force an
existing junction or tank elevation to change.

To edit an existing pipe, right-click the pipe segment to insert a vertex, or
right-click a vertex for the available **Move**, **Delete**, or **Convert to
Junction** actions. Vertex movement uses the shared preview/confirm workflow.
The selected pipe and its bends are highlighted in the editor.

### Pumps and valves

Choose **Pump** or **Valve** and right-click the two nodes to connect. Floating
icons and rubber-band connection previews use the same editor artwork as the
2D view. Device-link positioning is derived from the connected network
geometry; adding the link does not rewrite its endpoint elevations.

### Demand points and attachments

Choose **Demand Point** and right-click to place one. Demand points are shown
on terrain in the 3D view, including when the hydraulic network runs above or
below the ground.

Right-click a demand point and choose **Attach to network**, then right-click a
junction or a position on a pipe. The attachment line, handle, target feedback,
and move/reattach preview use the existing shared editor visual state. Right-click
an attachment handle for its available **Move**, **Reattach**, **Detach**, or
pipe-attachment **Convert to Junction** operations.

## Move existing objects

Right-click a node, select **Move**, position its floating preview, then
right-click to commit. Press **Esc** to cancel. Shift-select multiple objects
and use **Move selected entities** from a selected node's context menu to move
a group. Connected pipe previews follow the movement.

In **3D**, moving a node follows its *existing elevation input mode*:

| Elevation mode | Move behavior |
| --- | --- |
| **Terrain + Offset** | Update the sampled terrain elevation at the destination; preserve the offset exactly |
| **Absolute elevation** | Preserve the stored absolute elevation; change the geographic position |
| **Demand point** | Move geographic position; display remains terrain-draped |

The node's input mode is not silently changed by moving it. Junction elevations,
tank bottom elevations, and reservoir hydraulic heads are **different model
quantities**: in particular, the reservoir's head is not assumed to be the
physical height of its icon. Reservoir icons remain terrain-anchored; junctions
and tanks with resolved model elevations are displayed at the appropriate
hydraulic height.

A 3D group move checks every affected terrain-relative node. If any required
sample is unavailable or non-finite, the shared batch is rejected; the editor
does not partially relocate the network. Node coordinates, terrain components,
and connected pipe lengths are synchronized through the hydraulic edit
transaction. Absolute node elevation and terrain-relative offsets are not
rewritten as a side effect.

**Invalid cursor locations:** When the cursor is outside the globe or cannot
resolve to a valid world coordinate, a pending placement or move must not
commit at a stale location. Move back onto the globe to resume, or press Esc.

## What 3D height means

AOWIS maintains **one hydraulic model**, not separate model and visual heights.
The 3D view adds a projection of that model over terrain:

- Junctions use their resolved hydraulic elevation.
- Tanks use their resolved bottom elevation.
- Reservoir icons use a terrain anchor; reservoir hydraulic head stays a
  hydraulic boundary condition rather than a physical structure height.
- Demand points and intermediate pipe bends follow terrain.
- Pumps and valves follow their rendered link geometry.
- Vertical exaggeration and the network ground-offset display setting affect
  projection; they do **not** change stored hydraulic elevations.

The renderer can use a provisional drawn-terrain surface or fallback when terrain
has not loaded, but **model changes that require a terrain height must have a
valid sample**. Missing terrain is not silently converted to 0 m. There is no
additional project-wide vertical-datum setting or separate visual-elevation
system in this editor.

## Editing state, undo and persistence

**One shared editor:** `MapCanvasEntities` coordinates the existing marker,
pipe, device-link, selection, and placement helpers. Both map views publish the
same `MapEditorVisualState` (floating sprites, highlighted entities, connection
targets, bend dots, attachment handles, and movement previews). The bgfx editor
overlay renders that state in either projection.

The **flat adapter** delegates screen/world conversions to `MapModel`'s 2D
Mercator methods. The **globe adapter** uses the live renderer's terrain
projection, elevated projection, and hit-test results. A globe projection may
fail when a ray misses the world; a failed projection is not a valid edit.

Model changes use the existing `HydraulicData` operations and edit transactions.
A completed action should participate in the same undo/redo and private-draft
capture mechanisms as the corresponding 2D action. The source contains focused
unit/regression coverage for those components, but a successful component test
is **not** proof of an end-to-end interactive save/reopen session, or of the
Linux, Windows, and WebAssembly user experience.

## Source and verification reference

| Component | Responsibility |
| --- | --- |
| `src/map/editor/map_canvas_entities.*` | Shared entity selection, creation, move, and hydraulic synchronization |
| `src/map/editor/map_canvas_placement.*` | Floating markers and pending placement/move state |
| `src/map/editor/map_canvas_markers.*` | Node and demand-point markers |
| `src/map/editor/map_canvas_pipes.*` | Pipe construction and vertex editing |
| `src/map/editor/map_canvas_devicelinks.*` | Pump and valve placement |
| `src/map/editor/map_canvas_projection.h` | 2D and globe coordinate/hit adapters |
| `src/map/editor/map_editor_visual_state.h` | Shared overlay/preview state |
| `src/map/editor/map_editor_controller.*` | Routes map input, view-specific rectangle selection and cache selection |
| `src/map/bgfx/map_bgfx_editor_overlay.*` | GPU HUD icons, previews, selections and sprite hit rectangles |
| `src/tabs/tab_map_editor_container.cpp` | Editor controls, surface integration and overlay callbacks |
| `src/map/editor/map_editor_guide_dock.cpp` | In-app instructions in the right dock |

For a focused source regression run in an already configured Linux test build:

```bash
cmake --build build-linux-tests --target \
  aowis-server-gui \
  aowis-map-canvas-projection-tests \
  aowis-map-canvas-preview-tests \
  aowis-bgfx-editor-overlay-tests \
  aowis-map-globe-pipe-bend-drape-tests \
  aowis-hydraulic-edit-transaction-tests -j 8

ctest --test-dir build-linux-tests --output-on-failure \
  -R '^(aowis-map-canvas-projection|aowis-map-canvas-preview|aowis-bgfx-editor-overlay|aowis-map-globe-pipe-bend-drape|aowis-hydraulic-edit-transaction)$'
```

For a real workflow check, edit a writable network in **both** map views,
select buried entities, create and move terrain-relative and absolute nodes,
create a bent pipe, attach a demand point, cancel a placement, undo/redo,
reopen the network, and inspect its saved coordinates and elevations. Also
exercise a missing-terrain location and switching views during an operation.
The C++ tests alone cannot replace this interactive check.
