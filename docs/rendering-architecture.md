# Rendering architecture

This is the **current renderer contract** for AOWIS-SERVER-GUI (5 November
2026). It covers the production Maps workspace, the native Linux Projects
revision graph, and opt-in multi-window diagnostics. It describes the code in
this repository rather than a sequence of implementation patches.

## What renders where

| Surface | Renderer | State ownership | Status |
| --- | --- | --- | --- |
| Maps: Distribution Monitor / Editor, Drainage placeholders, Map Management | One persistent `MapBgfxSurface`, bgfx 2D/globe | One `MapModel`, `MapWidget`, shared native map target | Normal desktop and WASM UI |
| Projects revision/HEAD graph | bgfx (`ProjectGraphBgfxViewport`) on native Linux X11/Wayland; QPainter fallback | `ProjectRevisionGraph` owns layout, navigation, picking, selection, Inspector routing | bgfx enabled by default on supported Linux Qt platforms |
| Two additional 2D map windows | bgfx, opt-in `AOWIS_BGFX_THREE_VIEW_PROBE=1` | Two separate `MapBgfxThreeViewCamera` instances | **Developer diagnostic only**, not extra production editors |

The normal Maps page still has **one shared map viewport across all five
modes**. Opening the three-view diagnostic does not create new Maps tabs or
independent hydraulic editing sessions. The Projects graph is a different
surface sharing bgfx infrastructure; it is not another map.

The map itself has **no QPainter renderer fallback**. On an unsupported or
failed bgfx backend it reports a renderer status. QPainter fallback applies to
the **Projects graph**, not the Maps renderer.

## Process-wide bgfx device and frame contracts

All bgfx calls are coordinated on the application's common GUI/render thread.
Do not initialize bgfx independently for a new QWidget, QWindow or QtQuick
view. The relevant components are:

| Component | Responsibility | Source |
| --- | --- | --- |
| `BgfxDeviceManager` | Sole `bgfx::init()` / `bgfx::shutdown()` entry point; visibility-aware primary-device lease; retirement/reinitialization when the owning native surface is lost or relinquished | `src/map/bgfx/bgfx_device_manager.{h,cpp}` |
| `BgfxFrameCoordinator` | Device-generation-scoped, non-overlapping bgfx view-ID reservations; batch all active clients before **one** `bgfx::frame()` | `src/map/bgfx/bgfx_frame_coordinator.{h,cpp}` |
| `BgfxNativeTargetRegistry` | Bind the initial/default swapchain and own additional native swapchain framebuffers; resize and release each secondary independently | `src/map/bgfx/bgfx_native_target_registry.{h,cpp}` |
| `MapBgfxSurface` | Production map GPU content, imagery atlases, terrain, HUD, event routing and diagnostic map windows | `src/map/bgfx/map_bgfx_surface.{h,cpp}` |
| `ProjectGraphBgfxViewport` | Projects' embedded native window, GPU HUD draws, text atlas, spatial index and view/target lifetime | `src/tabs/project_graph_bgfx_viewport.{h,cpp}` |

**A bgfx view ID is process-global, not a renderer-local slot.** The Maps
surface reserves five contiguous views (main map and four globe heatmap-bake
passes). The Projects graph and each diagnostic map window reserve their own
view ID. For example, Maps initialized first usually owns views 0–4 and the
two diagnostics get views 5 and 6. If Projects starts first, it may own view
0, with Maps and other clients allocated from the remaining IDs. Never assume
that the map view is always 0: the retained 2D network draw API accepts an
explicit view ID.

`renderAndPresent()` preflights the registered client reservations, runs their
draw submissions, and calls bgfx's global frame function once. Its
single-client `present()` helper is valid only when **exactly one** client is
registered. A hidden client may retain a reservation without drawing; it need
not submit every frame. Do not call `bgfx::frame()` from individual renderer
widgets.

