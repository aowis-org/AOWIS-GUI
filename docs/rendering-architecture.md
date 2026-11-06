# Rendering architecture

This is the **current renderer contract** for the 6 November 2026 GUI source.
It covers the production Maps workspace, native Linux Projects revision graph,
shared renderer submodule, and opt-in multi-window diagnostics. It describes
current behavior and ownership rather than implementation stages. For the
source/dependency boundary and remaining map-engine work, see
[Shared renderer integration](renderer-extraction-readiness.md).

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

## Shared renderer build boundary

`external/AOWIS-GUI-Render` is a **pinned standalone repository**, not an
internal GUI-only CMake library. GUI consumes five shared targets:

| Target | Current role |
| --- | --- |
| `AOWIS::RenderGeometry` | Pure C++20 projection, camera, icon/height-cache and imagery fallback helpers |
| `AOWIS::RenderNetwork` | 2D network CPU scene and mesh preparation |
| `AOWIS::RenderGlobe` | 3D model, flow, link, buried-run geometry, retained scene invalidation and terrain topology |
| `AOWIS::RenderDevice` | Single bgfx device, frame coordinator, native swapchain registry |
| `AOWIS::RenderCore` | 2D and globe network GPU submission, atlases, shader resources, terrain buffers/index caches/drawing, sky and heatmap GPU ownership, HUD caches |

The library registers the shader resources itself at
`:/aowis/map/bgfx/<family>/<shader>.bin`. The `aowis-render-core-link`
test verifies that actual Render objects and embedded resources link into a
GUI-independent test executable. GUI still supplies the single native map
viewport, scene and frame scheduling, camera adaptation, image resources,
network MODEL adapter, picking/editor tools and the Qt/WebAssembly host.

**The remaining common map-engine pipeline is not yet in Render:**
`MapGlobeSurfaceScene`, `MapGlobeSurfacePreparation`,
`MapTerrainMeshScheduler`, `MapTerrainTile`, `MapTerrainRepository`,
`MapTileRepository`, `MapTileStreamingPolicy` and provider-source adapters
still coordinate CPU terrain/imagery work in GUI. These need a neutral
engine/source split; merely moving `MapBgfxSurface` verbatim would introduce
Qt Widgets, service and MODEL dependencies. See
[Shared renderer integration](renderer-extraction-readiness.md).

Mobile currently exercises shared device/geometry contracts but has not
adopted the production 2D/globe GPU pipeline. A successful GUI build is not
Android or WebAssembly GPU qualification.

## Process-wide bgfx device and frame contracts

All bgfx calls are coordinated on the application's common GUI/render thread.
Do not initialize bgfx independently for a new QWidget, QWindow or QtQuick
view. The relevant components are:

| Component | Responsibility | Source |
| --- | --- | --- |
| `BgfxDeviceManager` | Sole `bgfx::init()` / `bgfx::shutdown()` entry point; visibility-aware primary-device lease; retirement/reinitialization when the owning native surface is lost or relinquished | `AOWIS-GUI-Render/src/map/bgfx/bgfx_device_manager.{h,cpp}` |
| `BgfxFrameCoordinator` | Device-generation-scoped, non-overlapping bgfx view-ID reservations; batch all active clients before **one** `bgfx::frame()` | `AOWIS-GUI-Render/src/map/bgfx/bgfx_frame_coordinator.{h,cpp}` |
| `BgfxNativeTargetRegistry` | Bind the initial/default swapchain and own additional native swapchain framebuffers; resize and release each secondary independently | `AOWIS-GUI-Render/src/map/bgfx/bgfx_native_target_registry.{h,cpp}` |
| `BgfxNativeSurface` | Toolkit-independent native window/display/type/physical-size descriptor, converted to bgfx swapchain and primary-init data | `AOWIS-GUI-Render/src/map/bgfx/bgfx_native_surface.h` |
| `BgfxQtNativeSurface` | GUI-side QWindow adapter: obtains XCB/Wayland native handles and device-pixel dimensions, or native desktop window IDs | `src/map/bgfx/bgfx_qt_native_surface.{h,cpp}` |
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

