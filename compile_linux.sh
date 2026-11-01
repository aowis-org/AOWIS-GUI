#!/bin/bash
set -euo pipefail

PROJECT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="${PROJECT_DIR}/build-linux"

usage()
{
    cat <<'HELP'
Usage: ./compile_linux.sh [--shader-tools]

  --shader-tools  Also build the bgfx shader compiler and regenerate the
                  committed shader binaries in src/map/bgfx/shaders/bin
                  (needed after editing a .sc file; commit the result).
                  The dx11 (Windows) family needs Wine on PATH.

Environment:
  AOWIS_BUILD_TYPE  CMake build type (default RelWithDebInfo: optimized with
                    debug info). Use Debug for an unoptimized build.
HELP
}

SHADER_TOOLS=OFF
case "${1:-}" in
    '') ;;
    --shader-tools) SHADER_TOOLS=ON ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

cd "${PROJECT_DIR}"

# The shader compiler sources are not part of the default sparse checkout.
if [ "${SHADER_TOOLS}" = "ON" ] && { [ ! -d external/bgfx/tools/shaderc ] \
    || [ ! -f external/bgfx/tools/bin/windows/d3d4linux.exe ]; }; then
    tools/bgfx_sparse_checkout.sh --shader-tools
fi

# Explicit values, so an existing build-linux cache configured while an
# option had another value is switched as well (option() defaults never
# override a cached value). Without --shader-tools no shader compiler is
# built: the committed binaries are embedded.
# The build type is explicit too: without one CMake compiles unoptimized
# (no -O flags at all), and the per-frame map work (bgfx, HUD, editor
# overlay, labels) then costs several times as much as in an optimized build.
BUILD_TYPE="${AOWIS_BUILD_TYPE:-RelWithDebInfo}"
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DAOWIS_BGFX_SHADER_TOOLS="${SHADER_TOOLS}" \
    -DBUILD_TESTING=OFF
cmake --build "${BUILD_DIR}" --target aowis-server-gui --parallel

"${BUILD_DIR}/aowis-server-gui"
