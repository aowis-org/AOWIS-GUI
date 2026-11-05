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
    src/map/bgfx/map_bgfx_hud_canvas.h; do
    if [[ -e "$relative" ]]; then
        echo "Duplicate extracted Render source remains in GUI: $relative" >&2
        exit 1
    fi
done
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
printf 'PASS: shared renderer submodule and source ownership boundary\n'