**Native surface boundary:** `BgfxQtNativeSurface::capture(QWindow *)` is the
only desktop native handle discovery path for Maps, Projects and diagnostic
windows. It runs in the GUI/Qt host, not the reusable device/target layer.
`BgfxNativeSurface` carries native **window**, **display**, window-handle
**type**, and **physical pixel** dimensions; it carries no QWindow or QWidget.
The same descriptor supplies `bgfx::Init` for a primary surface and a complete
`bgfx::SwapChain` for secondary targets (including its display field). **Depth
attachments are mandatory for 3D:** the pinned `bgfx::Init` defaults to D24S8,
whereas a freshly constructed `bgfx::SwapChain` has *no depth attachment*.
`BgfxNativeSurface::configurePrimary()` therefore modifies the existing Init
swapchain **in place**, preserving its depth format and other defaults;
`BgfxNativeSurface::swapChain()` and `resizeSwapChain()` explicitly use D24S8.
`MapBgfxSurface::synchronizeGeometry()` uses `resizeSwapChain()` for
`bgfx::reset()`; it must not use a bare default descriptor. This is a
**rendering correctness contract**, not an optional terrain tweak: disabling
depth makes imagery-coloured tile skirts show through nearer ground despite
`BGFX_STATE_DEPTH_TEST_LEQUAL` and `BGFX_STATE_WRITE_Z`. Skirt geometry remains
necessary to conceal seams between terrain LODs.

An unavailable Wayland `wl_surface` must be retried after expose; a native window
or display **replacement** must release the old framebuffer before Qt destroys
the old surface, then attach a new one. Resize deliberately rejects a changed
native identity instead of issuing a bgfx command against a stale handle.
The browser's independent WebGL canvas/context setup remains separate; no
new multi-canvas behavior is implied by this desktop adapter.

**Targets and device generations:** `attachDefault()` binds the primary
backbuffer, which is owned by bgfx initialization/reset. `attachWindow()`
requires an initialized primary target and `BGFX_CAPS_SWAP_CHAIN`, creates a
secondary native framebuffer and binds its view. Secondary resizing replaces
only that framebuffer; it does **not** reset the global device. Unbind the
view before destroying its framebuffer; release all secondary targets before
device shutdown. A device restart invalidates the entire set of view-ID and
GPU-handle reservations. Do not destroy handles from the previous generation.

**Multi-client lifecycle barrier:** a successful device initialization assigns
an increasing generation token. The primary owner is implicit; secondary GPU
clients register their identity, the generation and a retirement callback with
`BgfxDeviceManager` **before** allocating a view or target. Before destroying
its own GPU resources, the primary calls `retireClients()`; `shutdown()` also
checks the retirement barrier. Callbacks release GPU objects, targets and view
reservations, then unregister their leases. If any registered client refuses
to retire, throws, or an unmanaged view/target remains, shutdown is rejected
and the existing device stays alive rather than clearing live handles. Stale
generation tokens cannot unregister clients from a recreated device. A
Projects graph retired while visible schedules a new attach after the owner
finishes shutdown. Map diagnostic windows are retired and closed before the
owning map's native surface is destroyed. This is a single-render-thread
contract, not thread-safe or asynchronous device arbitration.

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
  aowis-render-core \
  aowis-render-core-link-tests \
  aowis-bgfx-device-manager-tests \
  aowis-bgfx-frame-coordinator-tests \
  aowis-bgfx-native-target-registry-tests \
  aowis-bgfx-three-view-camera-tests \
  aowis-map-input-focus-policy-tests \
  aowis-project-graph-render-policy-tests \
  aowis-project-graph-bgfx-scene-tests \
  aowis-project-revision-graph-tests \
  aowis-project-graph-performance-benchmark -j 8

ctest --test-dir build-linux-tests --output-on-failure \
  -R '^(aowis-render-core-link|aowis-bgfx-device-manager|aowis-bgfx-frame-coordinator|aowis-bgfx-native-target-registry|aowis-bgfx-three-view-camera|aowis-map-input-focus-policy|aowis-project-graph-render-policy|aowis-project-graph-bgfx-scene|aowis-project-revision-graph|aowis-project-graph-performance-smoke)$'
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

# Force Vulkan as the first bgfx owner for controlled backend comparison.
AOWIS_PROJECTS_BGFX_GRAPH=0 AOWIS_BGFX_RENDERER=vulkan \
  ./build-linux-tests/aowis-server-gui
