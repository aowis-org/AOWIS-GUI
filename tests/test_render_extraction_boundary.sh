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
    src/map/render/map_globe_reservoir_model.cpp \
    src/geo/geo_metric_projection.h \
    src/geo/geo_metric_projection.cpp \
    src/geo/geo_web_mercator.h \
    src/geo/geo_web_mercator.cpp \
    src/geo/geo_wgs84_ellipsoid.h \
    src/geo/geo_wgs84_ellipsoid.cpp \
    src/map/data/map_terrain_tile.h \
    src/map/data/map_terrain_tile.cpp \
    src/map/data/map_terrain_dataset.h \
    src/map/data/map_terrain_dataset.cpp \
    src/map/core/map_model.h \
    src/map/core/map_model.cpp \
    src/map/render/map_camera_view_state.h \
    src/map/render/map_render_diagnostics.h \
    src/map/render/map_globe_picking.h \
    src/map/render/map_globe_picking.cpp \
    src/map/render/map_render_cache_math.h \
    src/map/render/map_render_cache_math.cpp \
    src/map/render/map_globe_surface_scene.h \
    src/map/render/map_globe_surface_scene.cpp \
    src/map/render/map_globe_surface_preparation.h \
    src/map/render/map_globe_surface_preparation.cpp \
    src/map/render/map_terrain_mesh_scheduler.h \
    src/map/render/map_terrain_mesh_scheduler.cpp \
    src/map/render/map_globe_terrain_navigation.h \
    src/map/render/map_globe_terrain_navigation.cpp \
    src/map/render/map_globe_camera.h \
    src/map/render/map_globe_camera.cpp \
    src/map/render/map_globe_heatmap_scene.h \
    src/map/render/map_globe_heatmap_scene.cpp \
    src/map/render/map_globe_retained_origin.h \
    src/map/render/map_globe_terrain_levels.h \
    src/services/interface_server_map.h \
    src/map/data/map_tile_repository.h \
    src/map/data/map_tile_repository.cpp \
    src/map/data/map_terrain_repository.h \
    src/map/data/map_terrain_repository.cpp \
    src/map/data/map_tile_browser_decoder.h \
    src/map/data/map_tile_browser_decoder.cpp \
    src/network/network_render_snapshot.h \
    src/network/network_symbology.h \
    src/network/network_symbology_rendering.h \
    src/network/infrastructure_entity_traits.h \
    src/map/render/map_globe_network_scene.h \
    src/map/render/map_globe_network_scene.cpp \
    src/map/render/map_globe_render_frame.h \
    src/map/render/map_globe_render_frame.cpp \
    src/map/render/map_network_render_data.h \
    src/map/render/map_retained_network_scene.h \
    src/map/render/map_globe_icon_draw_order.h \
    src/map/render/map_icon_atlas.h \
    src/map/render/map_icon_atlas.cpp \
    src/map/render/map_label_settings.h \
    src/map/render/map_tile_streaming_policy.h \
    src/map/render/map_render_types.h \
    src/map/render/map_network_render_symbology.h \
    src/map/bgfx/map_bgfx_entity_style.h \
    src/map/bgfx/map_bgfx_entity_style.cpp \
    src/map/bgfx/map_bgfx_globe_network.h \
    src/map/bgfx/map_bgfx_globe_network.cpp \
    src/map/bgfx/map_bgfx_globe_surface.h \
    src/map/bgfx/map_bgfx_globe_surface.cpp \
    src/map/bgfx/map_bgfx_2d_symbology.h \
    src/map/bgfx/map_bgfx_2d_snapshot_adapter.h \
    src/map/bgfx/map_bgfx_2d_picking.h \
    src/map/bgfx/map_bgfx_layer_visibility.h \
    src/map/bgfx/map_bgfx_hud.h \
    src/map/bgfx/map_bgfx_hud.cpp \
    src/map/bgfx/map_bgfx_labels.h \
    src/map/bgfx/map_bgfx_labels.cpp \
    src/map/core/map_frame_pacer.h \
    src/map/core/map_input_focus_policy.h \
    tests/map_input_focus_policy_test.cpp \
    tests/bgfx_entity_style_test.cpp \
    tests/bgfx_hud_test.cpp \
    tests/bgfx_labels_test.cpp \
    tests/bgfx_2d_picking_test.cpp \
    tests/map_bgfx_layer_visibility_test.cpp; do
    if [[ -e "$relative" ]]; then
        echo "Duplicate extracted Render source remains in GUI: $relative" >&2
        exit 1
    fi
