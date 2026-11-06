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

printf 'PASS: Render checkout contains required globe scene and layout sources.\n'