```

For the independent-map diagnostic, verify all three simultaneously visible
maps can pan and zoom independently, resize and close the secondary windows,
and switch to Projects and back without a crash. Confirm the selected GPU
backend in the log. The C++ three-view-camera test covers independent cameras,
view/target reservations and teardown **without opening native windows or
running real GPU frames**. The graph scene and performance tests likewise
exercise CPU paths rather than native presentation.

**Reported native evidence (5 November 2026):** the user observed three
simultaneously rendered, independently pannable/zoomable maps and clean
secondary attach/detach with Vulkan; a separate run with Projects' GPU graph
disabled explicitly logged `Map Monitor renderer: "bgfx / OpenGL 4.3"` and
attached/detached the two extra views. Projects was observed as both primary
Vulkan view 0 and secondary view 5 after Maps. Following the internal-library
and native-surface changes, a reported 3D globe regression exposed the loss of
a swapchain depth attachment: terrain skirts appeared as vertical imagery
stripes. The corrected D24S8 handling is present in this source, and the user
subsequently confirmed the visual problem was fixed. The corrective source
contains regression coverage in `bgfx_native_target_registry_test.cpp`.
That confirmation is **visual evidence**, not a substitute for recording a
fresh complete CTest run and Vulkan/OpenGL native acceptance after the final
patch.
The graph's 10k-revision CPU benchmark showed ~403 ms layout, ~91 ms one-time
index build, and ~3.5 ms median repeated scene preparation with 10 visible
cards and 15 indexed candidates. Relevant Projects graph CTests were reported
passing. These observations do **not** measure GPU frame pacing or establish
Windows/WASM multi-window support. Complete post-library-split CTest and
native regression results are still separate acceptance evidence; see
[Verification status](verification-status.md).

## Scope and extension boundary

The production globe/network GPU buffers, shader programs, vertex layouts,
terrain topology, sparse terrain uploads, base terrain draws, sky and heatmap
GPU resources have already moved into `AOWIS-GUI-Render`. The GUI owns
**which** visible network and terrain content to prepare, **when** to request
it, and the host's window/frame/event loop. That boundary is provisional
until DEM decoding, tile selection, streaming, and terrain preparation become
reusable engine modules with host-supplied data sources.

All future Qt Quick/Mobile and multi-map integrations must preserve the
single-device, explicit view-ID, target-lifetime, frame batching, depth and
per-camera-isolation contracts. The Linux diagnostic views do not establish
production multi-map or Android support.

## Map input loss and gesture cancellation

`MapWidget` is the navigation state owner even when bgfx's embedded native
`QWindow` receives mouse and keyboard events. Both the map's widget and its
native surface must cancel held navigation on focus loss, not merely clear
keyboard flags. `MapBgfxSurface` forwards `FocusOut` and `WindowDeactivate`
into the configured navigation cancellation callback; `MapWidget` also watches
application deactivation for platforms that omit the corresponding native
release events. Cancellation stops key-driven movement, active mouse panning,
3D orbit/pointer lock and HUD joystick input, and restores the cursor without
warping it into an inactive window. The editor canvas additionally aborts
unfinished entity moves and rectangle drags while preserving completed
selections and an armed entity-creation tool. The optional three-view probe
windows release their local drag state on lost focus as well.

`aowis-map-input-focus-policy` tests which native-window and application
transitions trigger cancellation. Native manual regression: hold a pan/zoom key,
left-drag, middle-button globe orbit (including pointer lock), or editor drag;
alt-tab or activate another window before releasing. Returning to the map must
not resume the old gesture or leave a cursor grab active. Repeat on X11 and
Wayland when available.

### Shared production 2D network implementation

`AOWIS-GUI-Render` now owns `MapBgfx2dNetworkInput`, `MapBgfx2dSceneVariants`,
`MapBgfx2dMesh`, `MapBgfx2dGpu`, and the shared node decluttering algorithm.
`AOWIS::RenderNetwork` provides CPU scene and mesh staging without bgfx, and
`AOWIS::RenderCore` supplies GPU network submission.
The GUI's `map_bgfx_2d_snapshot_adapter.h` translates its retained snapshot to
render IDs, entity class codes, geodetic positions and link endpoint identity.
That conversion runs on geometry revision changes / first-time view variant
preparation, never during camera, HUD or hydraulic value updates. Shader value
texture/palette binding remains host-owned through `MapBgfx2dStyleBinding`,
implemented by the GUI `MapBgfxEntityStyle` and suitable for a future Mobile
backend. This does not change the 3D globe renderer or the Mobile EGL loop.