done
# The bgfx globe network and surface live in Render (AOWIS::RenderMap); they
# must keep using Render's GPU pools, layouts, programs and overlay GPU.
# Checked against the Render checkout (absent in --source-only snapshots);
# Render's tests/test_extraction_contract.py enforces the same.
if [[ -f "$render/src/map/bgfx/map_bgfx_globe_surface.cpp" ]]; then
    # All retained globe network buffers are owned by Render, not by the host.
    if grep -Eq 'Dynamic(Vertex|Index)BufferHandle|createDynamic(Vertex|Index)Buffer|bgfx::update\(' \
        "$render/src/map/bgfx/map_bgfx_globe_network.cpp" "$render/src/map/bgfx/map_bgfx_globe_network.h"; then
        echo 'GUI globe network still owns bgfx dynamic GPU buffers' >&2
        exit 1
    fi
    if ! grep -Fq 'gpu_buffers.populateInput(&draw_input)' "$render/src/map/bgfx/map_bgfx_globe_network.cpp"; then
        echo 'GUI globe draw path bypasses shared GPU stream handles' >&2
        exit 1
    fi
    # Vertex layouts are constructed and validated by Render. GUI supplies the
    # host scene structs' byte strides, then uses Render's five layout accessors.
    if grep -Eq 'this->(link|node|icon|junction|model)_layout' \
        "$render/src/map/bgfx/map_bgfx_globe_network.cpp" "$render/src/map/bgfx/map_bgfx_globe_network.h"; then
        echo 'GUI globe network still references removed local vertex layouts' >&2
        exit 1
    fi
    if ! grep -Fq 'this->gpu_layouts.initialize(strides)' \
        "$render/src/map/bgfx/map_bgfx_globe_network.cpp"; then
        echo 'GUI globe network does not initialize Render-owned vertex layouts' >&2
        exit 1
    fi
    for layout in link node icon junction model; do
        if ! grep -Fq "this->gpu_layouts.${layout}()" \
            "$render/src/map/bgfx/map_bgfx_globe_network.cpp"; then
            echo "GUI globe network does not use Render-owned ${layout} layout" >&2
            exit 1
        fi
    done
    # Globe surface topology and GPU tile index buffers also belong to Render.
    if grep -Eq 'tile_index_buffers|MapBgfxGlobeSurface::tileIndexBuffer\(' \
        "$render/src/map/bgfx/map_bgfx_globe_surface.cpp" "$render/src/map/bgfx/map_bgfx_globe_surface.h"; then
        echo 'GUI globe surface still owns terrain tile GPU index buffers' >&2
        exit 1
    fi
    if ! grep -Fq 'this->tile_index_pool.get(tile.grid_cell_count)' \
        "$render/src/map/bgfx/map_bgfx_globe_surface.cpp"; then
        echo 'GUI globe surface bypasses Render tile index pool' >&2
        exit 1
    fi
    # Terrain dynamic vertex/cap-index ownership and sparse GPU updates belong
    # to Render; GUI supplies only the geometry and patch offsets.
    if grep -Eq 'Dynamic(Vertex|Index)BufferHandle|createDynamic(Vertex|Index)Buffer|bgfx::update\(' \
        "$render/src/map/bgfx/map_bgfx_globe_surface.h" "$render/src/map/bgfx/map_bgfx_globe_surface.cpp"; then
        echo 'GUI globe terrain surface still owns mutable GPU buffers' >&2
        exit 1
    fi
    if ! grep -Fq 'this->terrain_buffers.patchWindow(' \
        "$render/src/map/bgfx/map_bgfx_globe_surface.cpp"; then
        echo 'GUI globe terrain surface bypasses sparse Render patch uploads' >&2
        exit 1
    fi

    # Globe terrain GPU programs, frame uniforms, fallback textures and draw
    # submission must all remain in Render. Heatmap baking/sky are separate.
    if grep -Eq 'this->(surface_program|wireframe_program|tile_sampler|view_projection_uniform|missing_texture|cap_texture|tile_uniform|background_uniform|origin_uniform|light_uniform|shading_uniform)' \
        "$render/src/map/bgfx/map_bgfx_globe_surface.cpp" "$render/src/map/bgfx/map_bgfx_globe_surface.h"; then
        echo 'GUI still owns extracted terrain GPU programs or uniforms' >&2
        exit 1
    fi
    if sed -n '/MapBgfxGlobeSurface::FrameResult MapBgfxGlobeSurface::render(/,/\/\/ --- Heatmap/p' \
        "$render/src/map/bgfx/map_bgfx_globe_surface.cpp" \
        | grep -Eq 'bgfx::(setIndexBuffer|setVertexBuffer|submit)\('; then
        echo 'GUI globe frame still submits terrain draws directly' >&2
        exit 1
    fi
    if ! grep -Fq 'this->terrain_gpu.drawTile(' "$render/src/map/bgfx/map_bgfx_globe_surface.cpp" || \
       ! grep -Fq 'this->terrain_gpu.drawCap(' "$render/src/map/bgfx/map_bgfx_globe_surface.cpp" || \
       ! grep -Fq 'this->terrain_gpu.drawWireframe(' "$render/src/map/bgfx/map_bgfx_globe_surface.cpp"; then
        echo 'GUI surface bypasses shared terrain GPU drawing' >&2
        exit 1
    fi

    # Programs, uniforms and GPU material textures now belong to Render.
    if grep -Eq 'bgfx::(createUniform|createTexture2D|destroy\()' \
        "$render/src/map/bgfx/map_bgfx_globe_network.cpp" "$render/src/map/bgfx/map_bgfx_globe_network.h"; then
        echo 'GUI globe network still creates or destroys Render-owned GPU resources' >&2
        exit 1
    fi
    if ! grep -Fq 'gpu_resources.populateInput(&draw_input)' \
        "$render/src/map/bgfx/map_bgfx_globe_network.cpp"; then
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
    # The globe network scene lives in Render (AOWIS::RenderGlobeNetwork); it must
    # keep using Render's shared flow, link, underground and invalidation code.
    # Checked against the Render checkout (absent in --source-only snapshots).
    globe_scene="$render/src/map/render/map_globe_network_scene.cpp"
    if [[ -f "$globe_scene" ]]; then
        # The Globe scene may adapt flow strokes into its host vertex ABI, but
        # placement and ellipsoid tangent math must remain in the Render library.
        if grep -Eq 'FlowDirection(SpacingPixels|ChevronHalfWidthRatio|MaximumMarkersPerLink)|MapGlobeNetworkScene::ellipsoidNormalAt' \
            "$globe_scene"; then
            echo 'Globe scene: still owns extracted Globe flow placement or normal calculation' >&2
            exit 1
        fi
        if ! grep -Fq 'MapGlobeFlowGeometry::emitStrokes(' \
            "$globe_scene"; then
            echo 'Globe scene: Globe scene bypasses shared flow geometry' >&2
            exit 1
        fi
        if grep -Eq 'longLinkSubdivisionCount' \
            "$globe_scene"; then
            echo 'Globe scene: still owns extracted globe link geometry' >&2
            exit 1
        fi
        if ! grep -Fq 'MapGlobeLinkGeometry::emitCurvedSegments' "$globe_scene" || \
           ! grep -Fq 'MapGlobeLinkGeometry::emitRibbonCorners' "$globe_scene"; then
            echo 'Globe scene: is not using shared globe link geometry' >&2
            exit 1
        fi
        # Terrain-aware X-Ray classification and run coalescing must stay in Render.
        if grep -Eq 'UndergroundSubdivision(FallbackTargetLengthM|MinimumTargetLengthM|TerrainCellFraction|MaximumCount)' \
            "$globe_scene"; then
            echo 'Globe scene: still owns extracted underground sampling policy' >&2
            exit 1
        fi
        if ! grep -Fq 'MapGlobeUndergroundGeometry::subdivisionCount(' \
            "$globe_scene" || \
           ! grep -Fq 'MapGlobeUndergroundGeometry::emitBuriedRuns(' \
            "$globe_scene"; then
            echo 'Globe scene: X-Ray path bypasses Render underground run classification' >&2
            exit 1
        fi

        # Retained Globe scene invalidation is owned by Render. The GUI may execute
        # the plan using its hydraulic/terrain data but cannot decide stream groups.
        if ! grep -Fq 'MapGlobeSceneSync::forSymbology(changes)' \
            "$globe_scene" || \
           ! grep -Fq 'applySceneInvalidation(' \
            "$globe_scene"; then
            echo 'Globe scene: Globe scene bypasses shared retained-scene invalidation policy' >&2
            exit 1
        fi
    fi

    # Sky and heatmap GPU resources must not regress to GUI ownership.
    if grep -Eq 'this->(sky_program|sky_triangle|sky_forward_uniform|heatmap_bake_program|heatmap_surface_program|heatmap_pages)|bgfx::createFrameBuffer\(|bgfx::allocTransientVertexBuffer\(' \
        "$render/src/map/bgfx/map_bgfx_globe_surface.cpp" "$render/src/map/bgfx/map_bgfx_globe_surface.h"; then
        echo 'GUI globe surface owns GPU sky or heatmap resources again' >&2
        exit 1
    fi
    for token in 'this->sky_gpu.initialize(' 'this->sky_gpu.draw(' 'this->heatmap_gpu.initialize(' 'this->heatmap_gpu.bakePage('; do
        if ! grep -Fq "$token" "$render/src/map/bgfx/map_bgfx_globe_surface.cpp"; then
            echo "GUI globe surface bypasses shared overlay GPU: $token" >&2
            exit 1
        fi
    done
fi

# The GUI map surface only hosts Render's MapBgfxMapView (Qt Widgets
# embedding, events, GUI settings and assets): no bgfx frame, device or GPU
# code of its own.
if grep -Eq 'bgfx::|BgfxDeviceManager|BgfxFrameCoordinator|EM_JS|emscripten_' \
    src/map/bgfx/map_bgfx_surface.h src/map/bgfx/map_bgfx_surface.cpp; then
    echo 'GUI map surface renders with bgfx itself instead of hosting MapBgfxMapView' >&2
    exit 1
fi
if ! grep -Fq 'std::make_unique<MapBgfxMapView>(' src/map/bgfx/map_bgfx_surface.cpp; then
    echo 'GUI map surface does not host the shared MapBgfxMapView' >&2
    exit 1
fi

printf 'PASS: shared renderer submodule and source ownership boundary\n'
