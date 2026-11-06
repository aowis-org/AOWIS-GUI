#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
verifier="${root}/tools/verify_render_checkout.sh"
temporary="$(mktemp -d)"
trap 'rm -rf -- "$temporary"' EXIT

create_render_repo()
{
    local path="$1"
    git init -q -b main "$path"
    git -C "$path" config user.name 'AOWIS Test'
    git -C "$path" config user.email 'test@example.invalid'
    mkdir -p "$path/src/map/bgfx" "$path/src/map/render" "$path/tools"
    printf 'header original\n' > "$path/src/map/bgfx/map_bgfx_globe_vertex_layouts.h"
    printf 'source original\n' > "$path/src/map/bgfx/map_bgfx_globe_vertex_layouts.cpp"
    printf 'flow header original\n' > "$path/src/map/render/map_globe_flow_geometry.h"
    printf 'flow source original\n' > "$path/src/map/render/map_globe_flow_geometry.cpp"
    printf 'link header original\n' > "$path/src/map/render/map_globe_link_geometry.h"
    printf 'link source original\n' > "$path/src/map/render/map_globe_link_geometry.cpp"
    printf 'underground header original\n' > "$path/src/map/render/map_globe_underground_geometry.h"
    printf 'underground source original\n' > "$path/src/map/render/map_globe_underground_geometry.cpp"
    printf 'scene sync header original\n' > "$path/src/map/render/map_globe_scene_sync.h"
    printf 'scene sync source original\n' > "$path/src/map/render/map_globe_scene_sync.cpp"
    printf 'tile topology header original\n' > "$path/src/map/render/map_globe_tile_topology.h"
    printf 'tile topology source original\n' > "$path/src/map/render/map_globe_tile_topology.cpp"
    printf 'tile GPU pool header original\n' > "$path/src/map/bgfx/map_bgfx_globe_tile_index_pool.h"
    printf 'tile GPU pool source original\n' > "$path/src/map/bgfx/map_bgfx_globe_tile_index_pool.cpp"
    printf 'terrain vertex pool header original\n' > "$path/src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.h"
    printf 'terrain vertex pool source original\n' > "$path/src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.cpp"
    printf 'terrain GPU header original\n' > "$path/src/map/bgfx/map_bgfx_globe_terrain_gpu.h"
    printf 'terrain GPU source original\n' > "$path/src/map/bgfx/map_bgfx_globe_terrain_gpu.cpp"
    printf 'overlay GPU header original\n' > "$path/src/map/bgfx/map_bgfx_globe_overlay_gpu.h"
    printf 'overlay GPU source original\n' > "$path/src/map/bgfx/map_bgfx_globe_overlay_gpu.cpp"
    # Geodesy and terrain data moved to Render with their own CMake module.
    mkdir -p "$path/src/geo" "$path/src/map/data" "$path/src/map/core" "$path/cmake"
    for moved in src/geo/geo_metric_projection src/geo/geo_web_mercator \
                 src/geo/geo_wgs84_ellipsoid src/map/data/map_terrain_tile \
                 src/map/data/map_terrain_dataset; do
        printf 'moved header original\n' > "$path/$moved.h"
        printf 'moved source original\n' > "$path/$moved.cpp"
    done
    printf 'dependencies original\n' > "$path/cmake/aowis_render_dependencies.cmake"
    # Globe terrain pipeline moved to Render.
    for moved in src/map/render/map_globe_surface_scene.h src/map/render/map_globe_surface_scene.cpp src/map/render/map_globe_surface_preparation.h src/map/render/map_globe_surface_preparation.cpp src/map/render/map_terrain_mesh_scheduler.h src/map/render/map_terrain_mesh_scheduler.cpp src/map/render/map_globe_terrain_navigation.h src/map/render/map_globe_terrain_navigation.cpp src/map/render/map_globe_camera.h src/map/render/map_globe_camera.cpp src/map/render/map_globe_heatmap_scene.h src/map/render/map_globe_heatmap_scene.cpp src/map/render/map_globe_retained_origin.h src/map/render/map_globe_terrain_levels.h src/map/data/map_terrain_tile_source.h src/map/render/map_globe_surface_hit.h; do
        printf 'moved terrain source original\n' > "$path/$moved"
    done
    # MapModel and its helpers moved to Render.
    for moved in src/map/core/map_model.h src/map/core/map_model.cpp src/map/core/map_view_types.h src/map/core/map_imagery_provider_directory.h src/map/render/map_camera_view_state.h src/map/render/map_render_diagnostics.h src/map/render/map_globe_picking.h src/map/render/map_globe_picking.cpp src/map/render/map_render_cache_math.h src/map/render/map_render_cache_math.cpp; do
        printf 'moved view source original\n' > "$path/$moved"
    done
    printf 'test helper\n' > "$path/tools/readme"
    git -C "$path" add .
    git -C "$path" commit -qm 'fixture'
}

render="$temporary/render"
create_render_repo "$render"
"$verifier" "$render" >/dev/null
echo 'PASS: complete tracked checkout accepted'

