# Rendering architecture

What the GUI renders with bgfx, and the rules every GPU surface in the
process follows. The map engine itself lives in AOWIS-GUI-Render
(`external/AOWIS-GUI-Render`); see [Render integration](render-integration.md)
for the dependency boundary and Render's own `docs/` for its internals.

## What renders where

| Surface | Renderer | State ownership |
| --- | --- | --- |
| Maps: Distribution Monitor / Editor, Drainage, Map Management | One persistent `MapBgfxSurface` hosting Render's `MapBgfxMapView`: 2D and globe | One `MapModel` and `MapWidget`, one native map window |
| Projects revision/HEAD graph | bgfx `ProjectGraphBgfxViewport` on native Linux X11/Wayland; QPainter elsewhere | `ProjectRevisionGraph`: layout, navigation, picking, selection, Inspector routing |
| Two extra 2D map windows | bgfx, opt-in `AOWIS_BGFX_THREE_VIEW_PROBE=1` | Two `MapBgfxThreeViewCamera` instances; **developer diagnostic only** |

The Maps page has **one shared map viewport for all five modes**. The
Projects graph is a different surface that shares the bgfx device; it is not
another map.

The map has **no QPainter fallback**: on an unsupported or failed bgfx
backend it reports a renderer status. The QPainter fallback belongs to the
Projects graph only.

## One device, many surfaces

All bgfx calls run on the GUI thread. No QWidget, QWindow or view initializes
bgfx itself. The pieces, all in Render except the GUI hosts:

| Component | Responsibility |
| --- | --- |
| `BgfxDeviceManager` | The only `bgfx::init()` / `bgfx::shutdown()`; primary-device lease; client retirement and reinitialization when the owning surface is lost |
| `BgfxFrameCoordinator` | Generation-scoped, non-overlapping view-ID reservations; all clients submitted before **one** `bgfx::frame()` |
| `BgfxNativeTargetRegistry` | The default swapchain and the secondary native framebuffers; each secondary resized and released on its own |
| `BgfxNativeSurface` / `BgfxQtNativeSurface` | Toolkit-free native window descriptor; the Qt adapter fills it from a `QWindow` (X11, Wayland, Windows, macOS) |
| `MapBgfxMapView` | The map: GPU content, imagery, terrain, HUD, labels, picking, diagnostic map windows |
| `MapBgfxSurface` (GUI) | Qt Widgets host of the map view ([map host guide](../src/map/bgfx/README.md)) |
| `ProjectGraphBgfxViewport` (GUI) | Projects' native window, GPU HUD draws, text atlas, spatial index, view and target lifetime |

Rules that matter for GUI code:

- **View IDs are process-global.** Maps reserves five contiguous views (main
  map and four globe heatmap bake passes); Projects and each diagnostic
  window reserve their own. Whoever initializes first gets view 0; never
  assume the map is view 0.
- **One frame for everybody.** `renderAndPresent()` runs the registered
  clients' submissions and calls `bgfx::frame()` once. A hidden client keeps
  its reservation without drawing. No widget calls `bgfx::frame()` itself.
- **First owner wins.** The first surface to initialize the device owns the
  default swapchain; a second visible surface attaches as a secondary
  swapchain (if the backend supports them). A hidden Projects graph releases
  its GPU resources and the device, so Maps can become the owner. Maps mode
  changes and ordinary tab hide/show never recreate the device; only losing
  the native window does.
- **Retire before destroying.** Secondary clients register with the device
  manager before allocating views or targets; the owner retires them before
  shutdown, and a shutdown with an unretired client is rejected. Release a
  swapchain before its Wayland surface is destroyed or replaced; resize
  rejects a changed native identity.
- **Depth is mandatory in 3D.** Secondary swapchains and resizes request
  D24S8. Without it, terrain skirts show through nearer ground as vertical
  imagery stripes.
- **Backend selection** (`AOWIS_BGFX_RENDERER=opengl|vulkan|auto`, within
  the compiled renderers) applies only when the device is initialized; a
  later surface cannot change it. Check the log for the selected backend
  (`Map Monitor renderer: "bgfx / ..."`).

The WebAssembly map uses its own browser canvas and WebGL 2 context; none of
the multi-window behaviour applies there.

## Projects graph

The embedded bgfx graph is the default on Qt's `xcb`, `wayland` and
`wayland-*` platforms on Linux. `offscreen`, `minimal`, other desktop
platforms, a failed native target or a missing HUD shader use the QPainter
path; `AOWIS_PROJECTS_BGFX_GRAPH=0` forces it.

`ProjectRevisionGraph` owns the graph nodes, fold state, layout, camera,
picking, keyboard and mouse inertia, selection and Inspector signals.
`ProjectGraphBgfxViewport` only presents it; `ProjectGraphBgfxScene` turns
it into HUD triangles and text sprites, with labels rasterized for the
current zoom and DPI.

`ProjectGraphBgfxScene::Index` keeps cards, fold markers and routes in a
BVH. It is rebuilt when the displayed graph or layout changes (fold, unfold,
orientation) and reused for pan, zoom and selection; each frame queries the
viewport's candidates, keeps long curves crossing the viewport, and preserves
painter order. The layered-DAG layout has no Graphviz dependency; see
[graph semantics](project-graph-layout.md) and the
[CPU benchmark](project-graph-performance.md).

