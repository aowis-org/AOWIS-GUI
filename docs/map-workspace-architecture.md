# Maps workspace architecture

## Modes

The **Maps** main-navigation page exposes five modes, in tab/page order:

| Mode | Domain | Interaction | Tools |
| --- | --- | --- | --- |
| Distribution Monitor | Distribution | Monitor | Available |
| Distribution Editor | Distribution | Editor | Available |
| Drainage Monitor | Drainage | Monitor | Empty placeholder |
| Drainage Editor | Drainage | Editor | Empty placeholder |
| Map Management | Management | Cache selection | Tile and terrain cache tools |

`src/tabs/map_workspace_mode.h` is the Qt-independent source of mode labels,
icons, domains, activities and tool availability. `MapWorkspaceWidget::Mode`
retains a Qt `Q_ENUM` for UI integration; compile-time assertions enforce
matching indices. The default mode is Distribution Monitor. Switching to a
blank drainage mode is valid and must not accidentally display distribution
networks or run distribution editing.

For the current 2D/3D hydraulic editing controls, network workflows and
height behavior, see the [Distribution Editor guide](distribution-editor.md).
This page focuses on the shared map workspace and renderer ownership.

The workspace keeps one persistent native bgfx viewport: Render's
`MapBgfxMapView`, hosted by `MapBgfxSurface` ([map host guide](../src/map/bgfx/README.md)).
Device, view-ID and frame rules shared with the Projects graph are in
[Rendering architecture](rendering-architecture.md).

## Ownership and boundaries

`MapWorkspaceWidget` owns the single shared `MapModel` and hosts
the persistent `MapWidget` and
`MapBgfxSurface` in `MapWorkspaceViewportPage`. The renderer is constructed
by the monitor controller once; the editor receives non-owning pointers to
that same map and surface. The editor keeps its `MapCanvasWidget` as an
input-only, offscreen QWidget. It does not create another navigation widget,
native window or bgfx surface. Its canvas is resized with the live viewport
and receives forwarded pointer/keyboard/wheel events from the surface on
desktop or from the `MapWidget` event filter on WASM.

Distribution Monitor ↔ Editor changes only the visible sidebar in a nested
`QStackedWidget`, plus active navigation handlers, HUD actions, editor overlay
builders, network symbology and labels. The right viewport is never hidden
or reparented during these switches. `MapMonitorContainer` owns the GPU
lifecycle; `MapEditorContainer` supplies editor behavior and keeps its editor
callbacks until shutdown. The workspace explicitly destroys the editor
controller before the monitor's renderer to avoid dangling captured pointers.

