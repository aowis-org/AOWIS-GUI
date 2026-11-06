#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd -- "$root"

# The source snapshots intentionally omit submodule contents. The test checks
# the actual Git gitlink when run inside a real checkout.
render=external/AOWIS-GUI-Render
[[ "$(git config -f .gitmodules --get "submodule.$render.path")" == "$render" ]]
[[ "$(git config -f .gitmodules --get "submodule.$render.url")" == 'https://github.com/aowis-org/AOWIS-GUI-Render.git' ]]
if [[ "${1:-}" != '--source-only' ]]; then
    [[ "$(git ls-files --stage -- "$render")" == '160000 '* ]] || {
        echo "Missing real Render Git gitlink at $render" >&2
        exit 1
    }
    [[ -f "$render/CMakeLists.txt" ]] || {
        echo "Initialize Render with ./git_submodule_init.sh --render" >&2
        exit 1
    }
fi

# Everything that has moved must be absent locally so the GUI cannot compile
# or include an old copy accidentally. The only shader ownership is Render.
for relative in \
    cmake/aowis_bgfx.cmake \
    cmake/aowis_bgfx_shaders.cmake \
    cmake/aowis_bgfx_shaderc.cmake \
    src/map/bgfx/bgfx_device_manager.h \
    src/map/bgfx/bgfx_frame_coordinator.h \
    src/map/bgfx/bgfx_native_target_registry.h \
    src/map/bgfx/map_bgfx_backend.h \
    src/map/bgfx/map_bgfx_2d_imagery_gpu.h \
    src/map/bgfx/map_bgfx_hud_gpu.h \
    src/map/bgfx/map_bgfx_hud_canvas.h \
    src/map/bgfx/map_bgfx_2d_projection.h \
    src/map/bgfx/map_bgfx_three_view_camera.h \
    src/map/bgfx/map_bgfx_globe_icon_index.h \
    src/map/bgfx/map_bgfx_globe_height_cache.h \
    src/map/bgfx/map_bgfx_globe_imagery.h \
    src/map/bgfx/map_bgfx_pan_reuse.h \
    src/map/bgfx/map_bgfx_2d_scene.h \
    src/map/bgfx/map_bgfx_2d_scene_variants.h \
    src/map/bgfx/map_bgfx_2d_mesh.h \
    src/map/bgfx/map_bgfx_2d_gpu.cpp \
    src/map/bgfx/map_bgfx_2d_gpu.h \
    src/map/bgfx/map_bgfx_globe_gpu.h \
    src/map/bgfx/map_bgfx_globe_gpu.cpp \
    src/map/bgfx/map_bgfx_globe_buffer_pool.h \
    src/map/bgfx/map_bgfx_globe_buffer_pool.cpp \
    src/map/bgfx/map_bgfx_globe_resource_pool.h \
    src/map/bgfx/map_bgfx_globe_resource_pool.cpp \
    src/map/bgfx/map_bgfx_globe_texture_pixels.cpp \
    src/map/bgfx/map_bgfx_globe_draw_plan.h \
    src/map/render/map_node_declutter.h \
    src/map/render/map_node_declutter.cpp \
    src/map/render/map_globe_render_instances.h \
    src/map/render/map_globe_vertical_transform.h \
    src/map/render/map_globe_surface_vertex.h \
    src/map/render/map_globe_tile_topology.h \
    src/map/render/map_globe_tile_topology.cpp \
    src/map/bgfx/map_bgfx_globe_tile_index_pool.h \
    src/map/bgfx/map_bgfx_globe_tile_index_pool.cpp \
    src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.h \
    src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.cpp \
    src/map/bgfx/map_bgfx_globe_terrain_gpu.h \
    src/map/bgfx/map_bgfx_globe_terrain_gpu.cpp \
    src/map/bgfx/map_bgfx_globe_overlay_gpu.h \
    src/map/bgfx/map_bgfx_globe_overlay_gpu.cpp \
    src/map/render/map_globe_flow_geometry.h \
    src/map/render/map_globe_flow_geometry.cpp \
    src/map/render/map_globe_link_geometry.h \
    src/map/render/map_globe_link_geometry.cpp \
    src/map/render/map_globe_underground_geometry.h \
    src/map/render/map_globe_underground_geometry.cpp \
    src/map/render/map_globe_scene_sync.h \
    src/map/render/map_globe_scene_sync.cpp \
    src/map/render/map_globe_tank_model.h \
    src/map/render/map_globe_tank_model.cpp \
    src/map/render/map_globe_reservoir_model.h \
    src/map/render/map_globe_reservoir_model.cpp; do
    if [[ -e "$relative" ]]; then
        echo "Duplicate extracted Render source remains in GUI: $relative" >&2
        exit 1
    fi
