# Render integration

The map engine is **AOWIS-GUI-Render**, a separate repository pinned at
`external/AOWIS-GUI-Render`. It draws the 2D map and the globe, streams and
caches imagery and terrain, and talks to aowis-server-map. AOWIS-GUI-Mobile
uses the same engine. The GUI is one of its hosts: it supplies the Qt Widgets
window, input, settings, assets and the hydraulic network.

Render's own documentation: `README.md` (libraries), `docs/architecture.md`
(data pipeline and correctness contracts), `docs/renderer.md` (how things
are drawn), `docs/host-integration.md` (hosting rules) and `docs/roadmap.md`.

## What the GUI adds

| Render library | GUI side |
| --- | --- |
| `AOWIS::RenderGeo` | Links the GUI's `GeographicLib_STATIC` and `AOWIS::Model`, added before Render |
| `AOWIS::RenderView` (`MapModel`) | `MainWindow` owns the one `MapModel`, forwards provider-catalog changes and injects the orbit sensitivity |
| `AOWIS::RenderMapData` (tile and terrain repositories) | `guiMapServerTransport()` (`services/map_server_transport_factory.*`) creates the REST transport or the Standalone in-process one, with the Windows Standalone request limits |
| `AOWIS::RenderMapRest` | `guiMapServerRestConfiguration()` builds the server configuration from the GUI settings; `mapProviderCatalog()` (`map/core/gui_map_provider_catalog.*`) is the process's catalog and stores fallbacks in the GUI configuration file |
| `AOWIS::RenderGlobeNetwork` (network contract) | `network_render_snapshot_builder` fills `NetworkRenderSnapshot` from `HydraulicData`; `MapNetworkSymbologyResolver` fills the symbology from simulation results |
| `AOWIS::RenderMap` (`MapBgfxMapView`) | `MapBgfxSurface` hosts it ([map host guide](../src/map/bgfx/README.md)), with the terrain detail setting, tank/reservoir textures, HUD images, shortcuts, the unit-aware scale bar, `MapBgfxGlobeHudController` and the legend content |
| `AOWIS::RenderQtSurface` | `ProjectGraphBgfxViewport` captures its own window through it |
| `AOWIS::RenderDevice` | The Projects graph and the diagnostic map windows register as clients of the one device ([Rendering architecture](rendering-architecture.md)) |

The GUI keeps everything application-specific: Widgets, editing tools and
placement, hydraulic identity and elevations, units and locale, settings, the
Standalone server. Render must not depend on any of it; do not move
`MainWindow`, services, widgets or MODEL hydraulic types into Render to
share a class.

## Build

`CMakeLists.txt` adds Render with `add_subdirectory()` after GeographicLib
and MODEL, switches on all Render libraries, and builds bgfx from the GUI's
own pinned `external/{bx,bimg,bgfx}` (`AOWIS_RENDER_VENDOR_ROOT`), so there is
exactly one bgfx. Configuration stops with a message when a required Render
file is missing. `AOWIS_RENDER_SOURCE_DIR` points at another Render checkout
for explicit development only; normally the gitlink decides.

Embedded shaders keep their resource paths
`:/aowis/map/bgfx/<family>/<shader>.bin`; the GUI has no shader pack of its
own. `./compile_linux.sh --shader-tools` regenerates Render's committed
shaders.

## Changing Render and the GUI together

```sh
# In AOWIS-GUI-Render: apply its patch, then
./compile_linux_tests.sh
git add -A && git commit -m "..." && git push      # before touching the GUI
```

```sh
# In the GUI checkout
./git_submodule_update.sh --render
./tools/verify_render_checkout.sh
./compile_linux_tests.sh
```

`compile_linux_tests.sh` builds and links the whole application and every
test before CTest runs; a CTest pass on stale binaries after a failed build
is not possible. The GUI suite includes `aowis-render-extraction-boundary`
(no source that moved to Render is back in the GUI; Render's globe code keeps
using Render's GPU pools and layouts),
`aowis-render-core-link` and the device tests. `verify_render_checkout.sh`
catches a Render checkout with missing files before compiling.

Source and mock tests prove algorithms and GPU call sequences, not real
rendering. Check by hand on OpenGL and Vulkan: terrain LOD skirts and depth,
DEM tiles arriving, imagery fallback and provider changes, Solid / X-Ray /
Hide, heatmaps, sky, resize, and repeated device retirement and recreation.
