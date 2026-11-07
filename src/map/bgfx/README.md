# GUI map host (`src/map/bgfx`)

The map is drawn by AOWIS-GUI-Render's `MapBgfxMapView` (`AOWIS::RenderMap`,
pinned at `external/AOWIS-GUI-Render`). How it renders, the HUD, labels,
imagery, terrain, the globe network, shaders and the bgfx build are
documented in Render's `docs/renderer.md`; embedding and GPU lifecycle in its
`docs/host-integration.md`.

This folder holds what the GUI adds as a host:

| File | Role |
| --- | --- |
| `map_bgfx_surface.*` | `MapBgfxSurface`: the Qt Widgets host of the view (`MapBgfxMapViewHost`). Embeds the native window, forwards widget events, keeps keyboard focus, supplies settings and assets (terrain detail, model textures, HUD images, navigation shortcuts, units). Every map method forwards to the view |
| `map_bgfx_editor_overlay.*` | Map Editor drawing on top of the network: full-colour icons, previews, selections, sprite hit rectangles, cache-selection rectangles (2D and globe) |
| `map_bgfx_editor_icons.*` | The editor's full-colour entity pixmaps with selected / error / stale glows, composited once per size and highlight into the overlay atlas |
| `map_bgfx_globe_hud_controller.*` | Applies globe HUD commands (sliders, compass, X-Ray/Hide/Solid) to `MapModel` and supplies the globe HUD state in the user's units |
| `map_bgfx_legend_content.*` | Builds the HUD legend from the monitor's metrics and ranges and applies the user's legend choices |
| `map_bgfx_globe_model_images.*` | Loads the tank and reservoir textures (`:/map-3d/`) handed to the view |

Around it: `src/map/core/map_widget.*` (`MapWidget`, the navigation
controller: keys, wheel, mouse, joystick, smooth zoom in `map_zoom_animation.h`),
`map_navigation_widget.*` (the left menu, labels switches), Wayland pointer
lock, `gui_map_provider_catalog.*` (the process's `MapProviderCatalog`,
fallbacks in the GUI configuration), and `src/map/render/` (the network
symbology resolver from hydraulic results, entity pixmaps). The map server
transport (REST or the Standalone in-process server) is chosen in
`src/services/map_server_transport_factory.*`.

The map has no CPU fallback: if bgfx cannot start, Map Monitor reports a GPU
status instead of painting the map with QPainter.

## Hosting

- `MapBgfxSurface` embeds `nativeInputWindow()` with
  `QWidget::createWindowContainer()`. Desktop native windows are captured by
  Render's `BgfxQtNativeSurface`; WebAssembly positions the browser canvas
  through `window.aowisBgfxMap`.