**Targets and device generations:** `attachDefault()` binds the primary
backbuffer, which is owned by bgfx initialization/reset. `attachWindow()`
requires an initialized primary target and `BGFX_CAPS_SWAP_CHAIN`, creates a
secondary native framebuffer and binds its view. Secondary resizing replaces
only that framebuffer; it does **not** reset the global device. Unbind the
view before destroying its framebuffer; release all secondary targets before
device shutdown. A device restart invalidates the entire set of view-ID and
GPU-handle reservations. Do not destroy handles from the previous generation.

## Visibility, ownership and backend selection

The **first native surface to initialize the device** becomes the owner of
the default swapchain. Projects opened first can initialize it; Maps opened
first can initialize it instead. A second simultaneous surface attaches as a
secondary swapchain, provided the active backend supports native swapchains.
When the owning Projects graph is hidden it releases its GPU resources and
device, allowing Maps to become the primary owner. Normal Maps mode changes
reuse the same `MapBgfxSurface` and do not recreate the device. Ordinary tab
hide/show of an initialized map keeps its surface/resources and suspends or
resumes rendering; native-window destruction may require a device restart.

Backend selection (`AOWIS_BGFX_RENDERER=opengl|vulkan|auto`, subject to the
compiled renderer families) applies when **initializing** the device. A later
client cannot change the backend of a device already initialized by another
surface. Diagnostic logs should be checked for the *selected* renderer, not
merely an initialization request; `Map Monitor renderer: "bgfx / ..."` is
useful for this. See the [bgfx technical guide](../src/map/bgfx/README.md)
for shader families and other backend details.

The embedded Projects bgfx window is supported by default for Qt's native
`xcb`, `wayland` and `wayland-*` platforms on Linux. `QT_QPA_PLATFORM=offscreen`
and `minimal`, other desktop platforms, and platforms lacking the verified
native adapter use the Projects QPainter path. `AOWIS_PROJECTS_BGFX_GRAPH=0`
forces QPainter for debugging. If a native bgfx graph target or required HUD
shader fails, it also falls back to QPainter. This does **not** change the
WASM map's separate WebGL 2 canvas contract.

## Projects DAG data and camera contract

`ProjectRevisionGraph` owns the **authoritative** visible graph nodes,
fold/expand state, layout, camera offset/zoom, picking, keyboard and mouse
inertia, selection, and Inspector signals. `ProjectGraphBgfxViewport` only
presents that graph; `ProjectGraphBgfxScene` converts it into GPU-ready HUD
triangles and text sprites. Labels are rasterized at a zoom- and DPI-appropriate
resolution so they remain sharp when zoomed in.

`ProjectGraphBgfxScene::Index` retains graph-space cards, folded markers and
structural/ancestry/HEAD routes in a BVH. The viewport rebuilds this index
when the **displayed graph/layout changes** (including fold/unfold or
orientation), but reuses it when only panning, zooming or selecting. Each
frame queries viewport candidates and builds geometry for the visible region.
Long curves crossing the viewport remain eligible even when both endpoints
are offscreen, and visible items retain their original painter order. This is
CPU spatial culling, not automatic bgfx culling. No persistent GPU geometry
cache or additional graph LOD framework is currently required.

The current layered-DAG layout stays in the GUI. There is no Graphviz
runtime dependency. See [graph semantics](project-graph-layout.md) and the
[CPU benchmark](project-graph-performance.md).

## Diagnostic three-map mode

On native Linux only, enabling `AOWIS_BGFX_THREE_VIEW_PROBE=1` and opening
Maps creates two extra 520×350 `QWindow` map diagnostics. The production
Maps viewport is map 1; the diagnostics are maps 2 and 3. Each diagnostic:

- keeps an independent Mercator center and continuous zoom;
- pans with left-button drag and zooms around the pointer with the wheel;
- draws using a unique bgfx view and secondary native swapchain;
- shares resident imagery textures, the map tile repository and uploaded
  hydraulic network GPU geometry with the production map;
- requests tiles for its own viewport, but **does not** have a separate tile
  repository or a production multi-source scheduler;
- shows a geographic grid and center crosshair when imagery is unavailable;
- is read-only: no separate distribution editor, selection tools, flow/heatmap
  overlay, 3D globe or project merge controller is created.