## Diagnostic three-map mode

On native Linux, `AOWIS_BGFX_THREE_VIEW_PROBE=1` opens two extra 520×350 map
windows next to the Maps viewport (map 1). Each:

- has its own Mercator centre and continuous zoom, pans with a left drag
  and zooms about the pointer with the wheel;
- draws through its own view ID and secondary swapchain;
- shares imagery textures, the tile repository and the uploaded network GPU
  geometry with the main map, and requests tiles for its own viewport;
- shows a grid and crosshair without imagery;
- is read-only: no editor, selection, heatmap or globe.

The three views are submitted in one frame. Resizing or closing a diagnostic
window affects only its own target. Leaving Maps closes them; returning
reopens them while the variable is set.

## Input loss and gesture cancellation

`MapWidget` owns navigation even though the embedded native window receives
the events. On focus loss, `MapBgfxSurface` forwards `FocusOut` and
`WindowDeactivate` to the navigation cancellation, and `MapWidget` watches
application deactivation for platforms that omit native releases.
Cancellation stops key movement, mouse panning, globe orbit and free look
and their pointer lock and the HUD joystick, and restores the cursor without
warping it into an inactive window. Focus changes caused by the map's own
click focus routing are not a loss: while a mouse button is held and focus
stays in the map's top-level window, Render ignores them (on Windows every
click moves the system focus between the top-level and the embedded native
window, which cancelled every drag), and `MapBgfxSurface::focusNavigationInput()`
returns at once when the native window already has focus. The editor canvas also aborts unfinished entity moves and
rectangle drags, keeping completed selections and an armed creation tool. The
diagnostic windows drop their drag state too.

Manual check: hold a pan or zoom key, a left drag, a middle-button globe orbit
or an editor drag, then alt-tab away before releasing. Back on the map, the
old gesture must not resume and no cursor grab may remain. Repeat on X11,
Wayland and Windows (there also: a plain left drag pans).

## Globe camera gestures

`MapWidget` starts the globe gestures; Render's camera modes and terrain
following are described in Render's `docs/renderer.md` ("Camera modes and
terrain following").

| Input | Gesture | Camera mode |
| --- | --- | --- |
| Orbit hold key (default Ctrl) + mouse move, or middle-button drag | Orbit around the terrain under the crosshair, tilt 0 to 90 degrees | Orbit |
| Look hold key (default Alt) + mouse move | Free look from the camera, straight down to straight up | FreeLook (kept after release) |
| Left drag, keys, HUD joystick | Pan; follows the terrain in both modes | unchanged |

The hold keys are in Settings → Keyboard Shortcuts, group "Globe Navigation
(hold)" (`GuiShortcutRegistry` hold modifiers, `GuiHoldModifierId`); the
middle mouse button is listed there read-only. A hold gesture runs only
while no mouse button is pressed. When the look key is released, the
application event filter swallows the release, so a bare Alt does not select
the window's menu bar on Windows. `MapWidget::navigationGestureChanged()`
drives the HUD hint; `MapBgfxSurface` opens the globe controls card once on
the first globe view (`map_hints/globe_controls_help_shown`). The HUD tilt
slider follows the mode (`MapBgfxGlobeHudController`).

## Verification

```bash
cmake --build build-linux-tests --target \
  aowis-server-gui aowis-render-core-link-tests \
  aowis-bgfx-device-manager-tests aowis-bgfx-frame-coordinator-tests \
  aowis-bgfx-native-target-registry-tests aowis-bgfx-three-view-camera-tests \
  aowis-project-graph-render-policy-tests aowis-project-graph-bgfx-scene-tests \
  aowis-project-revision-graph-tests aowis-project-graph-performance-benchmark -j 8

ctest --test-dir build-linux-tests --output-on-failure \
  -R '^(aowis-render-core-link|aowis-bgfx-device-manager|aowis-bgfx-frame-coordinator|aowis-bgfx-native-target-registry|aowis-bgfx-three-view-camera|aowis-project-graph-render-policy|aowis-project-graph-bgfx-scene|aowis-project-revision-graph|aowis-project-graph-performance-smoke)$'
```

These tests run without native windows or GPU frames. Check native rendering
by hand:

```bash
./build-linux-tests/aowis-server-gui                                   # Projects, then Maps, and back
AOWIS_BGFX_THREE_VIEW_PROBE=1 ./build-linux-tests/aowis-server-gui      # three maps
AOWIS_PROJECTS_BGFX_GRAPH=0 AOWIS_BGFX_RENDERER=opengl ./build-linux-tests/aowis-server-gui
AOWIS_PROJECTS_BGFX_GRAPH=0 AOWIS_BGFX_RENDERER=vulkan ./build-linux-tests/aowis-server-gui
AOWIS_PROJECTS_BGFX_GRAPH=0 ./build-linux-tests/aowis-server-gui        # graph QPainter path
```

With the diagnostic windows, check that all three maps pan and zoom
independently, that the secondary windows resize and close cleanly, and that
switching to Projects and back does not crash. Results are recorded in
[Verification status](verification-status.md).