- One `MapBgfxSurface` and one `MapModel` live for the whole Maps workspace
  (see [Shared Maps workspace](#shared-maps-workspace)). Mode and page
  switches never restart bgfx.
- Fullscreen is in place (`MainWindow::setMapFullscreen()`): the map widget
  stays parented, and the window hides its other parts, so the native window
  is never reparented. Qt Wayland would destroy its `wl_surface` on reparent
  without a `QPlatformSurfaceEvent`, and a Vulkan swapchain alive on it ends
  the Wayland connection.
- `MapWidget` is a pure navigation controller that draws nothing; the view's
  frame loop is its clock (`setNavigationFrameHandler()` ->
  `MapWidget::advanceNavigationFrame(dt)`), it starts no timer of its own.
  The footer's map readouts update at most every 100 ms.
- WASM builds drop the function-name section unless
  `AOWIS_WASM_FUNCTION_NAMES=ON`.

## Input

The embedded native `QWindow` receives all input over the map.

- Mouse events go to the HUD first (`MapMonitorContainer` /
  `MapEditorContainer` call `handleHudMouseEvent()`); the rest are sent to
  `MapWidget` with `QCoreApplication::sendEvent`, so the containers'
  click-selection filters still run. Keys and the wheel go to `MapWidget`.
- Keyboard focus: a press on the map or a HUD element calls
  `MapBgfxSurface::focusNavigationInput()` (also the view's
  `mapViewFocusNavigationInput()`). In the monitor the surface takes widget
  focus and activates the native window (Wayland subsurfaces never get
  keyboard focus of their own). In the editor the keys go to its canvas
  widget (`setKeyboardFocusWidget()`), and the native window does not take
  focus (`setNativeWindowAcceptsFocus(false)`), so the editor's window
  shortcuts keep working.
- Pan joystick: the HUD's `JoystickDrag` Begin calls
  `MapWidget::beginJoystickPan()`, which captures the pointer and pans while
  the knob is deflected, with the same pointer lock as the globe orbit.
- Orbit (Ctrl or middle button) keeps going at the screen edges. X11,
  Windows and macOS warp the hidden cursor back to the orbit start; Wayland
  locks the pointer and reads relative motion (`MapWaylandPointerLock`). The
  lock is requested on the top-level window's `wl_surface` and on the map
  window's own (a `wl_subsurface`, `MapWidget::setOrbitPointerLockWindow`),
  because compositors differ in which of the two they honour (KWin and older
  mutter only the main surface). The log names the surface the compositor
  activated, once per run, or warns when it activated none.
- A pan or zoom key is released by its physical key (native scan code, else
  virtual key, else Qt key), remembered at the press: keys pressed in
  between change modifiers or layout level (AltGr turns E into the euro
  sign), which used to leave the motion running.
- No frames are rendered while the native window is unexposed, so the vsync
  swap cannot block the GUI thread on a hidden Wayland surface.

## Network inputs

- The containers build the `NetworkRenderSnapshot` from `HydraulicData` and
  send it on changes; `MapNetworkSymbologyResolver` fills
  `MapNetworkRenderSymbology` per time step from the simulation results.
- Renames change no geometry: `HydraulicData::setNodeId()` & co. rebuild the
  current snapshot (same render ids), the retained scene takes the IDs over
  on the visual-revision update, and the containers re-send the snapshot on
  `signalNodeChanged` / `signalLinkChanged` / `signalDemandPointChanged`.
- Hydraulic identity, elevations and editing stay in the GUI; Render keeps
  no hydraulic model.

## Map Editor rendering

Network editing is 2D. The editor overlay (`setOverlayBuilder()`) draws the
full-colour node and device-link icons, previews, selections and their hit
rectangles; labels use its icon rectangles (`setLabelSymbolRects()`).

On the globe (`setGlobeOverlayBuilder()`, a `GlobeProjector` per frame:
WGS84 -> logical pixels on the drawn relief, hidden behind the camera and
beyond the horizon):

- the cache sections (Tile Cache, Terrain Cache) select tiles: a right drag
  picks two ground corners (terrain-aware) and the tile range is drawn on the
  surface, map tiles green, terrain tiles brown
  (`MapBgfxEditorOverlay::buildGlobe()`);
- in 3D Editor mode the full-colour icons are projected onto the globe
  instead of the monitor's generic GPU symbols. The editor's 2D icon path and
  the retained globe network geometry are unchanged.

### 3D Editor icon visibility

The overlay keeps a **CPU-only retained ECEF kd-tree** of the icon anchors
(Render's `map_bgfx_globe_icon_index.h`). It is rebuilt only when the
snapshot's geometry revision, identity or node/link count changes; pan, zoom,
rotation, terrain updates and workspace changes never rebuild it or the GPU
geometry. Each frame supplies four view-side clipping planes, with an
allowance for the largest icon and a conservative one for exaggerated relief,
so whole branches are rejected before any terrain-height lookup. The projector
still checks horizon, drawn DEM height and pixel bounds.

Icons are drawn back to front: the editor depth-sorts its projected sprites
before building the canvas (the index returns kd-tree order; a source-index
tie-breaker keeps equal-distance icons stable; a fully enclosed frustum uses a
linear range). The monitor's GPU icons update only a 32-bit draw-index stream
when the eye or icon geometry changes.

**Terrain-height reuse.** The projection's mesh-ray and DEM sample is cached
across frames by `map_bgfx_globe_height_cache.h`, keyed by the exact
coordinate. Unavailable terrain is never cached as a height: failed samples
are retried after at most eight overlay frames (the ellipsoid stands in) or
at once when the drawn terrain changes. The cache is cleared on a new drawn
surface revision, a vertical exaggeration change, a new terrain repository or
a bgfx shutdown, and bounded to 262,144 entries (FIFO). Screen position, depth
order and visibility are still computed every frame.

### Measuring large networks

`aowis-map-bgfx-globe-icon-index` runs a deterministic 100,000-anchor query
regression without Qt or GPU. For synthetic CPU workloads, build with
`./compile_linux_tests.sh --both` and run:

```bash
./build-linux-tests-release/aowis-map-bgfx-globe-icon-benchmark \
    > /tmp/aowis-globe-icon-synthetic.jsonl
```

It uses the real kd-tree on 10k, 50k and 100k anchors in three views
(regional, urban_dense, fully_visible), checks the culled result against a
full projection and draw-order pass, and reports build time, memory estimate,
median and p95 CPU cost, candidates, leaves examined and unintended rebuilds,
one JSON object per scenario (`--smoke` runs in `aowis-tests`). These are
simulated projections: no DEM sampling, rasterization or GPU. A kd-tree can be
slower than a full scan when most anchors are visible.

For real GUI CPU costs, load 10k, 50k and 100k demand-point networks in
Distribution Editor, enable 3D, let terrain and icons load, and record each
size and backend as its own process:

```bash
AOWIS_MAP_RENDER_DIAGNOSTICS=1 ./build-linux/aowis-server-gui \
    2>&1 | tee /tmp/aowis-editor-3d-10k.log
# Pan, rotate and zoom in 3D for 15-30 s, then close the GUI.
python3 tools/report_globe_editor_performance.py \
    /tmp/aowis-editor-3d-10k.log --min-icon-frames 30 --min-anchors 10000 --json
```

The counters `globe_editor_icon_frame`, `_index_query`, `_projection` and
`_draw` time the real CPU phases; `globe_editor_icon_index_build`,
`_anchors`, `_candidates`, `_visible` and `globe_terrain_height_cache_hit`,
`_sample`, `_unavailable_reuse`, `_eviction` show culling and cache reuse.
The report uses differences of successive cumulative snapshots, skips idle
intervals and weighs them by overlay invocations. It measures CPU submit work,
not GPU time or FPS: for those, record Chromium's frame metrics in WASM and
OS process and GPU metrics on Linux and Windows, cold start and warmed
navigation separately, against the previous commit on the same machine.

## Shared Maps workspace

The Maps workspace keeps one `MapBgfxSurface` and one `MapModel` while
switching between Distribution Monitor, Distribution Editor, Drainage
Monitor, Drainage Editor and Map Management. The drainage sidebars are empty
for now; Map Management keeps tile selection and cache tools. Map, imagery,
terrain, camera, navigation and the common HUD stay live in every mode.

- `setDistributionNetworkVisible(false)` hides the distribution network's
  draws, labels, editing overlays, 3D entities, heatmap and picking, but keeps
  the retained scene and its GPU geometry. Map Management's cache-selection
  overlay stays visible.
- Returning to a distribution mode restores that mode's symbology and input
  handlers.
- No mode change and no main-page change (Maps to Projects, Dashboard,
  Settings and back) may call `bgfx::shutdown()` or `bgfx::init()`. Check on
  a machine with a GPU:

  ```bash
  AOWIS_MAP_RENDER_DIAGNOSTICS=1 ./build-linux/aowis-server-gui 2>&1 | tee map-tabs.log
  python3 tools/check_map_workspace_diagnostics.py map-tabs.log --min-switches 0 --min-cache-switches 0
  ```

  The check rejects any GPU shutdown or repeated initialization.

## Build and test

- Native Linux: `./compile_linux.sh` (GUI), `./compile_linux_tests.sh
  --release` (build first, then tests).
- `./compile_linux.sh --shader-tools` regenerates Render's committed shaders
  (Render's `docs/renderer.md`, "bgfx build").
- WASM: `sudo ./compile_wasm.sh`, with the committed essl shaders.
- 2D performance: `AOWIS_MAP_RENDER_DIAGNOSTICS=1` and
  `tools/report_2d_performance.py`.