done
# All retained globe network buffers are owned by Render, not by the host.
if grep -Eq 'Dynamic(Vertex|Index)BufferHandle|createDynamic(Vertex|Index)Buffer|bgfx::update\(' \
    src/map/bgfx/map_bgfx_globe_network.cpp src/map/bgfx/map_bgfx_globe_network.h; then
    echo 'GUI globe network still owns bgfx dynamic GPU buffers' >&2
    exit 1
fi
if ! grep -Fq 'gpu_buffers.populateInput(&draw_input)' src/map/bgfx/map_bgfx_globe_network.cpp; then
    echo 'GUI globe draw path bypasses shared GPU stream handles' >&2
    exit 1
fi
# Vertex layouts are constructed and validated by Render. GUI supplies the
# host scene structs' byte strides, then uses Render's five layout accessors.
if grep -Eq 'this->(link|node|icon|junction|model)_layout' \
    src/map/bgfx/map_bgfx_globe_network.cpp src/map/bgfx/map_bgfx_globe_network.h; then
    echo 'GUI globe network still references removed local vertex layouts' >&2
    exit 1
fi
if ! grep -Fq 'this->gpu_layouts.initialize(strides)' \
    src/map/bgfx/map_bgfx_globe_network.cpp; then
    echo 'GUI globe network does not initialize Render-owned vertex layouts' >&2
    exit 1
fi
for layout in link node icon junction model; do
    if ! grep -Fq "this->gpu_layouts.${layout}()" \
        src/map/bgfx/map_bgfx_globe_network.cpp; then
        echo "GUI globe network does not use Render-owned ${layout} layout" >&2
        exit 1
    fi
done
# Globe surface topology and GPU tile index buffers also belong to Render.
if grep -Eq 'tile_index_buffers|MapBgfxGlobeSurface::tileIndexBuffer\(' \
    src/map/bgfx/map_bgfx_globe_surface.cpp src/map/bgfx/map_bgfx_globe_surface.h; then
    echo 'GUI globe surface still owns terrain tile GPU index buffers' >&2
    exit 1
fi
if ! grep -Fq 'this->tile_index_pool.get(tile.grid_cell_count)' \
    src/map/bgfx/map_bgfx_globe_surface.cpp; then
    echo 'GUI globe surface bypasses Render tile index pool' >&2
    exit 1
fi
# Terrain dynamic vertex/cap-index ownership and sparse GPU updates belong
# to Render; GUI supplies only the geometry and patch offsets.
if grep -Eq 'Dynamic(Vertex|Index)BufferHandle|createDynamic(Vertex|Index)Buffer|bgfx::update\(' \
    src/map/bgfx/map_bgfx_globe_surface.h src/map/bgfx/map_bgfx_globe_surface.cpp; then
    echo 'GUI globe terrain surface still owns mutable GPU buffers' >&2
    exit 1
fi
if ! grep -Fq 'this->terrain_buffers.patchWindow(' \
    src/map/bgfx/map_bgfx_globe_surface.cpp; then
    echo 'GUI globe terrain surface bypasses sparse Render patch uploads' >&2
    exit 1
fi

# Globe terrain GPU programs, frame uniforms, fallback textures and draw
# submission must all remain in Render. Heatmap baking/sky are separate.
if grep -Eq 'this->(surface_program|wireframe_program|tile_sampler|view_projection_uniform|missing_texture|cap_texture|tile_uniform|background_uniform|origin_uniform|light_uniform|shading_uniform)' \
    src/map/bgfx/map_bgfx_globe_surface.cpp src/map/bgfx/map_bgfx_globe_surface.h; then
    echo 'GUI still owns extracted terrain GPU programs or uniforms' >&2
    exit 1
fi
if sed -n '/MapBgfxGlobeSurface::FrameResult MapBgfxGlobeSurface::render(/,/\/\/ --- Heatmap/p' \
    src/map/bgfx/map_bgfx_globe_surface.cpp \
    | grep -Eq 'bgfx::(setIndexBuffer|setVertexBuffer|submit)\('; then
    echo 'GUI globe frame still submits terrain draws directly' >&2
    exit 1
fi
if ! grep -Fq 'this->terrain_gpu.drawTile(' src/map/bgfx/map_bgfx_globe_surface.cpp || \
   ! grep -Fq 'this->terrain_gpu.drawCap(' src/map/bgfx/map_bgfx_globe_surface.cpp || \
   ! grep -Fq 'this->terrain_gpu.drawWireframe(' src/map/bgfx/map_bgfx_globe_surface.cpp; then
    echo 'GUI surface bypasses shared terrain GPU drawing' >&2
    exit 1
fi

