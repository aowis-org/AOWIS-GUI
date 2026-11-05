#!/bin/bash
set -euo pipefail

PROJECT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage()
{
    cat <<'USAGE'
Usage: ./compile_linux_tests.sh [--debug | --release | --both]

  (no option)  Keep the original build-linux-tests configuration and directory.
  --debug      Build/test the explicit Debug configuration in build-linux-tests-debug.
  --release    Build/test the explicit Release configuration in build-linux-tests-release.
  --both       Run the independent Debug and Release builds; fail if either fails.

Only the aowis-tests target is built; all tests with the 'aowis' label are run.
The existing no-argument workflow is unchanged.
USAGE
}

run_suite()
{
    local build_dir="$1"
    local build_type="$2"
    local cmake_args=(
        -S "${PROJECT_DIR}"
        -B "${build_dir}"
        -G Ninja
        -DBUILD_TESTING=ON
    )
    if [[ -n "${build_type}" ]]; then
        cmake_args+=("-DCMAKE_BUILD_TYPE=${build_type}")
    fi

    echo "=== AOWIS Linux unit tests: ${build_type:-existing configuration} ==="
    cmake "${cmake_args[@]}"
    cmake --build "${build_dir}" --target aowis-tests --parallel
    # JSON inspection avoids CTest -N's dozens of repeated "looked in" paths
    # for every unbuilt executable. The preflight prints only missing test names.
    python3 "${PROJECT_DIR}/tools/ctest_preflight.py" "${build_dir}"
    # Normal CTest output includes per-test results and a final summary.
    # Verbose mode produces far too much noise; output from successful tests
    # stays hidden while failed tests include their diagnostics.
    ctest \
        --test-dir "${build_dir}" \
        --output-on-failure \
        --timeout 30 \
        -L '^aowis$'
}

case "${1:-}" in
    '')
        run_suite "${PROJECT_DIR}/build-linux-tests" ''
        ;;
    --debug)
        run_suite "${PROJECT_DIR}/build-linux-tests-debug" Debug
        ;;
    --release)
        run_suite "${PROJECT_DIR}/build-linux-tests-release" Release
        ;;
    --both)
        run_suite "${PROJECT_DIR}/build-linux-tests-debug" Debug
        run_suite "${PROJECT_DIR}/build-linux-tests-release" Release
        ;;
    --help|-h)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
