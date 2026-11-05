# Renderer library boundary and extraction readiness

**Current state — 5 November 2026.** The GUI contains a separately linked
**internal static renderer library**, `aowis-render-core`
(`AOWIS::RenderCore`). There is **no separate `AOWIS-RENDER` repository** and
no stable external render API. This page describes the current source and the
minimum contract an extraction must preserve; it is not an implementation
history or a commitment to expand renderer features.

## Where the boundary actually is

| Owns / builds | Current source | Responsibility |
| --- | --- | --- |
| Internal `AOWIS::RenderCore` | `CMakeLists.txt` (`add_library(aowis-render-core STATIC ...)`) | Shared GPU infrastructure and shaders |
| Device and generations | `src/map/bgfx/bgfx_device_manager.{h,cpp}` | Sole bgfx initialize/shutdown; primary owner and secondary retirement leases |
| Global views and frames | `src/map/bgfx/bgfx_frame_coordinator.{h,cpp}` | View-ID reservations; one bgfx frame submission for active work |
| Native target registry | `src/map/bgfx/bgfx_native_target_registry.{h,cpp}` | Primary backbuffer, secondary swapchain framebuffers, resize, release |
| Toolkit-neutral surface description | `src/map/bgfx/bgfx_native_surface.h` | Native window/display/type, physical pixel size and depth-safe swapchain descriptors |
| GPU imagery and HUD utilities | `src/map/bgfx/map_bgfx_2d_imagery_atlas.*`, `map_bgfx_2d_imagery_gpu.*`, `map_bgfx_hud_gpu.*`, `map_bgfx_hud_text_cache.*` | Reusable upload and drawing machinery; some types still use Qt Gui |
| Embedded shader programs | `cmake/aowis_bgfx_shaders.cmake`, `src/map/bgfx/shaders/` | Shader family selection, static-resource registration and lookup |
| Qt native-surface adapter (GUI) | `src/map/bgfx/bgfx_qt_native_surface.{h,cpp}` | Capture Qt/X11/Wayland window/display handles and pixel dimensions |
| Production Maps host (GUI) | `src/map/bgfx/map_bgfx_surface.{h,cpp}`, related scene/globe/editor files | QWidget lifecycle, tile repositories, camera, network/entity styling, terrain and application HUD |
| Projects graph (GUI) | `src/tabs/project_revision_graph.*`, `project_graph_bgfx_viewport.*`, `project_graph_bgfx_scene.*` | Graph model/layout, BVH, interaction/Inspector routing and GPU presentation |
| Three-map diagnostic (GUI) | `MapBgfxSurface` and `map_bgfx_three_view_camera.h` | Two optional read-only 2D native windows; not independent editor instances |

The target links **`bgfx` and `Qt6::Gui` publicly**. Current helper APIs and
source include paths live inside the GUI tree. Thus "internal linkable
library" is accurate; "standalone Qt-independent renderer" is not.

## Compile and resource contract

The application links `AOWIS::RenderCore` rather than compiling its GPU
infrastructure sources a second time. `aowis_bgfx_add_shaders(aowis-render-core)`
embeds the compiled backend shader families in the static target, with an
explicit resource-registration translation unit. Embedded resource paths
remain `:/aowis/map/bgfx/<family>/...`.

`aowis-render-core-link-tests` links **only the render-core target** (not GUI
application sources) and verifies both a device-manager symbol and shader
access **without creating a GPU**. Passing it proves current static-library
linkage and resource inclusion, not portability, a stable SDK or successful
presentation on a real window.

## Invariants any future extraction must preserve

1. **Exactly one process-wide device.** `BgfxDeviceManager` alone initializes,
   shuts down and changes device generations. Do not create a separate bgfx
   device for each window, tab, QWidget or QtQuick host.
2. **Generation-scoped secondary clients.** Register a retirement callback
   before allocating GPU resources or view/target reservations. Retire all
   secondary clients before the primary releases its resources. Failed
   retirement prevents device shutdown. Stale generations must not release
   new-generation handles.
3. **Single-threaded frame coordination.** All bgfx clients share the GUI/render
   thread. Reserve disjoint global view IDs and batch active work before one
   `bgfx::frame()`. A single client's `present()` is not the multi-client API.
4. **Explicit native surface boundary.** Qt owns native-window lifetime.
   `BgfxQtNativeSurface` captures handles; the renderer consumes only
   `BgfxNativeSurface`. Window identity changes require releasing old native
   framebuffers before their host surfaces disappear. A resize is not a new
   device initialization.
5. **Depth attachments on map swapchains.** Preserve primary `bgfx::Init`
   defaults, especially D24S8. Secondary `SwapChain` and primary `reset()`
   descriptors must explicitly request D24S8. Never "fix" terrain skirt
   stripes by deleting the LOD skirts; the observed regression was lost depth.
6. **No application workflow in the renderer core.** Keep network database
   clients, project/branch management, editing transactions, main window,
   QWidget/QWindow hosting, Qt input routing and Inspector logic in their
   application hosts. Render inputs may be specialized for AOWIS without
   importing GUI service ownership into the core.
7. **Map-mode sharing.** Five Maps modes share one production `MapModel`,
   `MapWidget`, `MapBgfxSurface`, camera and retained model geometry. Tab
   changes must not cause unnecessary device restarts or network reloads.
8. **Visible-surface correctness.** Native input lost on focus change must
   cancel held keyboard/mouse/orbit/joystick/editor gesture state. The Projects
   graph may fall back to QPainter on unsupported/headless platforms; the
   production map has no QPainter fallback.

## Tested behavior versus remaining qualification

- **Observed on Linux:** Projects can initialize Vulkan as primary or attach
  as a secondary; three independent 2D maps render and pan/zoom concurrently
  on Vulkan; OpenGL 4.3 was confirmed as the actual backend with Projects bgfx
  disabled. Secondary windows attached/detached cleanly in user logs.
- **Observed in CPU benchmark:** at 10k revisions, ~403 ms graph layout,
  ~91 ms one-time retained-index construction and ~3.5 ms median scene build
  for a viewport with 10 cards and 15 candidates. Not a GPU FPS measurement.
- **Observed after swapchain-depth repair:** the user confirmed the 3D
  terrain/skirt visual regression was fixed. The depth format is enforced in
  current `BgfxNativeSurface` and primary resize code.
- **Still needs recorded acceptance after the final internal split/fixes:**
  native build and `aowis-render-core-link`, all focused lifecycle/target/input
  tests, Vulkan/OpenGL 3D depth during orbit and resize, repeated Map ↔ Projects
  handoffs, Alt+Tab mid-gesture, and native device teardown/recreation.
- **Not established:** GPU frame pacing, Windows/WebAssembly multi-window
  support, QtQuick/native mobile hosting, independent network repositories per
  diagnostic view, and a release-quality public renderer interface.

Run the commands in [Verification status](verification-status.md) and read
[Rendering architecture](rendering-architecture.md) for implementation detail.
The architecture is sufficient to start a **separate future extraction task**,
but extraction is not part of this documentation update.

## Extraction boundary: avoid accidental scope growth

Do not migrate `MapBgfxSurface` whole into the reusable library just to move
files. Its QWidget/window lifetime, repository access, map/controller state
and editor actions are GUI responsibilities. Likewise, the two diagnostic
windows are proof of multiple GPU views, not a three-way merge UI or a reusable
multi-editor API. When an actual extraction is authorized, use the already
linked core as the starting boundary, inspect **real** include/dependency
edges before moving additional code, and preserve the link-test plus full
native regression matrix.
