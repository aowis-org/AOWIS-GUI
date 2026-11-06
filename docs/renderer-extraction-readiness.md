# Shared renderer integration in the GUI

**Current source boundary — 6 November 2026.** `AOWIS-GUI-Render` is a real,
standalone, independently built repository. The GUI consumes it at
`external/AOWIS-GUI-Render` as a pinned Git submodule. It is not merely an
internal GUI static library and it is **not yet** a complete map engine:
CPU-side terrain/imagery preparation and the host-owned render loop remain
in GUI. See the Render repository's `README.md`,
`docs/map-engine-boundary.md`, and `docs/roadmap.md` for source ownership and
remaining forward work.

## Build and dependency boundary

| Render library | Implemented responsibility | GUI-side dependency/adapter |
| --- | --- | --- |
| `AOWIS::RenderGeometry` | Header-only C++20 Mercator/camera/icon/height-cache and imagery fallback helpers | MapModel/camera and scene state remain with host |
| `AOWIS::RenderGeo` | `src/geo/`: WGS84/ECEF and orbit camera basis, Web Mercator tile math, UTM / project-local metric projection | Links the GUI's `GeographicLib_STATIC` and `AOWIS::Model` (added before Render) |
| `AOWIS::RenderView` | `MapModel` (camera/view state, screen <-> WGS84, imagery tile addressing), `MapCameraViewState`, globe picking, render cache math, render diagnostics | `MapProviderCatalog` implements `MapImageryProviderDirectory`; `MainWindow` forwards its change signals and injects the orbit sensitivity |
| `AOWIS::RenderGlobeTerrain` | Globe terrain CPU pipeline: quadtree selection/LOD, surface preparation (tile window, sparse DEM patches, terrain queries), mesh scheduler, terrain navigation, globe camera, heatmap scene | `MapTerrainRepository` implements `MapTerrainTileSource`; `MapBgfxGlobeSurface` / `MapBgfxSurface` inject the terrain detail setting and forward tile signals |
| `AOWIS::RenderTerrainData` | `src/map/data/map_terrain_tile.*`, `map_terrain_dataset.*`: DEM tile decoder, tile keys/endpoints, dataset catalog | `map_terrain_repository.*` requests, caches and transports the bytes |
| `AOWIS::RenderNetwork` | CPU 2D network scene, variants, mesh staging | `map_bgfx_2d_snapshot_adapter.h` and host style binding |
| `AOWIS::RenderGlobe` | Tank/reservoir, link, flow and underground geometry, terrain tile topology and retained scene invalidation policy | Hydraulic scene snapshots, GeographicLib sampling, terrain queries and scene updates |
| `AOWIS::RenderDevice` | Single bgfx device, frame/view coordination, native target registry | Qt native/window adapter and toolkit event routing |
| `AOWIS::RenderCore` | 2D GPU/imagery atlas and HUD caches; globe network resources/draw; terrain buffer/index pools, terrain draw, sky and heatmap GPU owners, shader assets | `MapBgfxSurface`, `MapBgfxGlobeSurface`, QImage model resources, frame and tile preparation |

CMake includes Render via `add_subdirectory(...)`, using a caller-owned
`bgfx` target and pinned vendor checkouts. `AOWIS_RENDER_SOURCE_DIR` supports
**only explicit** development overrides; otherwise the GUI gitlink is the
source of truth. There is no independent GUI-owned bgfx runtime or second
shader pack. Embedded shader resource URLs stay
`:/aowis/map/bgfx/<family>/<shader>.bin`.

### Current GPU resource owners

- `MapBgfxGlobeGpu`, `MapBgfxGlobeBufferPool`,
  `MapBgfxGlobeResourcePool` and `MapBgfxGlobeVertexLayouts`: 3D network
  submission, vertex/index streams, shader programs, uniforms, samplers,
  tank/reservoir material textures and shader-facing layout descriptors.
- `MapGlobeTileTopology`, `MapBgfxGlobeTileIndexPool` and
  `MapBgfxGlobeTerrainBufferPool`: terrain grid/skirt topology, cached indices,
  window/cap/wireframe buffers and sparse DEM range uploads.
- `MapBgfxGlobeTerrainGpu`: terrain, caps and wireframe drawing.
- `MapBgfxGlobeSkyGpu` and `MapBgfxGlobeHeatmapGpu`: sky and heatmap draw
  submission, bake render targets and resource lifecycle.

**Do not bring these GPU handles back into `map_bgfx_globe_network.cpp` or
`map_bgfx_globe_surface.cpp`.** GUI supplies neutral geometry and asset
inputs, plus camera/tile selections, rather than duplicating ownership.

## Still in GUI — by design for now

- `src/map/data/map_terrain_repository.*`: dataset catalog request, DEM
  tile requests, caching (decoding is Render's `AOWIS::RenderTerrainData`).
- `src/map/data/map_tile_repository.*` and
  `src/map/render/map_tile_streaming_policy.h`: imagery request scheduling,
  priorities, decoded tile cache and cancellation.
- `src/map/core/map_provider_catalog.*` and server/config files: provider
  administration, fallback selection, server transport.
- Qt Widgets/native-surface/event lifecycle, hydraulic/model adaptation,
  picking/placement, locale/unit configuration and UI controllers.

Engine-neutral **CPU** logic will also move into Render. The stable boundary
is the host's tile/source interface: application settings and server request
adapters stay outside. Do not embed `MapModel`, widgets, server interfaces or
hydraulic MODEL types in the shared cache/scene/GPU targets just to move a
large class verbatim.

## Apply, build and test

For Render-first changes:

```sh
# In AOWIS-GUI-Render, after applying its patch
./compile_linux.sh && ./compile_linux_tests.sh
# Commit + push Render's actual tracked sources before updating GUI.
```

```sh
# In AOWIS-SERVER-GUI, after publishing Render and applying the GUI adapter
./git_submodule_update.sh --render && \
  ./tools/verify_render_checkout.sh && \
  ./compile_linux_tests.sh
```

`compile_linux_tests.sh` must **successfully build and link the complete GUI
application and all test executables** before starting CTest. A successful
CTest run from an old binary after a compile failure is invalid. The GUI
suite includes `aowis-render-extraction-boundary`,
`aowis-render-core-link`, terrain/scene regressions and device tests. The
checkout verifier prevents missing Render files from reaching compilation.

Source/mock regressions prove algorithms and GPU call sequences—not real
OpenGL/Vulkan/WASM/Android rendering. Native checks should cover terrain LOD
skirt stitching and D24S8 depth, sparse DEM arrival, imagery fallback and
provider changes, Solid/X-Ray/Hide, heatmaps, sky, resize and repeated device
retirement/recreation.

## Forward direction

The next engineering work is a complete shared **terrain/imagery data-to-GPU
pipeline**, not merely more GPU code: imagery residency, normalized DEM
format, quadtree and LOD, terrain mesh scheduling, streaming/caching and
terrain ray/height queries. Then common HUD/labels/overlays, stable scene
interfaces and host lifecycle cleanup. Mobile's production renderer
integration remains deferred until those boundaries are demonstrated in GUI.