The main map submits the three views in a coordinated frame. Resizing either
secondary window recreates only that target. Closing it unbinds/releases its
resources without resetting the primary map. Leaving Maps closes the
additional windows; re-entering Maps recreates them while the environment
variable is set. In the normal application, this diagnostic remains **off**.

## Verification and reproducible commands

From the configured Linux GUI source directory:

```bash
cmake --build build-linux-tests --target \
  aowis-server-gui \
  aowis-bgfx-device-manager-tests \
  aowis-bgfx-frame-coordinator-tests \
  aowis-bgfx-native-target-registry-tests \
  aowis-bgfx-three-view-camera-tests \
  aowis-project-graph-render-policy-tests \
  aowis-project-graph-bgfx-scene-tests \
  aowis-project-revision-graph-tests \
  aowis-project-graph-performance-benchmark -j 8

ctest --test-dir build-linux-tests --output-on-failure \
  -R '^(aowis-bgfx-device-manager|aowis-bgfx-frame-coordinator|aowis-bgfx-native-target-registry|aowis-bgfx-three-view-camera|aowis-project-graph-render-policy|aowis-project-graph-bgfx-scene|aowis-project-revision-graph|aowis-project-graph-performance-smoke)$'
```

Check native rendering separately from headless logic tests:

```bash
# Default Linux backend: open Projects, then Maps; test switch-back.
./build-linux-tests/aowis-server-gui

# Enable two independent secondary 2D maps.
AOWIS_BGFX_THREE_VIEW_PROBE=1 ./build-linux-tests/aowis-server-gui

# Ensure OpenGL is the *actual* first initialized backend.
AOWIS_PROJECTS_BGFX_GRAPH=0 AOWIS_BGFX_RENDERER=opengl \
AOWIS_BGFX_THREE_VIEW_PROBE=1 ./build-linux-tests/aowis-server-gui

# Isolate the graph's QPainter fallback.
AOWIS_PROJECTS_BGFX_GRAPH=0 ./build-linux-tests/aowis-server-gui
```

For the independent-map diagnostic, verify all three simultaneously visible
maps can pan and zoom independently, resize and close the secondary windows,
and switch to Projects and back without a crash. Confirm the selected GPU
backend in the log. The C++ three-view-camera test covers independent cameras,
view/target reservations and teardown **without opening native windows or
running real GPU frames**. The graph scene and performance tests likewise
exercise CPU paths rather than native presentation.

**Reported native evidence (5 November 2026):** the user observed three
simultaneously rendered, independently pannable/zoomable map views and clean
secondary attach/detach with Vulkan. A second run with Projects' GPU graph
disabled explicitly logged `Map Monitor renderer: "bgfx / OpenGL 4.3"` and
attached/detached the two extra views. The Projects graph was also observed
initializing as primary Vulkan view 0 and as secondary view 5 after Maps.
The graph's 10k-revision CPU benchmark showed ~403 ms layout, ~91 ms one-time
index build, and ~3.5 ms median repeated scene preparation with 10 visible
cards and 15 indexed candidates. Relevant Projects graph CTests were reported
passing. These observations do **not** measure GPU frame pacing or establish
Windows/WASM multi-window support, and the full three-view CTest selection
has not been separately reported in the provided logs.

## Scope and extension boundary

This repository currently contains the shared renderer infrastructure and
its native Linux diagnostics. It **does not** contain a standalone
`AOWIS-RENDER` library or an extracted, platform-neutral renderer API. An
independent renderer-library extraction is **deferred**; do not treat it as
already implemented or begin that work as part of renderer documentation.

Future QtQuick/mobile or production multi-map integration must preserve the
same single-device, explicit view-ID, target-lifetime, frame batching and
per-camera isolation contracts. It still needs separately designed QtQuick
surface adaptation, mobile/platform GPU capabilities, UI/editor input,
independent map data scheduling, and platform acceptance. Successful Linux
multi-window diagnostics are not proof of QtQuick or mobile readiness.
