#!/bin/bash
set -euo pipefail

PROJECT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "${PROJECT_DIR}/tools/linux_linker.sh"
LINKER_FLAGS="$(aowis_linux_executable_linker_flags)"

usage()
{
    cat <<'USAGE'
Usage: ./compile_linux_tests.sh [--debug | --release | --both]

  (no option)  Keep the original build-linux-tests configuration and directory.
  --debug      Build/test the explicit Debug configuration in build-linux-tests-debug.
  --release    Build/test the explicit Release configuration in build-linux-tests-release.
  --both       Run the independent Debug and Release builds; fail if either fails.

The aowis-tests target builds the entire GUI executable AND all test binaries.
CTest runs only after configuration, compilation, linking, and preflight succeed.
The no-argument workflow uses the existing build-linux-tests directory.

Environment:
  AOWIS_LINUX_LINKER  lld (default), or system for the compiler default.
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
        "-DCMAKE_EXE_LINKER_FLAGS=${LINKER_FLAGS}"
    )
    if [[ -n "${build_type}" ]]; then
        cmake_args+=("-DCMAKE_BUILD_TYPE=${build_type}")
    fi

    echo "=== AOWIS Linux unit tests: ${build_type:-existing configuration} ==="
    if ! cmake "${cmake_args[@]}"; then
        echo 'ERROR: CMake configuration failed. No tests were executed.' >&2
        return 1
    fi
    # aowis-tests also depends on aowis-server-gui: a passing test-only
    # compile cannot conceal a broken GUI executable link. Never invoke CTest
    # when Ninja fails; CTest can otherwise run STALE test executables.
    if ! cmake --build "${build_dir}" --target aowis-tests --parallel; then
        echo 'ERROR: GUI or test compilation/linking failed. No tests were executed.' >&2
        return 1
    fi
    # JSON inspection avoids CTest -N's repeated missing-executable paths.
    if ! python3 "${PROJECT_DIR}/tools/ctest_preflight.py" "${build_dir}"; then
        echo 'ERROR: Test executable preflight failed. No tests were executed.' >&2
        return 1
    fi
    echo 'PASS: GUI and test executables built successfully; starting CTest.'
    # Only run tests after the entire application and all test targets link.
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
