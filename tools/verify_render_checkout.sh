#!/usr/bin/env bash
set -euo pipefail

# Verify that the Render commit is actually materialized on disk. A Git
# submodule can report the expected HEAD even when sparse/skip-worktree state
# has left required files absent. Restore only missing required tracked files;
# never overwrite a developer's existing edits.
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
render_dir="${1:-${project_dir}/external/AOWIS-GUI-Render}"

if [[ $# -gt 1 ]]; then
    printf 'Usage: %s [Render submodule directory]\n' "$0" >&2
    exit 2
fi
if [[ ! -d "$render_dir" ]] || ! git -C "$render_dir" rev-parse --verify HEAD >/dev/null 2>&1; then
    printf 'ERROR: Render submodule is not checked out at %s\n' "$render_dir" >&2
    printf 'Run ./git_submodule_init.sh --render first.\n' >&2
    exit 1
fi

required_files=(
    src/map/bgfx/map_bgfx_globe_vertex_layouts.h
    src/map/bgfx/map_bgfx_globe_vertex_layouts.cpp
    src/map/render/map_globe_link_geometry.h
    src/map/render/map_globe_link_geometry.cpp
    src/map/render/map_globe_flow_geometry.h
    src/map/render/map_globe_flow_geometry.cpp
    src/map/render/map_globe_underground_geometry.h
    src/map/render/map_globe_underground_geometry.cpp
    src/map/render/map_globe_scene_sync.h
    src/map/render/map_globe_scene_sync.cpp
    src/map/render/map_globe_tile_topology.h
    src/map/render/map_globe_tile_topology.cpp
    src/map/bgfx/map_bgfx_globe_tile_index_pool.h
    src/map/bgfx/map_bgfx_globe_tile_index_pool.cpp
    src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.h
    src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.cpp
    src/map/bgfx/map_bgfx_globe_terrain_gpu.h
    src/map/bgfx/map_bgfx_globe_terrain_gpu.cpp
    src/map/bgfx/map_bgfx_globe_overlay_gpu.h
    src/map/bgfx/map_bgfx_globe_overlay_gpu.cpp
    src/geo/geo_metric_projection.h
    src/geo/geo_metric_projection.cpp
    src/geo/geo_web_mercator.h
    src/geo/geo_web_mercator.cpp
    src/geo/geo_wgs84_ellipsoid.h
    src/geo/geo_wgs84_ellipsoid.cpp
    src/map/data/map_terrain_tile.h
    src/map/data/map_terrain_tile.cpp
    src/map/data/map_terrain_dataset.h
    src/map/data/map_terrain_dataset.cpp
    cmake/aowis_render_dependencies.cmake
    src/map/core/map_model.h
    src/map/core/map_model.cpp
    src/map/core/map_view_types.h
    src/map/core/map_imagery_provider_directory.h
    src/map/render/map_camera_view_state.h
    src/map/render/map_render_diagnostics.h
    src/map/render/map_globe_picking.h
    src/map/render/map_globe_picking.cpp
    src/map/render/map_render_cache_math.h
    src/map/render/map_render_cache_math.cpp
    src/map/render/map_globe_surface_scene.h
    src/map/render/map_globe_surface_scene.cpp
    src/map/render/map_globe_surface_preparation.h
    src/map/render/map_globe_surface_preparation.cpp
    src/map/render/map_terrain_mesh_scheduler.h
    src/map/render/map_terrain_mesh_scheduler.cpp
    src/map/render/map_globe_terrain_navigation.h
    src/map/render/map_globe_terrain_navigation.cpp
    src/map/render/map_globe_camera.h
    src/map/render/map_globe_camera.cpp
    src/map/render/map_globe_heatmap_scene.h
    src/map/render/map_globe_heatmap_scene.cpp
    src/map/render/map_globe_retained_origin.h
    src/map/render/map_globe_terrain_levels.h
    src/map/data/map_terrain_tile_source.h
    src/map/render/map_globe_surface_hit.h
    src/services/interface_server_map.h
    src/map/data/map_tile_repository.h
    src/map/data/map_tile_repository.cpp
    src/map/data/map_terrain_repository.h
    src/map/data/map_terrain_repository.cpp
    src/map/data/map_tile_browser_decoder.h
    src/map/data/map_tile_browser_decoder.cpp
    src/map/data/map_server_transport.h
    src/network/network_render_snapshot.h
    src/network/network_symbology.h
    src/network/network_symbology_rendering.h
    src/network/infrastructure_entity_traits.h
    src/map/render/map_globe_network_scene.h
    src/map/render/map_globe_network_scene.cpp
    src/map/render/map_globe_render_frame.h
    src/map/render/map_globe_render_frame.cpp
    src/map/render/map_network_render_data.h
    src/map/render/map_retained_network_scene.h
    src/map/render/map_globe_icon_draw_order.h
    src/map/render/map_icon_atlas.h
    src/map/render/map_icon_atlas.cpp
    src/map/render/map_label_settings.h
    src/map/render/map_tile_streaming_policy.h
    src/map/render/map_render_types.h
    src/map/render/map_network_render_symbology.h
    src/map/core/map_network_types.h
    src/map/bgfx/map_bgfx_entity_style.h
    src/map/bgfx/map_bgfx_entity_style.cpp
    src/map/bgfx/map_bgfx_globe_network.h
    src/map/bgfx/map_bgfx_globe_network.cpp
    src/map/bgfx/map_bgfx_globe_surface.h
    src/map/bgfx/map_bgfx_globe_surface.cpp
    src/map/bgfx/map_bgfx_2d_symbology.h
    src/map/bgfx/map_bgfx_2d_snapshot_adapter.h
    src/map/bgfx/map_bgfx_2d_picking.h
    src/map/bgfx/map_bgfx_layer_visibility.h
    src/map/bgfx/map_bgfx_hud.h
    src/map/bgfx/map_bgfx_hud.cpp
    src/map/bgfx/map_bgfx_labels.h
    src/map/bgfx/map_bgfx_labels.cpp
    src/map/bgfx/map_bgfx_map_view.h
    src/map/bgfx/map_bgfx_map_view.cpp
    src/map/core/map_frame_pacer.h
    src/map/core/map_input_focus_policy.h
    src/map/bgfx/bgfx_qt_native_surface.h
    src/map/bgfx/bgfx_qt_native_surface.cpp
    src/services/map_server_rest_configuration.h
    src/services/rest_client.h
    src/services/rest_client.cpp
    src/services/interface_server_map_rest.h
    src/services/interface_server_map_rest.cpp
    src/map/core/map_provider_catalog.h
    src/map/core/map_provider_catalog.cpp
    src/map/core/map_provider_fallback_resolution.h
)

for relative in "${required_files[@]}"; do
    if ! git -C "$render_dir" cat-file -e "HEAD:${relative}" 2>/dev/null; then
        printf 'ERROR: Render HEAD is missing required source %s\n' "$relative" >&2
        printf 'Update Render with ./git_submodule_update.sh --render and verify the pinned commit.\n' >&2
        exit 1
    fi
done

for relative in "${required_files[@]}"; do
    if [[ -f "${render_dir}/${relative}" ]]; then
        continue
    fi
    if [[ "$(git -C "$render_dir" config --bool core.sparseCheckout 2>/dev/null || true)" == true ]]; then
        printf 'Render sparse checkout omits required sources; materializing full Render checkout.\n'
        git -C "$render_dir" sparse-checkout disable
    fi
    if [[ ! -f "${render_dir}/${relative}" ]]; then
        printf 'Restoring missing Render source from pinned HEAD: %s\n' "$relative"
        git -C "$render_dir" restore --source=HEAD --worktree -- "$relative"
    fi
    if [[ ! -f "${render_dir}/${relative}" ]]; then
        printf 'ERROR: Unable to restore required Render source: %s\n' "$relative" >&2
        exit 1
    fi
done

printf 'PASS: Render checkout contains required geo, terrain data, globe scene and layout sources.\n'