# Programs, uniforms and GPU material textures now belong to Render.
if grep -Eq 'bgfx::(createUniform|createTexture2D|destroy\()' \
    src/map/bgfx/map_bgfx_globe_network.cpp src/map/bgfx/map_bgfx_globe_network.h; then
    echo 'GUI globe network still creates or destroys Render-owned GPU resources' >&2
    exit 1
fi
if ! grep -Fq 'gpu_resources.populateInput(&draw_input)' \
    src/map/bgfx/map_bgfx_globe_network.cpp; then
    echo 'GUI globe draw path bypasses Render-owned shader/texture handles' >&2
    exit 1
fi
if [[ -d src/map/bgfx/shaders ]] && \
   find src/map/bgfx/shaders -type f -print -quit | grep -q .; then
    echo 'GUI still contains Render-owned shaders' >&2
    exit 1
fi
if grep -Fq '"${CMAKE_CURRENT_SOURCE_DIR}/../AOWIS-GUI-Render"' CMakeLists.txt; then
    echo 'Implicit sibling checkout fallback remains in GUI CMake' >&2
    exit 1
fi
if ! grep -Fq 'external/AOWIS-GUI-Render" -o' tools/zip_gui_project.sh; then
    echo 'GUI source snapshots would recurse into the Render submodule' >&2
    exit 1
fi
# The Globe scene may adapt flow strokes into its host vertex ABI, but
# placement and ellipsoid tangent math must remain in the Render library.
if grep -Eq 'FlowDirection(SpacingPixels|ChevronHalfWidthRatio|MaximumMarkersPerLink)|MapGlobeNetworkScene::ellipsoidNormalAt' \
    src/map/render/map_globe_network_scene.cpp; then
    echo 'GUI still owns extracted Globe flow placement or normal calculation' >&2
    exit 1
fi
if ! grep -Fq 'MapGlobeFlowGeometry::emitStrokes(' \
    src/map/render/map_globe_network_scene.cpp; then
    echo 'GUI Globe scene bypasses shared flow geometry' >&2
    exit 1
fi
if grep -Eq 'longLinkSubdivisionCount' \
    src/map/render/map_globe_network_scene.cpp; then
    echo 'GUI still owns extracted globe link geometry' >&2
    exit 1
fi
if ! grep -Fq 'MapGlobeLinkGeometry::emitCurvedSegments' src/map/render/map_globe_network_scene.cpp || \
   ! grep -Fq 'MapGlobeLinkGeometry::emitRibbonCorners' src/map/render/map_globe_network_scene.cpp; then
    echo 'GUI is not using shared globe link geometry' >&2
    exit 1
fi
# Terrain-aware X-Ray classification and run coalescing must stay in Render.
if grep -Eq 'UndergroundSubdivision(FallbackTargetLengthM|MinimumTargetLengthM|TerrainCellFraction|MaximumCount)' \
    src/map/render/map_globe_network_scene.cpp; then
    echo 'GUI still owns extracted underground sampling policy' >&2
    exit 1
fi
if ! grep -Fq 'MapGlobeUndergroundGeometry::subdivisionCount(' \
    src/map/render/map_globe_network_scene.cpp || \
   ! grep -Fq 'MapGlobeUndergroundGeometry::emitBuriedRuns(' \
    src/map/render/map_globe_network_scene.cpp; then
    echo 'GUI X-Ray path bypasses Render underground run classification' >&2
    exit 1
fi

# Retained Globe scene invalidation is owned by Render. The GUI may execute
# the plan using its hydraulic/terrain data but cannot decide stream groups.
if ! grep -Fq 'MapGlobeSceneSync::forSymbology(changes)' \
    src/map/render/map_globe_network_scene.cpp || \
   ! grep -Fq 'applySceneInvalidation(' \
    src/map/render/map_globe_network_scene.cpp; then
    echo 'GUI Globe scene bypasses shared retained-scene invalidation policy' >&2
    exit 1
fi

# Sky and heatmap GPU resources must not regress to GUI ownership.
if grep -Eq 'this->(sky_program|sky_triangle|sky_forward_uniform|heatmap_bake_program|heatmap_surface_program|heatmap_pages)|bgfx::createFrameBuffer\(|bgfx::allocTransientVertexBuffer\(' \
    src/map/bgfx/map_bgfx_globe_surface.cpp src/map/bgfx/map_bgfx_globe_surface.h; then
    echo 'GUI globe surface owns GPU sky or heatmap resources again' >&2
    exit 1
fi
for token in 'this->sky_gpu.initialize(' 'this->sky_gpu.draw(' 'this->heatmap_gpu.initialize(' 'this->heatmap_gpu.bakePage('; do
    if ! grep -Fq "$token" src/map/bgfx/map_bgfx_globe_surface.cpp; then
        echo "GUI globe surface bypasses shared overlay GPU: $token" >&2
        exit 1
    fi
done

printf 'PASS: shared renderer submodule and source ownership boundary\n'