Both drainage modes use the same live map viewport and camera, with empty
sidebar tool placeholders. Their basemap-only presentation hides distribution
network geometry, symbology, selected/highlighted entities, labels, globe
network meshes, editor overlays and monitoring legends. The renderer's
`setDistributionNetworkVisible()` switch changes presentation without clearing
or reallocating retained network geometry. The terrain, imagery and general
map HUD remain visible, and the map can still pan, zoom and switch 2D/3D.
Node decluttering uses two cached camera-independent 2D scene presentations:
monitor may separate coincident nodes while editor displays exact coordinates.
The variants share one retained geometry revision and each is prepared once
per revision (editor's variant is lazy). Mode switches reuse their preparation
IDs without recomputing positions. The inactive variant is discarded when
actual network geometry changes to avoid retaining stale large networks.
The current GPU mesh still follows the selected variant's preparation ID and
may upload on mode changes; this cache eliminates CPU derived geometry
preparation, not all presentation-dependent GPU work. 3D decluttering and
symbol-size changes can also require appropriate derived globe stream updates.

Each mode owns its visualization profile. Entering editor activates its
symbology, overlays, input handlers and blank monitoring legend/diagnostic
highlights. The monitor's model-selection and simulation-diagnostic signals
may change while editing, but cannot overwrite the editor's GPU state; the
latest selected entity and diagnostics are restored when returning to monitor.
The monitor reinstates its own HUD action handlers, labels and symbology.
The shared globe restores icon *presentation* on each activation. Both
distribution modes use flat entity sprites rather than 3D object models.
Distribution Editor projects its full-colour editor pixmaps through the
terrain-aware overlay, shared with its 2D visual-state pipeline; the Monitor
uses its own overlay instance and icon-size setting without inheriting Editor
selection or placement state. The Editor positions hydraulic entity icons
using resolved model elevations where appropriate, while demand points and
reservoir markers remain terrain-anchored. Icon placement is culled for
invisible/off-viewport entities, and the drawn icon rectangles support picking
through terrain. This presentation does not require a new retained network
geometry snapshot. The no-3D-model mode policy lives in
`MapWorkspace::useGlobe3dIconModels()`.

The editor's hydraulic tool state is not duplicated for the globe:
`MapCanvasEntities` and the marker, pipe, device-link and placement helpers
remain responsible for the actual editing actions. The projection boundary
(`MapCanvasProjection`) supplies Mercator or globe coordinates and renderer
hits; `MapEditorController` handles shared-map input and selection rectangles.
Both the 2D and globe overlay builders consume `MapEditorVisualState`. For
user-visible controls and elevation rules, see the
[Distribution Editor guide](distribution-editor.md).

`MainWindow` has only one distribution `MapModel`. Camera movement, provider
changes, zoom edits, GPS navigation and fullscreen edge-panning are routed
through one shared `MapWidget`. The obsolete **Sync Map Movement** checkbox,
cross-model signal forwarding and per-mode center snapshots are removed.
`MapModel::setViewMode()` transfers the apparent zoom scale between 2D and
Globe; both views already use the same center coordinates, so no second
synchronization controller is needed. Switching Editor/Monitor changes neither
camera nor native surface.

Map fullscreen uses one MainWindow state. On a map mode change, the
previous controller relinquishes fullscreen/HUD focus before the newly active
controller receives it. `MapWorkspaceViewportPage` hides the entire outer
sidebar stack during fullscreen, rather than its active `MapModeToolPanel`:
hiding a stacked child alone leaves the parent `QStackedWidget` reserving its
full width and produces an empty strip instead of an expanded map. The native
map stays parented and visible; exiting fullscreen restores the sidebar.
Switching to drainage or management retains map-only fullscreen because the same viewport
remains visible; drainage navigation uses the monitor controller without
distribution selection or editing. All five modes share footer zoom and
edge-panning routes.

`MapModeToolPanel` owns scrolling and the mode-selector UI but never owns a
map model, native window or renderer. Mode tools and the monitor viewport are
siblings in the workspace page. The mode controller owns monitoring behavior; this does not create
another GPU renderer or another independent camera.

`MapWorkspaceWidget::activeMap()` and `activeModel()` provide *non-owning*
navigation access to the single shared map in **all five** modes. MainWindow
uses these accessors to route footer zoom edits, including in drainage.

**Shared viewport contract:** the Maps workspace exclusively owns one
`MapModel` (camera and provider), one `MapWidget` (continuous navigation), one
`MapBgfxSurface` (GPU resources, retained network, terrain, basemap and HUD),
and shared repositories are supplied from MainWindow. This hierarchy must
stay attached to its native window / browser canvas while modes change. Mode
switches will not call `bgfx::shutdown()` or `bgfx::init()`.

The following contracts preserve the shared Maps workspace and prevent
reintroducing duplicate renderers:

- **Tools:** each sidebar owns its controls and user settings, not the camera,
  native window, bgfx resources, or a duplicate renderer. Controls send
  requests to their mode controller.
- **Domain visualization:** distribution monitoring supplies simulation
  symbology, legends, and picking; distribution editing supplies edit styling,
  editor overlays, hidden-entity previews and selection. Drainage behavior
  remains empty until its model and renderer integrations are ready.
- **Input:** the shared viewport retains the existing map navigation and
  HUD-first event handling. The active mode gets the next chance to consume
  input. Unconsumed input reaches ordinary navigation; hidden controllers
  must not receive editing actions.
- **Rendering:** camera, imagery and HUD changes must not invalidate retained
  network geometry. Geometry revisions reflect changes to actual networks.
  Presentation changes may require derived display preparation (currently
  node decluttering) but must not force a network snapshot rebuild. Simulation
  values are visual data, not new geometry.
- **Lifecycle:** losing or recreating the native surface can require a bgfx
  device restart. Switching modes never does.

The single persistent map viewport serves five modes. Only the tool panel,
input routing and rendering policy switch. Native-window changes, rather than
mode changes, are the only reason to use bgfx lifecycle restart machinery.
Fullscreen and WASM input retain the same viewport and navigation model.

## Diagnostics and regression baseline

Start the application with:

```bash
AOWIS_MAP_RENDER_DIAGNOSTICS=1 ./build-linux/aowis-server-gui
```

`MapRenderDiagnostics` emits compact JSON snapshots at most every five seconds
while the map renders, **plus an immediate snapshot after each mode change**.
The immediate snapshot is taken after the synchronous QWidget changes; an
asynchronous bgfx restart may appear in the *next* snapshot. These are cumulative process-wide counters for the shared bgfx surface.

| Counter | Meaning |
| --- | --- |
| `workspace_mode_switch` | Number and CPU-time distribution of actual mode changes (does not include identical selections) |
| `bgfx_device_started` | Successful bgfx backend and shader/GPU resource setup |
| `bgfx_device_stopped` | Shut down a previously running bgfx backend |
| `bgfx_2d_scene_prepared` | Successfully regenerated derived 2D network geometry, including declutter changes |
| `zoom_change`, `center_change` | Camera updates; independently of scene preparation |
| `globe_geometry_rebuilt`, `globe_flow_rebuilt` | Existing globe preparation diagnostics |
| `frames` | Submitted frames and their CPU time, **not** GPU execution time |

Because the distribution viewport is shared, ordinary Monitor ↔ Editor
mode switches should increment `workspace_mode_switch` **without incrementing**
`bgfx_device_started` or `bgfx_device_stopped`. The derived
`bgfx_2d_scene_prepared` can increase once for a previously unprepared
node-declutter presentation. Subsequent editor/monitor switches on the same
network must not increase it. Real network geometry revisions prepare the
current presentation and invalidate the other one.
Initial startup and native surface loss can change renderer lifecycle counters;
**none of the five map modes** should change them.

Automated checks:

```bash
./compile_linux_tests.sh --debug
ctest --test-dir build-linux-tests-debug --output-on-failure \
  -R 'aowis-map-(workspace|mode-tool-panel|camera-invalidation|retained-network-scene)'
```

The workspace viewport-page test checks that the monitor controller's
map stack and tools are workspace-owned without reconstruction, that
fullscreen releases the full sidebar width even after repeated mode changes,
and that the controller is explicitly destroyed before the map it references. It uses ordinary Qt widgets rather than initializing
bgfx. The tool-panel test checks that each controls tree is independently parented,
mode tabs remain outside the scroll viewport, scrollbar behavior is preserved, and fullscreen hides only the requested panel. It runs through
the `aowis-tests` aggregate build target using Qt's offscreen platform.
The separate mode-contract test compiles without Qt or a GPU. The diagnostics
test uses Qt Core but does not create a window. The existing camera
invalidation and retained-network-scene tests cover zoom/LOD and geometry
revision independence. A real GPU/window lifecycle check additionally
requires interactive validation on the target system.

Manual shared-viewport acceptance checks:

1. Open Distribution Monitor; pan and zoom continuously, change basemap and
   2D/3D view; observe one consistent camera and no geometry rebuild during
   pure camera movement.
2. Switch to Distribution Editor; exercise entity selection, editing overlays,
   and fullscreen, and switch back. There must be no device restart. The
   second decluttering presentation can prepare once on the first switch;
   subsequent switches must not increment `bgfx_2d_scene_prepared` until the
   actual network geometry changes.
3. Repeat rapid monitor/editor switching, including with a large network.
   Check the HUD, editor tools, native-window focus, configurable F1–F4
   provider shortcuts and the shared-camera navigation behavior.
   Change the selected entity and simulation timeline while the editor is
   active: its overlays and blank monitor legend must persist, while the
   latest monitor selection/diagnostics reappear on switching back.
4. Open both drainage placeholders in 2D and 3D: the satellite/terrain
   basemap and camera must remain usable, while distribution nodes, pipes,
   demand points, simulation heatmaps and editor overlays disappear.
   Return to Distribution Monitor: network styling and selection are restored.
   Neither `bgfx_device_started` nor `bgfx_device_stopped` should increase.
   Fullscreen remains active during drainage mode switches.
5. Repeat on Linux desktop, Windows and WASM before release.
   A unit test without a GPU cannot prove native-window behavior.

All five map modes share **one persistent GPU renderer and camera**, with
monitor/editor presentation settings, two cached distribution 2D scene
variants and basemap-only drainage placeholders. No mode change may shut down
bgfx, destroy the native window, or clear the network snapshot. No CPU-era
zoom buckets or geometry rebuilds tied to fractional zoom are permitted.

### Renderer lifecycle diagnostics acceptance

Capture both stdout and stderr (Qt writes messages to stderr):

```bash
AOWIS_MAP_RENDER_DIAGNOSTICS=1 ./build-linux/aowis-server-gui 2>&1 | tee /tmp/aowis-map-workspace.log
python3 tools/check_map_workspace_diagnostics.py /tmp/aowis-map-workspace.log --min-switches 12
```

Exercise all five modes before exiting. The checker rejects missing or
non-monotonic counters, unexpected bgfx starts/stops, insufficient mode
switch coverage, and an absence of repeated switches after the last 2D
scene preparation. Genuine network geometry revisions can occur while
testing; run several additional mode switches after loading a network.
This script checks *observed* renderer lifecycle, not visual correctness,
GPU driver allocations, or shutdown memory safety. The Qt regression test
checks explicit controller-before-viewport destruction and repeats mixed
fullscreen/mode changes without constructing a GPU device.

For an end-to-end release check, manually validate imagery/terrain in 2D/3D,
input focus and shortcuts, editor drag-selection cleanup, network styling,
blank drainage overlays, fullscreen expansion, high-density networks, and
clean shutdown on Linux, Windows, and WASM. A passing Linux diagnostics
trace is not evidence that those other platform runs were completed.

## Map Management

The rightmost **Map Management** tab shares the persistent bgfx viewport and
map camera with the two distribution and two drainage tabs. It displays the
basemap without distribution-network layers but preserves retained geometry.
Its sidebar owns the map imagery and terrain cache management toolbox, migrated
from Distribution Editor, including 2D/3D rectangle selection, zoom-range
configuration, and cache-deletion confirmations. **Basemap-only rendering
suppresses distribution-network geometry and labels, not editor overlays:**
the green map-tile and brown terrain-tile selection outlines, fill and grid
must remain visible while dragging and while a completed cache selection is
available for deletion. The actual map/terrain cache deletion workflows remain
owned by the editor controller and cache repositories. Cache selections continue to
use the existing editor input controller; management does not enable network
editing operations. Distribution Editor shows its editing controls directly
without a QToolBox. The 3D Editor icon scale ranges to 600% while 2D retains
its former 250% limit.

## 3D Distribution Editor icons

The 3D Editor's icon index, terrain-height cache, draw order and how to
measure them are described in the
[map host guide](../src/map/bgfx/README.md#3d-editor-icon-visibility).

## Known issue: rare `std::bad_alloc` on close

A rare `std::bad_alloc` on application close has not been reproduced with a debugger backtrace. Workspace-before-repository teardown
ordering is explicit, but the exception's root cause is **not confirmed fixed**.
Keep shutdown testing (including after 2D/3D and mode transitions) in native
Linux and WASM release acceptance (Windows reported working, 14 November 2026), and capture a backtrace if it recurs.

## Three independent map views

`AOWIS_BGFX_THREE_VIEW_PROBE=1` adds two read-only 2D map windows with their
own cameras next to the Maps viewport, on native Linux. They are a diagnostic
for the shared device and per-camera rendering that a future three-way merge
view needs, not editors. See
[Rendering architecture](rendering-architecture.md#diagnostic-three-map-mode)
for their behaviour and test steps. Production multi-map editing, independent
tile scheduling per map, globe instances and WASM multi-canvas are separate
future work.
