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
    # CTest 3.21 can return success when a label matches zero tests. Verify the
    # selection explicitly instead of requiring newer --no-tests=error support.
    local registered
    registered=$(ctest --test-dir "${build_dir}" -N -L '^aowis$' 2>&1)
    if ! grep -Eq 'Total Tests: [1-9][0-9]*' <<< "${registered}"; then
        echo "ERROR: no aowis tests registered in ${build_dir}" >&2
        echo "${registered}" >&2
        return 1
    fi
    # CTest registers tests even when their executable was not built. Fail
    # before running a long suite instead of reporting misleading Not Run
    # results at the end.
    if grep -Eq 'Could not find executable|Unable to find executable' <<< "${registered}"; then
        echo "ERROR: aowis test executables are missing from ${build_dir}" >&2
        echo "${registered}" >&2
        return 1
    fi
    ctest \
        --test-dir "${build_dir}" \
        --verbose \
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