rm -- "$render/src/map/bgfx/map_bgfx_globe_vertex_layouts.h" \
      "$render/src/map/bgfx/map_bgfx_globe_vertex_layouts.cpp" \
      "$render/src/map/render/map_globe_flow_geometry.h" \
      "$render/src/map/render/map_globe_flow_geometry.cpp" \
      "$render/src/map/render/map_globe_link_geometry.h" \
      "$render/src/map/render/map_globe_link_geometry.cpp" \
      "$render/src/map/render/map_globe_underground_geometry.h" \
      "$render/src/map/render/map_globe_underground_geometry.cpp" \
      "$render/src/map/render/map_globe_scene_sync.h" \
      "$render/src/map/render/map_globe_scene_sync.cpp" \
      "$render/src/map/render/map_globe_tile_topology.h" \
      "$render/src/map/render/map_globe_tile_topology.cpp" \
      "$render/src/map/bgfx/map_bgfx_globe_tile_index_pool.h" \
      "$render/src/map/bgfx/map_bgfx_globe_tile_index_pool.cpp" \
      "$render/src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.h" \
      "$render/src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.cpp" \
      "$render/src/map/bgfx/map_bgfx_globe_terrain_gpu.h" \
      "$render/src/map/bgfx/map_bgfx_globe_terrain_gpu.cpp" \
      "$render/src/map/bgfx/map_bgfx_globe_overlay_gpu.h" \
      "$render/src/map/bgfx/map_bgfx_globe_overlay_gpu.cpp"
"$verifier" "$render" >/dev/null
grep -qx 'header original' "$render/src/map/bgfx/map_bgfx_globe_vertex_layouts.h"
grep -qx 'source original' "$render/src/map/bgfx/map_bgfx_globe_vertex_layouts.cpp"
grep -qx 'flow header original' "$render/src/map/render/map_globe_flow_geometry.h"
grep -qx 'flow source original' "$render/src/map/render/map_globe_flow_geometry.cpp"
grep -qx 'link header original' "$render/src/map/render/map_globe_link_geometry.h"
grep -qx 'link source original' "$render/src/map/render/map_globe_link_geometry.cpp"
grep -qx 'underground header original' "$render/src/map/render/map_globe_underground_geometry.h"
grep -qx 'underground source original' "$render/src/map/render/map_globe_underground_geometry.cpp"
grep -qx 'scene sync header original' "$render/src/map/render/map_globe_scene_sync.h"
grep -qx 'scene sync source original' "$render/src/map/render/map_globe_scene_sync.cpp"
grep -qx 'tile topology header original' "$render/src/map/render/map_globe_tile_topology.h"
grep -qx 'tile topology source original' "$render/src/map/render/map_globe_tile_topology.cpp"
grep -qx 'tile GPU pool header original' "$render/src/map/bgfx/map_bgfx_globe_tile_index_pool.h"
grep -qx 'tile GPU pool source original' "$render/src/map/bgfx/map_bgfx_globe_tile_index_pool.cpp"
grep -qx 'terrain vertex pool header original' "$render/src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.h"
grep -qx 'terrain vertex pool source original' "$render/src/map/bgfx/map_bgfx_globe_terrain_buffer_pool.cpp"
grep -qx 'terrain GPU header original' "$render/src/map/bgfx/map_bgfx_globe_terrain_gpu.h"
grep -qx 'terrain GPU source original' "$render/src/map/bgfx/map_bgfx_globe_terrain_gpu.cpp"
grep -qx 'overlay GPU header original' "$render/src/map/bgfx/map_bgfx_globe_overlay_gpu.h"
grep -qx 'overlay GPU source original' "$render/src/map/bgfx/map_bgfx_globe_overlay_gpu.cpp"
echo 'PASS: missing files restored from pinned HEAD'

printf 'local edited header\n' > "$render/src/map/bgfx/map_bgfx_globe_vertex_layouts.h"
"$verifier" "$render" >/dev/null
grep -qx 'local edited header' "$render/src/map/bgfx/map_bgfx_globe_vertex_layouts.h"
echo 'PASS: existing local edits preserved'

sparse="$temporary/sparse"
create_render_repo "$sparse"
git -C "$sparse" sparse-checkout set --cone tools
[[ ! -e "$sparse/src/map/bgfx/map_bgfx_globe_vertex_layouts.h" ]]
"$verifier" "$sparse" >/dev/null
[[ -f "$sparse/src/map/bgfx/map_bgfx_globe_vertex_layouts.h" ]]
[[ "$(git -C "$sparse" config --bool core.sparseCheckout)" == false ]]
echo 'PASS: sparse checkout of required source corrected'

obsolete="$temporary/obsolete"
git init -q -b main "$obsolete"
git -C "$obsolete" config user.name 'AOWIS Test'
git -C "$obsolete" config user.email 'test@example.invalid'
printf 'outdated\n' > "$obsolete/README.md"
git -C "$obsolete" add README.md
git -C "$obsolete" commit -qm 'old revision'
if "$verifier" "$obsolete" >"$temporary/obsolete.out" 2>&1; then
    echo 'ERROR: outdated Render revision was accepted' >&2
    exit 1
fi
grep -q 'HEAD is missing required source' "$temporary/obsolete.out"
echo 'PASS: outdated Render revision fails with actionable message'

# The GUI must detect a broken checkout before any compilation begins.
grep -q 'AOWIS-GUI-Render checkout is incomplete' "$root/CMakeLists.txt"
grep -q 'verify_render_checkout.sh' "$root/git_submodule_update.sh"
grep -q 'verify_render_checkout.sh' "$root/git_submodule_init.sh"
echo 'PASS: CMake configuration and submodule scripts enforce checkout integrity'
