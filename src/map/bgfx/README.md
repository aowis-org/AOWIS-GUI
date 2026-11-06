# bgfx Maps renderer (2D, globe and shared workspace)

The shared production map surface draws the Map Monitor and Map Editor on
desktop and WASM; it is always built. The same process-wide bgfx device and
frame coordinator also serve the native Linux Projects graph and, when
enabled, two read-only independent diagnostic map windows. See
[Rendering architecture](../../../docs/rendering-architecture.md) for the
current cross-surface lifecycle contract.

The pinned `external/AOWIS-GUI-Render` Git submodule provides the shared
`AOWIS::RenderDevice`, `AOWIS::RenderCore`, and Qt/bgfx-independent
`AOWIS::RenderGeometry` CMake targets. The renderer owns the bgfx device,
frame/target coordination, imagery and HUD GPU helpers, shader resources,
continuous Mercator projection, secondary-view camera, globe icon index,
terrain-height cache, imagery fallback UV math, pan reuse helper,
retained 2D scene preparation, mesh/chunk staging and bgfx network draw submission.
Shared shader resources are explicitly registered by the library.

The GUI executable owns the QWidget/QWindow surface adapter,
`BgfxQtNativeSurface`, hydraulic model/data services, authoritative network snapshots and
`map_bgfx_2d_snapshot_adapter.h`, entity styles, the still-GUI-specific 3D
scene controller, editor tools and Projects UI.
See the [rendering architecture](../../../docs/rendering-architecture.md#shared-renderer-build-boundary).

Desktop native Qt windows are captured through `BgfxQtNativeSurface` into the
Qt-free `BgfxNativeSurface` descriptor (window, display, handle type,
physical pixels). `BgfxNativeTargetRegistry` consumes that descriptor for
secondary swapchains and rejects a resized target whose native identity has
changed. Release the old swapchain **before** destroying or reparenting its
Wayland surface. The WebAssembly canvas/context remains a separate path.

**Swapchain depth is not optional for the globe.** The current pinned bgfx
`Init` defaults to D24S8, but a default-created `SwapChain` has none.
`BgfxNativeSurface::configurePrimary()` preserves the primary Init defaults;
`swapChain()` and `resizeSwapChain()` request D24S8 explicitly for secondary
windows and resizing. `MapBgfxSurface::synchronizeGeometry()` uses that resize
descriptor, including after tab hide/show and window resizes. Loss of this
depth buffer makes LOD tile skirts draw over foreground terrain, producing
vertical imagery stripes. The skirts themselves must remain.
See [Renderer library readiness](../../../docs/renderer-extraction-readiness.md)
for the compiled target and independent link test.

The map has no CPU renderer fallback: if bgfx cannot start, Map Monitor
reports a GPU status instead of painting the map with QPainter.
bgfx renders both the 2D map and 3D globe on native desktop and WebAssembly
(WebGL 2 in the browser), and both Distribution Monitor and Editor use that
same surface. Neither QRhi, a separate JavaScript map renderer nor a CPU
`MapEditorRenderer` is part of the current product. The old native
`gui/map_desktop_renderer` preference is discarded, and any browser
`map_wasm_renderer` preference is ignored. Embedded bgfx shader families are
selected for the active backend; Qt ShaderTools is not required.

## Retained 2D network GPU drawing

* Inputs: GUI `MapRetainedNetworkScene` remains authoritative; the GUI
  adapts geometry-only revisions into Render's `MapBgfx2dNetworkInput`.
  Render owns `MapBgfx2dScene`, CPU mesh preparation and `MapBgfx2dGpu` GPU
  draw submission. `MapBgfxEntityStyle` still lives in the GUI and implements
  Render's `MapBgfx2dStyleBinding` for entity textures and colors.
* **Spatial chunks** (`map_bgfx_2d_mesh.h`): primitives are grouped by a fixed
  world grid (zoom-12 cells, ~9.8 km). Each chunk has its own local origin
  (float error ~0.002 px at zoom 19 for networks of any extent) and bounds
  for per-frame view culling. Off-screen chunks issue no draw calls.
* **Thick lines**: one indexed quad per segment, extruded in `vs_line_2d.sc`
  in screen pixels with square caps of half the width, which close polyline
  joins. Constant on-screen width at every zoom; a metre-based width is
  converted per frame with `MapBgfx2dChunking::pixelsForMeters()` (continuous
  in the zoom). GL/WebGL line primitives are not used (1 px on core/WebGL).
* **Nodes**: unit-corner quads scaled by a size uniform and cut to
  antialiased circles in `fs_marker_2d.sc`. Every snapshot node is drawn once,
  including demand points (entity `DemandPoint`); the attachment list only
  provides the attachment lines. 
* **Declutter** (`MapBgfx2dScene`): coincident nodes (pump/valve inlet and
  outlet digitized at one coordinate) are spread with the shared
  `computeNodeDeclutterOffsets()` at 1 m, same as QRhi and the globe. Link
  endpoints and demand-point attachments follow their node's offset. Links
  are kept as polylines so icons and chevrons are placed along the whole link.
* **Icons** (`vs_icon_2d.sc`, `fs_icon_2d.sc`): node icons for non-junction
  nodes and link icons (pumps, valves) at the middle of the link length, from
  `mapIconAtlasImage()` (premultiplied, full CPU-built mip chain, trilinear).
  Constant pixel size (px or metres, continuous in the zoom). The fragment
  shader is the QRhi icon shader's fill-mask logic. When icons are shown the
  node dot of an entity with an icon gets alpha 0.
* **Flow chevrons** (`vs_flow_2d.sc`, `fs_flow_2d.sc`): zoom-independent.
  Every link gets chevrons at fixed world positions centred on the link
  (pumps/valves at 30 %, the icon owns 50 %), organised as a hierarchy:
  position j has level `ctz(|j|) + base_level`, the centre is always shown.
  The vertex shader shows the levels whose on-screen spacing is >= 100 px
  (`MapBgfx2dFlow::thresholdLevel()` from the continuous zoom) and fades the
  next finer level in over one octave, so chevrons never pop at imagery level
  boundaries. At most 17 per link (long links use a coarser base level);
  none on links shorter than 18 px. One quad per chevron; the fragment shader
  draws the two strokes as an antialiased distance field. The direction is the
  link's flow sign in its entity data (below), so a timeline step never
  touches geometry.
* **GPU colouring** (`map_bgfx_entity_style.h`, `shaders/entity_style.sh`):
  the network's vertex buffers carry no colours, only the render id of their
  entity (plus "link icon" on icons). Every vertex shader colours its
  primitive from per-entity data, shared by the 2D map and the globe:
  - two RGBA32F data textures, nodes and links, 2048 texels per row, texel =
    render id: x the colouring metric's raw value, y the heatmap value (nodes)
    or the flow direction -1/0/+1 (links), z flag bits (has value, has heat
    value, hidden, selected, error, stale, junction / pump-or-valve, has
    icon). Read with `texelFetch` in the vertex shaders;
  - a 256 x 3 RGBA8 palette texture: the node, link and heatmap ramps in the
    CPU's 256 buckets (`networkSymbologyRampColor()`), flip applied;
  - uniforms: per metric (minimum, 1 / range, base fraction, state), the
    junction / icon / flow toggles, the fixed colours (base colours, the
    unavailable colour, icon fills, selection cyan, error red, stale grey)
    and a per-draw mode (symbology, selection, diagnostic, heatmap; vertex
    colours for the globe's outlines).
  What a change costs: a time step uploads the changed data texture(s) (16
  bytes per entity, one `updateTexture2D`), a palette change 3 KB, a range,
  junction, icon or flow toggle nothing but uniforms; hiding, selection and
  diagnostics are flag changes in the data textures. No vertex buffer is
  ever rebuilt or re-uploaded for colours. The rules are the QRhi rules,
  evaluated in the shader: visual None gives the base colour
  (`MapNetworkRenderSymbology::*_base_color`; the editor's brown pumps and
  valves), a missing value or a non-finite range the unavailable colour,
  else the ramp bucket of `(value - minimum) / (maximum - minimum)` (0.5 for
  an empty range); attachments take their demand point's colour; junctions
  can be switched off and node dots give way to shown icons; icons use the
  default fill unless colourised (then the value colour, or the
  icon-unavailable fill); chevrons are black or white by link luminance.
  Sizes (link width, node size, icon size, chevron size; px or metres) are
  per-frame uniforms (`map_bgfx_2d_symbology.h`).
* **Symbology values** (`MapNetworkSymbologyResolver`,
  `map_network_render_symbology.h`): per time step the resolver writes the
  raw values densely by render id (`QVector<double>`, NaN = no value) and the
  flow directions (`QVector<qint8>`). The value sources are visitors
  (`network_symbology_values.h`); uuids map to render ids through a cache of
  the source's order (result lists keep their order across time steps), so
  a step costs one uuid comparison per entity and never builds a hash table.
  `HydraulicData::symbologyRanges()` folds the same visitors into minimum /
  maximum. Measured on 360 k nodes / 720 k pipes: resolve 678 ms -> 50 ms
  per step, the render side 308 ms -> 17 ms (colour streams vs data
  textures), a palette or range change 0.07 ms instead of a full resolve.
* **Heatmap** (`fs_heatmap_2d.sc`): one spot per node (incl. demand
  points, at the decluttered position) with a heatmap value. No extra
  geometry: the heatmap pass draws the node marker quads with
  `vs_network_2d.sc` in the heatmap mode, the radius as size uniform (the
  continuous heatmap ramp of the node's heatmap value; no spot without a
  value or without a finite range). Radius in
  px or metres (metres converted from the continuous zoom, so it grows smoothly),
  opacity and solid centre are uniforms; same radial profile and premultiplied
  "over" compositing as QRhi. Culled with the radius as margin; skipped when no
  heatmap is selected or the opacity is 0.
* Layer order across all chunks: heatmap, attachments, links, chevrons, nodes,
  icons (the heatmap sits directly above the imagery, as in QRhi).
* Buffers are uploaded only on geometry revisions; camera movement changes
  only per-chunk uniforms. The debug overlay shows uploads and
  drawn/total chunks.
* 4x MSAA (`BGFX_RESET_MSAA_X4`, clamped by bgfx to renderer support) for
  antialiased line and marker edges.
* Shaders are compiled per renderer family and embedded in Qt resources
  (see "Renderer selection").

* **Picking** (`map_bgfx_2d_picking.h`): `hitTest()` for click
  selection and the hover cursor, with the flat QRhi priorities and radii
  (icons max(12, size/2 + 6) px, nodes incl. demand points max(10, r + 5) px,
  links max(8, w/2 + 5) px), but against what bgfx draws: decluttered
  positions, link icons at the middle of the link length, sizes from the
  continuous zoom and the model's current camera. Hidden icons/junctions are
  not pickable. CPU pass in screen space on click/mouse move only.
* **Highlights**: selection (0,190,255) and simulation errors (red,
  stale results grey) as in QRhi: wider copies of the entity drawn on top of
  links resp. nodes (link width + 2 px, + 6 px when the selected entity has an
  error; node radius + 2 / + 5 px). Small separate meshes built from
  `MapBgfx2dScene::filtered()` (exact drawn positions), rebuilt only when the
  selection, the error set, the prepared geometry or the junction visibility
  changes.
* **Node declutter toggle**: `MapBgfx2dSceneVariants` caches both the
  decluttered monitor and exact-position editor scenes per geometry revision.
  Each is prepared once; revisiting an unchanged variant reuses its
  `MapBgfx2dScene::preparationId()`. The GPU mesh, highlights and picking
  follow the selected variant and can still require a GPU mesh upload on
  switching, but do not recompute derived CPU geometry. On a true geometry
  revision, the unused presentation is evicted and prepared lazily again.

## On-demand rendering

The frame loop (desktop: a timer slightly faster than the display; WASM:
`requestAnimationFrame`) keeps ticking for the navigation clock, but a tick
presents a frame only when something visible changed: the camera
(`mapCameraUpdate()` against the last presented frame), the size or device
ratio, a call of any surface setter or `requestRedraw()` (the editor overlay's
content), an arriving tile, an expose; or while something animates by itself
(`MapBgfxHud::animating()`: download busy bars, a tooltip inside its delay;
tile uploads over the per-frame budget; deferred labels). An idle map costs
an early return per refresh instead of a full frame - in the browser it no
longer keeps a core busy. Plain pointer moves over the map present nothing;
only HUD hover feedback does. Unchanged download numbers (the monitor polls
every 400 ms) present nothing either.

Around the surface: `MapWidget` is a pure navigation controller that draws
nothing (no repaint and no backing-store scroll per pan step), and
the footer's map readouts are coalesced to at most one update per 100 ms
(each label change relayouts the status bar). WASM builds drop the function
name section by default (`AOWIS_WASM_FUNCTION_NAMES=ON` to keep it).

## Native HUD

QWidgets cannot sit on top of bgfx's native window, so the HUD is drawn by
bgfx itself, last in the map view (always on top), as one indexed draw per
frame:

* `map_bgfx_hud_canvas.h`: logical-pixel draw list (rects, rounded rects,
  circles, lines, atlas sprites) with premultiplied ABGR vertex colours and
  32-bit indices. Solid shapes sample one white atlas texel, so everything is
  one program, one texture, one draw.
* `map_bgfx_hud_text_cache.*`: whole text runs and images rasterized by Qt
  (QPainter, the widget font, the screen's logical DPI and device pixel
  ratio) into one 1024 x 1024 premultiplied RGBA8 atlas. Shelf packing; only
  the changed rectangle is uploaded. When the atlas is full it is cleared and
  the HUD frame is rebuilt once against the refilled atlas.
* `map_bgfx_hud.*` (pure Qt Gui, unit-tested in `tests/bgfx_hud_test.cpp`):
  state, layout and input. Same content, positions and sizes as the QWidget
  HUD: centred crosshair, GPS dot (red, black outline) at the continuous
  camera's projection, scale bar bottom left (sampled through the viewport
  centre from the continuous camera, label in the user's distance unit),
  download panels top centre (map tiles, terrain: busy bar, "n active ·
  m queued", Cancel stays disabled until fresh numbers arrive), one panel
  top left with the fullscreen button (always: monitor and editor, desktop
  and WASM) and the 2D/3D switch (3D disabled until the bgfx globe exists),
  the pan joystick top right with the zoom buttons (zoom in above zoom out,
  the former sidebar icons) under it. Pan and zoom are on the HUD only: the
  sidebar (`MapNavigationWidget`) no longer has buttons for them. Hit boxes come from the last build: clickable
  is exactly what was drawn; boxes of a popup closed since then are ignored.
* Pan joystick (`MapBgfxHud::joystick()`): a ring with four arrows and a
  knob. A press on it sends `JoystickDrag` (Begin, press position) and is
  not held by the HUD: `MapWidget::beginJoystickPan()` captures the pointer
  with the globe orbit's machinery (cursor hidden; Wayland pointer lock with
  relative motion, on the top-level and the embedded bgfx window; elsewhere
  pinned by warping back to the press point; WASM: plain moves) and moves a
  virtual knob up to `JoystickTravelPx` (120 px of hand movement for the
  HUD knob's 18 px). The deflection pans every navigation frame like the
  drag pan (globe: ground under the crosshair) at up to
  `JoystickMaximumSpeedPixelsPerSecond`, quadratic near the centre; the
  release (seen by the orbit's application event filter on desktop, by the
  map handlers on WASM) ends it, the knob springs back
  (`MapWidget::joystickChanged()` -> `setHudJoystick()`) and the pan glides
  out like a keyboard pan. The arrows glow towards the view's motion: the
  knob's lean, and the held keyboard pan keys
  (`MapWidget::keyboardPanChanged()` -> `setHudKeyboardPan()`). The wheel
  over the joystick and the zoom buttons zooms the map about the crosshair
  (`MapBgfxHud::wheelZoomsAtCrosshair()`; the surface re-anchors the event),
  also during a drag (`MapWidget::handleWheelEvent()` anchors at the
  crosshair while the joystick holds the pointer). The joystick's and the
  zoom buttons' tooltips list the configured keys under their description
  (`MapBgfxHud::setNavigationKeys()`, kept current by the surface from the
  shortcut registry).
* Keyboard focus: any press on the map or a HUD element brings the keys
  back to the map (`MapBgfxSurface::focusNavigationInput(Qt::MouseFocusReason)`:
  the map's window activated, then QWidget focus on the surface, or the
  editor's canvas, plus the native window's activation where the platform
  allows it; Wayland never activates a subsurface by itself; WASM: the
  containers focus the map / canvas on every press, also one the HUD takes).
* Map legend, bottom right, like `EntityMapLegendHud`: node /
  link / heatmap metric combos (list opens on press, below or above the
  combo and tall enough for every entry; only when neither side has room it
  scrolls, with a scroll bar: drag the thumb, click the track to page, wheel),
  colour ramps (gradient through the palette stops, 7/5/3 value labels that
  never collide, "uniform", "No finite values"), the hover value swatch, the
  palette menu (Sequential / Diverging, preview + Flip per palette, Reset)
  and QToolTip-style tooltips after 0.7 s. A press outside an open popup
  closes it and is swallowed; Escape and focus loss close it too.
* `map_bgfx_legend_content.*` (application side): the legend's entries,
  ranges in the displayed unit, palette stops and defaults, computed exactly
  like `EntityMapLegendHud` from the symbology settings and the ranges
  `applySymbology()` already has. While the bgfx map is active the legend
  dock stays hidden; a HUD choice goes through the same container paths as
  the left menu and the dock (`setNodePalette()`, `applySymbology()`).
* `map_bgfx_hud_gpu.*`, `vs_hud.sc`/`fs_hud.sc`: point-sampled atlas
  (texels on the physical pixel grid), premultiplied blend, transient buffers.

Input: every map mouse event goes to the HUD first
(`MapMonitorContainer::routeBgfxHudMouseEvent()`); a press on a HUD panel is
consumed together with its moves and release and never reaches navigation,
picking or hover (the joystick's press only: its drag is navigation). Wheel events over a HUD panel scroll an open list or are
swallowed (`handleHudWheelEvent()`), never zooming the map underneath.
Button and legend actions are delivered queued as `MapBgfxHud::Command`
(`setHudActionHandler()`), outside the native window's event dispatch
(a handler may change the layout around the map, e.g. fullscreen). Desktop cursors (pointing hand, cross
over ramps) are set on the embedded native window (`setMapCursor()`), the
only cursor visible there.

Fullscreen is in place (`MainWindow::setMapFullscreen()`): the main window
goes full screen (WASM: the browser page, where the browser allows it) and
hides everything but the current page's map (toolbar, status bar,
navigation bar, right docks, the page's left menu). The map stack is never
reparented, so bgfx keeps running: no shutdown, no re-upload, only a
resize. The former reparenting to a top-level window had to shut bgfx down
first (`suspendForNativeWindowChange()`, still available for planned native
window changes: Qt Wayland destroys the embedded window's wl_surface on
reparent without a `QPlatformSurfaceEvent`, and a Vulkan swapchain alive on
it ends the Wayland connection), restarting rebuilt every GPU resource, and
in the browser the full-screen Qt window was stacked above the bgfx canvas
(aowis-bgfx-map.js ranks windows by area; two full-screen windows tie):
an empty page. `QPlatformSurfaceEvent` handling remains as the fallback for
unplanned native-window recreation.

bgfx's statistics text overlay would cover the HUD; it is shown only with
`AOWIS_BGFX_DEBUG=1`.

**Not yet implemented:** terrain/globe.

## Map labels

Text on the map next to the network entities, like EPANET's *Map Options ->
Notation*: node IDs, link IDs and (monitor) the values of the metric the map
is coloured by, in the legend's unit (`MapLabelSettings`,
`map/render/map_label_settings.h`; switches under "Labels" in the left
menu's `MapNavigationWidget`, all off by default).

* `map_bgfx_labels.*` (pure Qt Gui, unit-tested in
  `tests/bgfx_labels_test.cpp`): greedy, decluttered screen-space placement.
  Candidates are the entities on screen at their drawn positions (links: the
  middle of their visible length; pipes shorter than 24 px get none). Every
  on-screen node symbol and device icon is an obstacle; labels are tried in
  priority order (tanks/reservoirs, pumps/valves, junctions, pipes, demand
  points; ties by render id, stable while panning) at up to four positions
  around their symbol, and kept only where they overlap neither a label nor
  a symbol and lie inside the viewport. A 48 px grid keeps each test local.
  Above 30000 on-screen nodes only tanks, reservoirs, pumps and valves are
  labelled.
* Text: `MapBgfxHudTextCache::haloText()` rasterizes each run once as one
  colour sprite, dark text with a white outline (`QPainterPath` stroke +
  fill), readable over imagery and lines; node labels near black, link
  labels dark blue. ID and value are two centred lines. The labels' own
  2048 x 2048 atlas is created on first use; placement stops before one
  frame's labels would exceed 70 % of it, and at most 160 new runs are
  rasterized per frame (the rest keep their slot and appear in the next
  frames).
* `MapBgfxSurface::renderLabels()`: drawn after the network and the editor
  overlay, before the HUD, with the network's camera (the editor's
  full-network move included); rebuilt only when the camera, the viewport,
  the prepared scene, the atlas or the content (settings, symbology, value
  formats, hidden entities, renames) changed. Values: canonical metric values
  by render id in `MapNetworkRenderSymbology::node_values / link_values`
  (dense, NaN = none),
  converted with `mapBgfxLabelValueFormat()` (the legend ramp's affine unit
  conversion). Symbol boxes: the GPU network's dots and icons (monitor), the
  editor's bottom-anchored / centred icon rects (`setLabelSymbolRects()`).
* Renames change no geometry: `HydraulicData::setNodeId()` & co. rebuild the
  current render snapshot (same render ids), the retained scene takes the
  IDs over on the visual-revision update, and the containers re-send the
  snapshot on `signalNodeChanged` / `signalLinkChanged` /
  `signalDemandPointChanged`.

## Map editor rendering

The Map Editor uses a second `MapBgfxSurface`, and only bgfx. It draws the
imagery, the static network in the editor look (black pipes, brown device
links, black 10 px node dots: `applyRenderSurfaceEditorSymbology()`), every
editor overlay, map labels and the HUD (crosshair, GPS dot, scale bar, the
fullscreen button, 2D/3D switch, joystick). `MapCanvasWidget` is the input
owner (editor controller first, then MapWidget navigation) and paints
nothing. There is no fallback renderer: until the first frame, and when bgfx
cannot start (or is not in the build), a status label takes the map's place,
as in the monitor.

* `map_bgfx_editor_overlay.*` (pure Qt Gui, unit-tested in
  `tests/bgfx_editor_overlay_test.cpp`): port of the former QPainter editor's
  `paintRenderSurfaceOverlay()` into a HUD-kit draw list,
  rebuilt per frame from `MapEditorVisualState` with the continuous camera:
  tile-cache and rectangle selection frames, full-colour icons (nodes
  bottom-anchored at the rounded position, device links centred), pipe
  vertex dots, demand-point attachments (dashed, with handles), selection,
  connection target, simulation errors, pipe / device-link rubber bands, the
  floating marker and the move preview. Snapshot geometry is projected to
  Web Mercator once per geometry revision; icons are culled before they are
  requested.
* `map_bgfx_editor_icons.*`: the icon provider. Each (pixmap, width,
  highlight) is composited once at the device pixel ratio with the glows of
  `MapEntityPixmapRenderer::paint()` into the overlay's own 2048 x 2048
  atlas, then drawn as one quad.
* Cost rules (networks with many demand points): the editor's visual state
  (`MapCanvasWidget::visualState()`, O(demand points)) is rebuilt only when
  the editor's content changes, never per camera frame
  (`signalCameraChanged()` only refreshes the pointer's map coordinate) and
  never while the editor is hidden (synced map movement); pipe and device
  link endpoints come from the markers' uuid index
  (`MapCanvasMarkers::findMarker()`), not from a hash of all markers built
  per pipe or per demand point (that was quadratic: seconds per zoom level
  change); the overlay's icons are memoized per (entity, width, highlight)
  and atlas generation; zoom changes do no editor symbology work.
  Per frame the overlay culls demand-point attachments against the viewport,
  clips their dashes to it and draws handles only where the line is at least
  12 px long (or selected / edited); pipes with vertices are culled by their
  cached map bounds before projection. Per pointer move the hover hit tests
  reject markers and pipes outside a map-coordinate box around the pointer
  (`MapCanvasHitBox`) before projecting them or looking up pixmaps.
  Dashed lines are textured quads over a dash-pattern strip in the overlay
  atlas (one quad per 512 px instead of one per dash), attachment handles
  are one disc sprite each, circles and lines go straight into the draw
  list (no temporary outlines), and an unchanged frame (same camera, no
  editor change) reuses the last overlay canvas. `compile_linux.sh` builds
  RelWithDebInfo by default (`AOWIS_BUILD_TYPE`): without a build type
  CMake compiled without optimization.
* Move preview: dragged entities are hidden in the GPU network
  (`setHiddenEntityUuids()`: a flag in the entity data texture, no geometry
  upload) and drawn by the overlay; a full-network move shifts the network
  by the camera (`setNetworkScreenTranslation()`), the imagery stays.
* The overlay is drawn between the network and the HUD with a second
  `MapBgfxHudGpu`. HUD canvases use 32-bit indices; bgfx's transient buffers
  are raised to 16 MiB vertices / 8 MiB indices (`Init::limits`) for one icon
  quad per visible node.
* Input: desktop forwards the native window's events to the canvas
  (`sendEvent`, same geometry); keyboard focus stays on the canvas
  (`setKeyboardFocusWidget()`: the native window does not take focus), so
  the editor's window shortcuts keep working. WASM: the canvas is on top of
  the Qt stack and receives the pointer directly. Either way the container's
  event filter on the canvas gives the HUD every pointer event, wheel and
  Escape first (`routeBgfxHudEvent()`); a HUD button's pointing hand wins
  over the tool's cursor.
* Fullscreen: the HUD button and the map fullscreen shortcut
  (`MapMonitorFullscreen`, parented to each map stack, so only the visible
  map's fires) enter map fullscreen exactly like the monitor (in place,
  see above). The left menu is then hidden, so its buttons' shortcuts
  cannot fire; the canvas' key presses try them first
  (`MapEditorMenuWidget::triggerButtonShortcut()`), as the shortcut map would
  have.

The Maps workspace has one shared bgfx surface and one process-wide bgfx
renderer. Changing **main-navigation tabs** only stops the map's frame loop;
it retains the GPU device, shaders, textures, imagery atlases and terrain
caches. Returning to Maps resumes the same device without `bgfx::init()` or
asset uploads. In the browser the separate map canvas is hidden while Maps
is not selected, so it cannot cover the active Qt page. A real native-window
destruction/reparent (rather than a tab hide) still shuts bgfx down before
the native surface is lost and reinitializes it when the replacement is exposed.
The process-wide `BgfxDeviceManager` now also supports the native Projects
revision graph and secondary native map targets. A graph opened first may own
the primary device; Maps opened first supplies the primary target and the
Projects graph can attach as a secondary. `AOWIS_BGFX_THREE_VIEW_PROBE=1`
opens two independent, read-only secondary geographic map windows on Linux.
The exact view-ID, swapchain, frame batching, cleanup and fallback rules are
in [Rendering architecture](../../../docs/rendering-architecture.md).

## 2D imagery

* `map_bgfx_2d_imagery_layout.h` (pure C++, unit-tested): the imagery level is
  `round(continuous zoom)`, but quads are positioned with the *continuous*
  scale, so imagery stretches smoothly between levels and never steps. The
  test asserts continuity of a world point's screen position across x.5.
* A missing tile is covered by the nearest resident ancestor's sub-rectangle
  and by resident children on top; never a hole while tiles stream in.
* `map_bgfx_2d_imagery_gpu.*` with `map_bgfx_2d_imagery_atlas.*`: tiles are
  cells of a few atlas pages (edge-replicating 1-texel gutters, so bilinear
  sampling equals clamp-to-edge), LRU budget of 384 tiles or the tiles a
  frame draws plus 128 (the coarser fallbacks of not yet uploaded tiles;
  the globe draws 400+). Atlas uploads are budgeted nearest-first: at most
  **2 in 2D / 3 on the globe while the camera moves**, rising to **8 / 12
  when stationary**, so newly streaming tiles do not monopolize navigation
  frames. Set `AOWIS_MAP_2D_TILE_UPLOAD_BUDGET` or
  `AOWIS_MAP_3D_TILE_UPLOAD_BUDGET` to override the corresponding limit
  (integer 1–32) when comparing responsiveness with tile-fill throughput.
  Quad runs sharing the same atlas page typically use one draw call.
  The globe shares the atlas.
* Tile decode uses a background thread pool (up to four desktop workers,
  two on WebAssembly). `AOWIS_MAP_TILE_DECODE_THREADS` (integer 1–16)
  overrides the pool size for comparisons. With
  `AOWIS_MAP_RENDER_DIAGNOSTICS=1`, the periodic counters include successful
  2D/globe tile uploads and completion/cache-insertion work on the GUI thread;
  `tools/report_2d_performance.py` prints their interval totals.
* Requests go to the shared `MapTileRepository` only when the visible tile set
  changes or a failed tile becomes retryable. Provider switches and deleted
  tiles clear the texture cache. A provider switch also supersedes the old
  provider's requests (`MapTileRepository::supersedeTileSources()`: queued
  ones dropped, those in flight no longer hold request slots), so the new
  provider's tiles go out at once; the globe refreshes its window tiles'
  imagery keys (`MapGlobeSurfacePreparation::refreshImageryKeys()`) and
  starts a new request batch per window.
* The "slide opacity" fades the imagery towards the background colour in the
  imagery shader.
A single scene-local origin is sufficient for regional water networks, but is
not a final worldwide-precision architecture. Spatially grouped buffers with
per-batch origins and culling are still mandatory for larger extents.

Do not reintroduce integer-zoom geometry caching on this new GPU path. Imagery
LOD will remain independent from retained network geometry.

## Globe and terrain rendering

The shared `MapBgfxGlobeTerrainGpu` (`AOWIS::RenderCore`) owns the base
surface/wireframe shader programs, terrain frame uniforms, missing-imagery
and polar-cap GPU textures, and cap/window-tile/wireframe draw submission.
`MapBgfxGlobeSurface` provides camera/lighting vectors, per-tile atlas UVs,
GPU buffer handles and optionally heatmap-compositing resources. Sky drawing,
heatmap atlas baking, tile/DEM scheduling, and the underlying terrain mesh
preparation remain GUI responsibilities in this extraction slice. Shared
terrain buffer and index pools manage the physical GPU geometry; sparse DEM
updates remain unchanged. The new Render module is tested with recorded bgfx
submissions and a production archive link regression.

Globe mode draws on bgfx; the QRhi globe it replaces was removed with the
rest of QRhi. "QRhi" below names the behaviour that was ported. The HUD
offers 3D in the Map Monitor and the Map Editor once the globe surface
initialized. In the editor, network editing stays 2D; its cache sections
(Tile Cache, Terrain Cache) select tiles on the globe too: a right drag picks
two ground corners (terrain-aware), the tile range between them is drawn on
the drawn surface by the globe overlay pass (`setGlobeOverlayBuilder()`, a
`GlobeProjector` per frame: WGS84 -> logical px on the drawn relief, hidden
behind the camera and beyond the horizon), map tiles green, terrain tiles
brown (`MapBgfxEditorOverlay::buildGlobe()`). In 3D Editor mode the same
overlay projects its full-colour node and device-link pixmaps onto the globe,
instead of the monitor's generic GPU star symbols. Only the editor's 3D
presentation changes; its 2D icon path and the retained globe network geometry
remain unchanged.

### 3D Editor icon visibility and large networks

The Editor overlay keeps a separate **CPU-only, retained ECEF kd-tree** of the
node and device-link icon anchors (`map_bgfx_globe_icon_index.h`). It is rebuilt
only when the network snapshot's geometry revision, identity, or node/link count
changes; pan, zoom, rotation, terrain updates, and workspace changes do **not**
rebuild it or the GPU geometry. Each 3D frame supplies four view-side clipping
planes, with a pixel allowance for the Editor's largest icon and a deliberately
conservative displacement allowance for exaggerated relief. Whole spatial
branches are rejected before `GlobeProjector` performs any terrain-height
lookup. The projector still checks horizon, drawn DEM height and final pixel
bounds; retained icon depth ordering and the actual full-colour bitmaps are
unchanged. If a network is fully visible or concentrated inside the relief
allowance, the candidate set can still be large; this is not an icon density
limit or GPU instancing mechanism.

`aowis-map-bgfx-globe-icon-index` runs a deterministic 100,000-anchor spatial
query regression/benchmark without Qt or GPU. It reports index-build time and
200 query times. These measurements **do not** represent complete frame time,
GPU load, or WASM performance; production acceptance additionally requires
measuring the 3D Editor with realistic network density and visible DEM tiles.

Set `AOWIS_MAP_RENDER_DIAGNOSTICS=1` to read cumulative counters
`globe_editor_icon_index_build`, `globe_editor_icon_anchors`,
`globe_editor_icon_candidates`, and `globe_editor_icon_visible`. Compare
**deltas** between successive reports while moving the camera, not the raw
cumulative values. The ratio of candidate delta to anchor delta indicates
how many terrain projections spatial culling avoids. Dense networks where
most anchors remain plausible candidates should be benchmarked separately.

### Large-network performance validation

For reproducible *CPU-only synthetic workloads*, first build the test target
with `./compile_linux_tests.sh --both`, then run the Release executable:

```bash
./build-linux-tests-release/aowis-map-bgfx-globe-icon-benchmark \
    > /tmp/aowis-globe-icon-synthetic.jsonl
```

This uses the actual retained ECEF kd-tree on deterministic 10k, 50k and 100k
anchor networks and three views each: **regional** (100 km extent, fixed
camera footprint), **urban_dense** (4.5 km extent, the same camera footprint),
and **fully_visible** (all icons projected). It pans, rotates and changes zoom
over 90 queries per scenario; checks that the culled icon result matches a
complete projection and draw-order pass; records index construction time,
capacity-based memory estimate, median and p95 CPU cost, mean candidates and
leaf points examined, and unintended index rebuilds. `--smoke` checks correctness
quickly as part of `aowis-tests`. Benchmark output is one JSON object per
scenario suitable for comparisons across Linux/Windows/WASM builds.

These are **simulated projection calculations and CPU depth sorting** (no real
DEM sampling, sprite rasterization, GPU, Qt renderer or actual network importer).
A kd-tree can be slower than a full scan when most anchors are visible; never
use synthetic speedups as proof of full-frame throughput or WASM FPS.

For **actual GUI CPU costs**, load 10k, 50k and 100k demand-point networks in
Distribution Editor on the target machine, enable 3D, allow terrain and icon
assets to load, and capture diagnostic snapshots while panning, rotating and
zooming at a fixed viewport size. Run each network size and graphics backend
as a **separate process/log**:

```bash
AOWIS_MAP_RENDER_DIAGNOSTICS=1 ./build-linux/aowis-server-gui \
    2>&1 | tee /tmp/aowis-editor-3d-10k.log
# Exercise the Editor 3D camera for at least 15-30 seconds, then close GUI.
python3 tools/report_globe_editor_performance.py \
    /tmp/aowis-editor-3d-10k.log --min-icon-frames 30 --min-anchors 10000 --json
```

The runtime counters `globe_editor_icon_frame`,
`globe_editor_icon_index_query`, `globe_editor_icon_projection`, and
`globe_editor_icon_draw` time the **real** CPU phases of 3D icon display. The
report uses **successive cumulative-counter differences**, excludes idle
snapshots, and weighs intervals by their actual icon-overlay invocation counts.
It also reports mean anchors, frustum candidates, visible icons, mean CPU time
per `frames` diagnostic (bgfx submit work, **not GPU completion or FPS**), and
index builds during each window. If the icon atlas gets evicted, an icon pass
may repeat within one presented frame. Pixel-size, view angle, terrain cache
warmth and browser threading should be logged alongside results for meaningful
comparisons. Reported median/highest **interval means** are not per-frame p95.

For the **real** FPS and memory acceptance measurements, additionally record
Chromium's frame metrics/performance profile in WASM and the OS process RSS/GPU
metrics on Linux and Windows; the opt-in diagnostic JSON cannot report GPU time,
VRAM usage, browser frame scheduling or complete process memory. Keep separate
cold-start (index build/texture loading) and warmed-navigation measurements.
Performance is judged against the previous commit on the **same machine**, not
against absolute CPU time limits embedded in CTest. The benchmark does not introduce
zoom-bucket invalidation or a network geometry rebuild on camera movement.

Terrain surface (`map_bgfx_globe_surface.*`):

* CPU work is the backend-neutral `MapGlobeSurfacePreparation` the QRhi globe
  uses: quadtree leaves with horizon and view-cone culling, ellipsoid and polar
  cap geometry relative to `MapGlobeCamera`'s sticky render origin, DEM
  requests, asynchronous terrain meshes, wireframe edges.
* Leaf refinement weighs a node's projected bounding-sphere size by its
  foreshortening (`mapGlobeQuadtreeNodeForeshortening()`): the square root of
  the cosine between the node's normal and the direction to the eye, at least
  0.125 (three levels), not applied within two radii of the eye and blended in
  by four. A sphere looks the same from every side, so before this a node
  near the horizon was refined as if seen head-on: cells several pixels wide
  and a fraction of a pixel tall (moiré in the wireframe, most of a tilted
  view's triangles). Coarser leaves draw coarser DEM tiles, so imagery and
  terrain follow together. CPU harness (1516x958, Alps; triangles at
  reduction 0 / 1): 2 km at 20 deg 1.05 M -> 0.64 M / 0.32 M -> 0.20 M,
  10 km at 10 deg 1.30 M -> 0.72 M / 0.37 M -> 0.24 M, 201 km at 34 deg
  0.71 M -> 0.44 M / 0.36 M -> 0.18 M; leaves -15..40 %; top-down views
  unchanged.
* Terrain level of detail is the DEM pyramid, never thinning inside a DEM
  tile (`map_globe_terrain_levels.h`). The terrain service's dataset catalog
  (`GET /terrain/v1/datasets`, `MapTerrainRepository::activeDataset()`) gives
  the dataset and its zoom range; nothing is requested before it arrives, and
  no provider or zoom is hardcoded. A leaf at imagery zoom z draws the DEM tile
  of zoom `clamp(z - reduction, max(8, minimum_zoom), maximum_zoom)` at its full
  resolution, `mapGlobeTerrainCellCount()` = 64 >> (z - DEM zoom) cells, so
  every vertex lies on a DEM sample. `reduction` is the "terrain detail
  reduction" setting (0..3, default 1): 0 draws every sample at the imagery's
  level of detail (about 4 px per cell), each step halves it. Past the
  dataset's `maximum_zoom` (its native detail) the DEM tile is subdivided, so
  the drawn terrain never claims finer detail than the source has. Mesh
  density depends on nothing camera-related, so a camera move rebuilds only
  when the leaves change; a catalog or setting change rebuilds the window
  (`MapGlobeSurfacePreparation::refreshTerrainLevels()`). Neighbours from the
  same DEM zoom share their edge vertices exactly; the edge stitching of the
  former in-tile LOD is gone.
* Every surface tile (terrain mesh, stand-in, flat) is its (cells + 1)^2 grid
  followed by a skirt: the four edges again, lowered by the tile's DEM
  elevation range plus one cell length (at least 20 m), times the vertical
  exaggeration (`mapGlobeAppendTileSkirt()` / `mapGlobeTileSkirtDepthM()` in
  `map_terrain_mesh_scheduler.h`). Neighbours disagree about a shared edge
  where their DEM zooms or stand-in state differ; the skirt
  hanging from the higher edge closes that crack and lies hidden below the
  neighbour elsewhere. It keeps the edge's texture coordinates, draws in the
  tile's one indexed draw, and is left out of ray picking, draping heights,
  terrain ray bounds and the wireframe (grid indices only).
* Sky (`vs_globe_sky.sc` / `fs_globe_sky.sc`, `MapBgfxGlobeSurface::drawSky()`):
  before the surface, one screen triangle whose corners carry their view
  rays adds the atmosphere's light onto the space colour (additive, no
  depth). Per pixel the ray is clipped to a spherical atmosphere about the
  Earth's centre (sea level: the radius under the eye; top 100 km) and to
  the ground; along it the optical depth of an exponential Rayleigh layer
  (8 km, per-channel coefficients: blue) and a Mie haze layer (1.2 km, grey:
  the white horizon) is integrated with 24 samples, in km, heights computed
  without cancelling Earth radii. The light of an evenly lit atmosphere,
  1 - exp(-beta * depth), is mapped so long paths reach the pale horizon
  colour and the short vertical path a deep blue. Hence, with no special
  case: a blue sky with a hazy horizon near the ground, darker with
  altitude, black above the atmosphere except for the thin bright band along
  the limb; the surface covers everything below the horizon. Drawn with the
  map tiles (not in the wireframe-only view); optional like the heatmap.
* Window vertices are a persistent pool (`MapGlobeSurfacePreparation`):
  a tile keeps its vertex range while it stays in the window, a dropped
  tile's range is reused by the next new tile of that size, the pool grows
  by half when full. A leaf change writes and uploads only the new tiles
  (`takeWindowVertexUploads()`); the whole pool goes up only after it was
  rebuilt (render-origin rebase, terrain levels change, free ranges
  outweighing used ones) or grown, since bgfx drops a dynamic buffer's
  content when it grows. Kept tiles are neither copied nor rescanned for
  their ray bounds. Before, every leaf change, i.e. nearly every frame of a
  camera move, copied and re-uploaded the whole window (4-5 MB per frame).
* A new tile gets its relief in the same frame: its own DEM's mesh when
  cached, else a stand-in resampled from the nearest cached coarser DEM
  (replaced on the mesh thread when its own DEM arrives); flat only before
  any DEM covering it is cached, or beyond an 8 ms meshing budget per
  rebuild (entering the globe, a render-origin rebase: those tiles start
  flat and get their mesh from the mesh thread instead of freezing a frame;
  flat terrain tiles come from the mesh builder too, the per-vertex
  geodetic conversion took ~45 ms for a full-detail window). New tiles
  waited for the mesh thread before
  and were drawn on the ellipsoid meanwhile, flickering between the relief
  around them during camera moves. Affordable because the mesh builder
  evaluates the Web Mercator inverse and the trigonometry per row and
  column (closed-form WGS84 per vertex): 64 cells 0.62 -> 0.09 ms.
  Frame harness (synthetic alps, 1920x1080, DEM 3 frames late, 8 per frame;
  zoom 60 -> 3 km, pan, orbit, zoom out; per phase): preparation CPU
  6-12 ms -> ~1 ms per frame, uploads 1.2-2.0 GB -> 17-44 MB, frames with
  flat relief tiles 150-180 -> 0 (only while the first coarse DEM loads).
* Hillshading (HUD toggle "hillshade", top left after "map", on by
  default, session state like the other toggles). Every surface vertex
  carries the terrain normal (ECEF) packed into four unsigned normalized
  bytes (`MapGlobePackedNormal`, vertex 20 -> 24 bytes). The mesh builder
  derives it from the DEM by central differences over one DEM cell, with a
  one-cell ring of samples around the grid taken from the tile's own DEM or,
  at DEM tile borders, from the four neighbour DEM tiles the request carries
  (`MapTerrainMeshRequest::terrain_neighbors`), so normals agree across
  tile borders (no shading seam). A neighbour not cached yet is reported in
  `missing_neighbor_mask` (edge normals one-sided meanwhile); its arrival
  rebuilds the mesh (`notifyTerrainTileAvailable()`). Flat tiles and caps
  carry the ellipsoid normal. Shaders (`globe_hillshade.sh`, used by
  `vs/fs_globe_surface` and `vs/fs_globe_surface_heatmap`): the vertex
  shader turns the light, given in east/north/up (`u_globe_light`:
  azimuth 315 deg, altitude 45 deg, the cartographic convention), into
  ECEF from the point's own up direction (position + `u_globe_origin`); the
  fragment shader scales the faded imagery by
  `mix(1, lambert / sin(altitude), strength)` (`u_globe_shading`: strength
  0.8, 0 when off): flat ground keeps its brightness, slopes facing the
  light brighten (up to 1.33x), the others darken; the heatmap colours stay
  unshaded on top. Verified: rendered with the compiled ESSL in WebGL 2
  (flat 100 -> 100, facing the light 133 (formula 133.1), facing away 20
  (20.0), off 100); normal tests (slope direction and size, no seam across
  a DEM tile border with the neighbour carried, missing-neighbour mask).
  The dx11 binaries of the four changed shaders need regenerating under Wine
  (`-DAOWIS_BGFX_SHADER_TOOLS=ON`); until then Direct3D 11 runs the old
  surface shaders (no hillshading; they ignore the extra vertex attribute
  and uniforms).
* GPU: window and cap vertices in dynamic buffers (window: the pool, patched
  per new tile and finished mesh; caps: full upload on a rebuild), one
  indexed draw per tile, depth test and write LessEqual. Window tiles have no index buffer
  of their own: the topology depends on the cell count only, so one static
  16-bit index buffer per cell count (topology from Render's
  `mapGlobeTileIndices()`, GPU cache in `MapBgfxGlobeTileIndexPool`) serves every tile, drawn with the tile's first vertex as start
  vertex (bgfx applies it as base vertex on GL/WebGL, D3D11, Vulkan and
  Metal). A window rebuild writes and uploads no indices (at 64 cells that
  was more than half of a tile's upload); picking and the wireframe read the
  same shared lists. The caps keep their own small index buffer. The camera's canonical OpenGL clip
  space gets the [0, 1] depth correction where bgfx reports no homogeneous
  depth (Direct3D, Vulkan, Metal). `QMatrix4x4::constData()` is column-major,
  as bgfx's `mul(matrix, vector)` expects.
* Imagery from the shared 2D atlas: a tile samples its own cell, or the
  covering part of its nearest resident ancestor (`map_bgfx_globe_imagery.h`,
  texture coordinates mapped into the ancestor's cell, no CPU compositing),
  or a solid colour (nothing loaded; polar caps). At most 12 uploads per
  frame (at 8 the backlog while zooming or turning showed as blank tiles);
  requests go to the tile repository in one batch per window.
* View-cone culling covers the frustum's corners: the cone's half-angle is
  the vertical half FOV widened by the aspect ratio, a node is kept when its
  direction is off-axis by at most that plus its angular radius asin(r/d)
  (also with its centre behind the eye), plus 0.1 rad slack (the leaves
  are selected every frame; the slack only prefetches). With the vertical FOV only, wide viewports
  lost terrain sections at their left and right edges.
* DEM still loading: culling bounds a quadtree node by its own DEM tile's
  elevation range, else by its nearest cached ancestor's, else by the whole
  possible range (ellipsoid to 9 km). The selection runs every frame:
  each DEM tile is looked up once per selection (by position; a repository
  lookup builds a string key), and the bounding spheres use the closed-form
  geodetic conversion. A node bounded on the ellipsoid under
  high terrain used to leave the view cone close to the camera, was never
  refined, so its DEM was never requested (a permanent hole next to the eye).
  A leaf whose own DEM is not cached is drawn from the nearest cached
  coarser DEM tile, resampled at its own DEM address
  (`mapTerrainTileFromAncestor()`, the mesh built on the mesh thread,
  `scheduleAncestorTerrainStandIns()`, again when a finer one arrives): same
  grid as the final mesh, which then replaces it in place; flat on the
  ellipsoid only where no DEM is cached at all.
* Globe shaders (`vs/fs_globe_surface`, `vs/fs_globe_wireframe`) declare
  their vec3 positions in `varying_globe.def.sc`; the 2D shaders keep
  `varying.def.sc` (and byte-identical binaries).

Terrain-aware navigation (`src/map/render/map_globe_terrain_navigation.*`,
backend-neutral, the port of MapRhiWidget's terrain code):

* Camera height: the orbit rig's vertical offset follows the DEM
  perpendicularly beneath the eye; while rotating (frozen pivot) an eye-only
  collision lift keeps `MinViewGlobeCameraGroundClearanceM`, released into the
  offset when the orbit ends. Monotonic exponential filter (0.22 s / 0.16 s),
  settled at a quarter pixel. Camera changes take a step at once (and request
  the DEM tile under the eye); the surface's frame loop continues the filter
  while `following()` (no extra timer).
* Rays against the drawn surface (`MapGlobeSurfacePreparation`'s retained
  meshes, ellipsoid elsewhere): the pointer coordinate with DEM altitude,
  `globeSurfaceRayHitAtScreen()`, and the crosshair's surface point for
  pans (`globeSurfacePointAtScreen()`). The Map Monitor installs MapWidget's
  resolvers for the bgfx surface; the 2D footer altitude reads cached DEM
  tiles too.
* Pans (`MapModel::panByPixelsGlobe()`): the ground under the crosshair
  moves with the pointer. The rays at the crosshair minus the pointer delta
  and at the crosshair meet the sphere through the drawn surface at the
  crosshair (about the Earth's centre); the camera rig turns rigidly, in
  double precision, from one point onto the other (`rotateGlobe()`). Speed
  thus follows the terrain at the crosshair (height, distance, viewing
  angle), not a fixed angle per pixel; nearby rays never land on unrelated
  relief, and a ray past the horizon takes its closest approach to the
  sphere. Keys turn the rig the same way along the great circle of the
  pressed direction. A rigid turn has no preferred axis, so pans cross the
  poles: the longitude swings by 180 degrees there and the yaw, taken from
  the turned screen-right direction, swings with it. North-up (compass) holds
  the yaw against the turn's small heading drift up to 80 degrees latitude;
  closer to a pole north spins around the crosshair with every step, so the
  pan releases the lock there and stays rigid.
* A vertical exaggeration change rebuilds the terrain meshes (vertex
  patches); a network opened in Globe mode requests its DEM tiles at once.

Network on the globe (`map_bgfx_globe_network.*`):

* Geometry is the backend-neutral `MapGlobeNetworkScene` of the QRhi globe
  (WGS84 placement relative to a retained network origin, adaptive geodesic
  subdivision, metre/pixel sizing, declutter, X-Ray classification against
  the DEM the globe shows). Its inputs are stored and applied on the next
  globe frame (`synchronize()`), so the 2D map never builds globe geometry.
  GPU streams are re-uploaded per category (links/nodes, highlights,
  chevrons, icons + models, X-Ray runs, junctions), as the QRhi backend's
  invalidations, but never for colours: the base streams (links, nodes,
  junctions, icons, chevrons, X-Ray runs) hold entity references in their
  colour fields (`mapNetworkSetEntityReference()`: render id, node/link,
  chevron orientation) and the shaders colour them from the shared entity
  data (GPU colouring above). Only the selection / diagnostic outlines keep
  vertex colours. Flow chevrons are built for every link in both
  orientations; the link's current direction shows one set, so a time step
  neither rebuilds nor uploads them.
* Shaders: the QRhi globe network shaders in bgfx form (`vs_globe_link`,
  `vs_globe_flow`, `fs_globe_link(_xray)`, `vs/fs_globe_node`,
  `vs/fs_globe_junction(_xray)`, `vs/fs_globe_icon`, `vs/fs_globe_model`),
  their own `varying_globe_network.def.sc` and shared uniforms/helpers in
  `globe_network.sh`. Pixel-space segment coordinates are interpolated
  screen-linearly (passed times w, divided in the fragment shader; WebGL 2
  has no `noperspective`), which also replaces the QRhi chevron shader's
  `gl_FragCoord` reconstruction.
* Junctions: sphere impostors writing the sphere's depth (`gl_FragDepth`,
  NDC to window depth per bgfx's homogeneous-depth cap). Expanded to six
  vertices each (no instancing); colour from the entity data in the vertex
  shader: error red / stale grey, else selection cyan, else the symbology;
  hidden junctions collapse. Selected icons take the selection colour.
* Draw order and states of `MapRhiGlobeNetworkBackend::draw()`: Solid draws
  the network once without depth test first; X-Ray draws the buried runs
  (dashed) and junctions (dithered) through the terrain; Hide lets the
  terrain occlude them. Premultiplied blending; tank/reservoir models with
  back-face culling. The chevrons keep their 8 px pull towards the eye but
  have no extra depth bias (bgfx has no depth-bias state).
* Demand points have no elevation (not part of the EPANET model): they lie
  on the terrain exactly as it is drawn, the network lift above it
  (`MapGlobeNetworkScene::setSurfaceHeightResolver()`: the window tile
  covering the point, then a vertical ray against the two triangles of its
  grid cell, `MapGlobeSurfacePreparation::drawnSurfaceHeightAt()`; the bare
  ellipsoid where no relief is drawn yet). The former ray against every
  tile's mesh cost ~40 ms per frame for 3000 demand points while terrain
  was loading. Whenever the drawn terrain changes (window, applied meshes:
  `MapBgfxGlobeSurface::surfaceRevision()`) they are moved in place before
  the network is drawn in that same frame, without a network rebuild. Their
  attachment lines end on the junction / pipe at its own elevation
  (`NetworkRenderDemandPointAttachment::attachment_elevation_m`).
* The underground mode is set through `setUndergroundMode()` (default
  X-Ray) from the globe HUD's buttons (globe HUD). X-Ray
  classification runs 500 ms after the last camera change or DEM arrival,
  once no terrain mesh is being built, like the QRhi globe.

Globe heatmap (`map_bgfx_globe_surface.*`):

* Markers, radius and solid centre as `MapRhiWidget::syncGlobeHeatmapOverlay()`
  (node positions, ramp colours of the heatmap metric; a pixel radius is
  converted at the orbit distance when the heatmap data or style change).
  `MapGlobeHeatmapScene` gives each window tile its stamps in the tile's
  256 px space.
* GPU bake: atlas pages of 2048 x 2048 (64 cells, up to 4 pages, created as
  needed) rendered in four bake views reserved alongside the current map
  view. `bgfx::setViewOrder()` presents those bake passes before the map pass.
  View IDs are allocated process-wide, not permanently fixed to 0–4. Per cell a clear quad (no blending), then the stamp
  quads (`vs/fs_globe_heatmap_bake`, premultiplied source-over, clamped to
  the cell). The bake's Y follows `Caps::originBottomLeft`, so the cell is
  sampled with the tile's own v-down texture coordinates on every backend.
* Cells per tile and scene revision (LRU over pages, 24 bakes per frame);
  a tile without stamps has no cell. Tiles with a cell are drawn by
  `vs/fs_globe_surface_heatmap` (the imagery, then the heatmap with the
  symbology opacity: rgb * (1 - a * opacity) + heat * opacity).

Globe picking (`MapBgfxGlobeNetwork::hitTest()`, used by
`MapBgfxSurface::hitTest()` in Globe mode for clicks and hover): the rules of
`MapRhiWidget::globeHitTest()`. Tank/reservoir 3D models by screen ray
against their triangles, then junction spheres (projected radius), icons
(projected billboard rectangle with the icon shader's depth-dependent size),
generic nodes, then link segments, with the globe's larger hit radii.
Positions are projected with the camera's matrix for the network origin, the
one the network is drawn with; pending inputs are applied first. Picking
deliberately reaches entities behind or under the terrain (as the QRhi
globe): buried and hidden parts of the network stay selectable.

Globe HUD (`map_bgfx_hud.*`, `map_bgfx_globe_hud_controller.*`):
the QWidget globe HUD drawn by the native HUD, with the same behaviour.

* Layout: "wireframe" and "map" toggles next to the 2D/3D switch, the
  "Terrain mesh" progress next to the downloads (x / y, or "building...";
  only after 0.6 s of continuous mesh building, gone 0.4 s after it ended,
  the HUD animating meanwhile so an idle surface presents that frame),
  and bottom left the four camera sliders (network height, tilt, distance,
  vertical exaggeration; maximum, value and minimum labels in the user's
  units, the QWidget tooltips), the X-Ray / Hide / Solid buttons, the compass
  with its heading, and the globe scale (above the controls when the map is
  too narrow).
* `MapBgfxHud` stays pure: it reports slider and compass interactions as
  commands with a phase (Begin / Move / End) and a number (slider fraction,
  pointer delta, wheel notches), collected by `MapBgfxSurface` after every
  pointer event and delivered queued, in order, through the HUD action
  handler. Right press on a slider resets it.
* `MapBgfxGlobeHudController` (Map Monitor) applies them to `MapModel` as
  `map_monitor_hud_controls.cpp` does: 0.1 m / 1 deg / logarithmic 1000-step
  / 0.1x slider steps, tilt and distance drags inside a rotate interaction,
  240 ms InOutCubic resets, compass drag by horizontal pointer motion, click
  to north (280 ms), wheel 5 deg per notch with the rotate interaction ending
  350 ms after the last notch. Everything stops when the view mode changes.
  `fillHudState()` gives the HUD its per-frame state (yaw, slider positions
  and labels, the globe scale at the focus distance).
* Toggles and underground buttons go through the container's
  `handleBgfxHudAction()` to the surface's setters
  (`setTerrainWireframeVisible()`, `setMapTilesVisible()`,
  `setUndergroundMode()`).

## Renderer selection

bgfx selects the renderer (`RendererType::Count`: platform ranking with
fall-through on init failure). AOWIS does not reimplement that; it only:

* compiles in the renderers it serves (`AOWIS_BGFX_RENDERERS`,
  `cmake/aowis_bgfx.cmake`): Linux OpenGL + Vulkan, Windows Direct3D 11 +
  OpenGL, macOS Metal, WASM OpenGL ES 3 (WebGL 2);
* embeds the matching pre-compiled shader families
  (`:/aowis/map/bgfx/<glsl|essl|spirv|dx11|metal>/`, see "bgfx build");
* rejects a pick without embedded shaders and retries with the next supported
  renderer that has them (`map_bgfx_backend.h`);
* honours `AOWIS_BGFX_RENDERER=opengl|vulkan|d3d11|metal` for diagnosis
  (environment only, not an ini option).

A software rasterizer (llvmpipe / WARP) is not selected automatically yet.

## bgfx build

AOWIS does not use bgfx.cmake. `cmake/aowis_bgfx.cmake` builds bx, bimg and
bgfx as three upstream unity TUs with only `AOWIS_BGFX_RENDERERS` compiled in,
no video, no software texture decoders (so no ASTC encoder), single-threaded.
Linux needs no extra link libraries (bgfx dlopen()s EGL/GL/wayland-egl/Vulkan).
Measured on Linux (OpenGL + Vulkan): 3 TUs / ~55 s CPU instead of 76 TUs / ~262 s.

The submodules are shallow and sparse (`tools/bgfx_sparse_checkout.sh`):
~15 MB of bgfx instead of ~270 MB.

**Shaders are pre-compiled and committed** in `src/map/bgfx/shaders/bin/
<family>/` (`cmake/aowis_bgfx_shaders.cmake`), so neither desktop nor WASM
builds build a shader compiler (bgfx.cmake's shaderc was ~690 TUs incl.
Tint/Dawn). They are embedded as committed; nothing checks that they match
their sources, so after editing a `.sc` file, or after updating bgfx:

    ./compile_linux.sh --shader-tools

builds `aowis-bgfx-shaderc` (`cmake/aowis_bgfx_shaderc.cmake`: shaderc
without Tint/Dawn/DXC/spirv-reduce, ~280 TUs, once), recompiles glsl, essl,
spirv, metal and dx11 into the build tree (again whenever a shader input
changes) and copies them into `src/map/bgfx/shaders/bin`. Commit that
directory.

dx11 (DXBC, for the Windows build's Direct3D 11 renderer) is compiled by
Microsoft's `d3dcompiler_47.dll`, which bgfx ships in `tools/bin/windows`
(part of the `--shader-tools` sparse checkout). A Windows host loads it
directly (copied next to `aowis-bgfx-shaderc.exe`); Linux and macOS run it
through bgfx's d3d4linux helper under **Wine** (any `wine` on `PATH`, or
`-DAOWIS_BGFX_WINE=/path/to/wine`; it uses its own prefix in the build tree).
The build sets `WINEDLLOVERRIDES=d3dcompiler_47=n`: Wine otherwise loads its
own builtin compiler (vkd3d-shader) even for a DLL given by full path, which
fails on `bgfx_shader.sh` ("Function \"reversebits\" is not defined", error
codes E5xxx) and would not produce Microsoft's DXBC anyway.
Without Wine the dx11 family is skipped with a warning and the committed
binaries stay as they are; a Windows build without current dx11 binaries
warns and runs on OpenGL.

## Input

Desktop bgfx runs in an embedded native `QWindow`, which receives all input
over the map. Keys, wheel and mouse are forwarded to MapWidget (mouse via
`QCoreApplication::sendEvent`, so the container's click-selection filter still
runs). Orbit (Ctrl or middle button) keeps going at the screen edges as on
QRhi: X11, Windows and macOS warp the hidden cursor back to the orbit start;
Wayland locks the pointer and reads relative motion
(`MapWaylandPointerLock`). The lock is requested on the top-level window's
`wl_surface` and on the native map window's own (a `wl_subsurface`,
`MapWidget::setOrbitPointerLockWindow`), because compositors differ in which
of the two they honour while the pointer is over a subsurface (KWin and
older mutter only the main surface). The log names the surface the
compositor activated, once per run, or warns when it activated none.
A pan or zoom key is released by its physical key (native scan code, else
virtual key, else Qt key), remembered at the press: other keys pressed in
between change the release's modifiers or layout level (Ctrl, Alt, AltGr
turning E into the euro sign), which left the motion running. Frames are not rendered
while the native window is unexposed, so the vsync swap cannot block the GUI
thread on a hidden Wayland surface.

## Frame clock and smooth zoom (do not regress)

* **One clock.** Once bgfx is active, `MapBgfxSurface` is the only navigation
  clock: each presented frame calls `MapWidget::advanceNavigationFrame(dt)`
  *before* sampling `MapModel::cameraViewState()`. MapWidget's own 16 ms pan
  timer is not started (`setNavigationFrameDriver(true)`). Two independent
  timers plus the vsync-blocking `bgfx::frame()` on the GUI thread advanced the
  camera by 0, 1 or 2 steps per displayed frame (simulated: ~28 % frozen
  frames with only 0-3 ms dispatch jitter). That was the keyboard-zoom judder.
* **Paced dt.** `MapFramePacer` quantizes measured frame time to whole refresh
  periods with carry, so a vsync-locked stream gets exactly one period per frame
  and camera time never drifts from wall time by more than half a period.
* **Desktop** uses a PreciseTimer 1 ms shorter than the refresh period; the
  vsync swap then locks presentation. **WASM** uses
  `emscripten_request_animation_frame_loop` (browser vsync), never QTimer.
* **Zoom motion** (`map_zoom_animation.h`): wheel, buttons and keys only move a
  target; the camera follows it as an exactly integrated critically damped
  spring. Velocity is continuous on press, tap->hold, reversal and release;
  results are frame-rate independent. No integer zoom anywhere in navigation.
* The camera uses **logical** pixels (`logicalSize()`); the view rectangle uses
  physical pixels. Using physical pixels for the camera was one octave off at
  devicePixelRatio 2.

## Build/test

Native Linux: `./compile_linux.sh` (GUI) and `./compile_linux_tests.sh
--release` (tests). `./compile_linux.sh --shader-tools` additionally
regenerates the committed shaders (see "bgfx build").

WASM: `sudo ./compile_wasm.sh`. It embeds the committed essl shaders; the
Docker image no longer builds a shader compiler.

## Shared Maps workspace: distribution and drainage

The Maps workspace retains one `MapBgfxSurface` and one `MapModel` while
switching between Distribution Monitor, Distribution Editor, Drainage Monitor,
Drainage Editor and Map Management. The two drainage sidebars are deliberately
empty; Map Management retains map/terrain tile selection and cache tools, but
the map, 2D imagery, 3D terrain, camera, navigation and common HUD stay live.
`setDistributionNetworkVisible(false)` hides distribution network draws,
labels, network-editing overlays, 3D network entities, heatmap and picking;
Map Management's green/brown cache-selection overlay remains visible. It does not
clear the retained scene or discard GPU network geometry. On returning to a
distribution mode, the appropriate monitor/editor symbology and input handlers
are restored. A mode change must not call `bgfx::shutdown()` or `bgfx::init()`.

The same invariant applies when changing **main-navigation pages** (Maps to
Projects/Dashboard/Settings and back): normal hides must not increase either
`bgfx_device_started` or `bgfx_device_stopped`. Check with
`AOWIS_MAP_RENDER_DIAGNOSTICS=1` when testing on a machine with the GPU.
For main-page-only switching (zero map-mode switches are required), collect
combined application output to a log, then run
`python3 tools/check_map_workspace_diagnostics.py map-tabs.log --min-switches 0 --min-cache-switches 0`.
The check rejects any GPU shutdown or repeated initialization.

### Globe icon overlap order

Transparent icon quads are drawn back-to-front relative to the active globe
camera. Monitor retains the per-entity globe vertex buffer and updates only a
32-bit draw-index stream when camera eye or icon geometry changes. Editor
projects its full-colour entity icons onto the HUD canvas and depth-sorts those
sprites before building the canvas. Its spatial index returns candidates in
kd-tree order (without a redundant index sort); a source-index tie-breaker
retains the same painter order for equal-distance icons. A fully enclosed
frustum uses a linear candidate range without visiting every tree leaf.
Both paths retain their existing terrain visibility and rendering styles;
neither rebuilds network geometry when panning or orbiting.

### 3D Editor icon terrain-height reuse

The 3D Editor projects icons onto the drawn terrain. The expensive mesh-ray
and DEM fallback sample is reused across camera frames by
`map_bgfx_globe_height_cache.h`, keyed by the exact geographic coordinate.
Finite, available heights are cached directly. Unavailable terrain is never
cached as a valid height: repeated failed samples are deferred for at most
eight overlay frames while rendering uses its existing ellipsoid fallback.
An arriving drawn-terrain revision retries them immediately. Cache entries
are cleared when the **drawn surface revision** changes, vertical exaggeration
changes, the terrain repository changes, or the bgfx backend shuts down. A
262,144-entry FIFO capacity bound prevents unbounded growth due to transient
selection lattices and camera travel. The screen position, distance/depth order and
visibility are still recalculated for every presented camera frame.
Diagnostics report `globe_terrain_height_cache_hit`,
`globe_terrain_height_sample`, `globe_terrain_height_unavailable_reuse` and
`globe_terrain_height_eviction` so actual DEM samples, successful reuse,
avoided repeated failures and memory eviction can be distinguished; run `tools/report_globe_editor_performance.py` against
`AOWIS_MAP_RENDER_DIAGNOSTICS=1` output to measure actual icon-frame CPU time.
The cached values refer to the terrain **as drawn**, not a permanent DEM
snapshot or the model's terrain-data revision. Neither this cache nor the
spatial index is tied to zoom buckets or network geometry re-preparation.

The production globe network draw-order and bgfx stream-submission path now
lives in `AOWIS::RenderCore` (`MapBgfxGlobeGpu`). `MapBgfxGlobeNetwork` still
owns the GUI snapshot adapter, picking, vertex layouts and per-frame camera
uniform values. `MapBgfxGlobeBufferPool` owns GPU streams, while
`MapBgfxGlobeResourcePool` owns shaders, uniforms and GPU model textures;
Render prepares the smooth mip chain from GUI-provided tank/reservoir images.
The shared icon atlas remains owned separately, not by the resource pool.
