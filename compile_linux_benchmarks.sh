#!/bin/bash
set -euo pipefail

PROJECT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "${PROJECT_DIR}/tools/linux_linker.sh"
LINKER_FLAGS="$(aowis_linux_executable_linker_flags)"

usage()
{
    cat <<'USAGE'
Usage: ./compile_linux_benchmarks.sh [--release | --debug | --both]

  (no option)  Build and run Release benchmarks.
  --release    Build and run benchmarks in build-linux-tests-release.
  --debug      Build and run benchmarks in build-linux-tests-debug.
  --both       Run independently in Debug and Release.
  --help       Show this help.

Builds only the aowis-benchmarks target. Runs CTest cases labeled "performance"
with verbose output so benchmark JSON and timing measurements are visible even
when the tests pass. No machine-dependent timing thresholds are enforced.

Environment:
  AOWIS_LINUX_LINKER  lld (default), or system for the compiler default.
USAGE
}

run_benchmarks()
{
    local build_type="$1"
    local build_dir="${PROJECT_DIR}/build-linux-tests-${build_type,,}"

    echo "=== AOWIS Linux benchmarks: ${build_type} ==="
    cmake -S "${PROJECT_DIR}" -B "${build_dir}" -G Ninja \
        -DBUILD_TESTING=ON "-DCMAKE_BUILD_TYPE=${build_type}" \
        "-DCMAKE_EXE_LINKER_FLAGS=${LINKER_FLAGS}"
    cmake --build "${build_dir}" --target aowis-benchmarks --parallel

    # CTest may succeed with zero matching tests: explicitly reject this case.
    local registered
    registered=$(ctest --test-dir "${build_dir}" -N -L '^performance$')
    if ! grep -Eq 'Total Tests: [1-9][0-9]*' <<< "${registered}"; then
        echo "ERROR: no performance benchmarks registered in ${build_dir}" >&2
        echo "${registered}" >&2
        return 1
    fi

    echo "=== Benchmark output (${build_type}; JSON appears below) ==="
    ctest --test-dir "${build_dir}" --verbose --output-on-failure -L '^performance$'
}

case "${1:-}" in
    ''|--release)
        run_benchmarks Release
        ;;
    --debug)
        run_benchmarks Debug
        ;;
    --both)
        run_benchmarks Debug
        run_benchmarks Release
        ;;
    --help|-h)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
