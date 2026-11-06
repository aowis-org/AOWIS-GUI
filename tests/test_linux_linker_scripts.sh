#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "${tmp_dir}"' EXIT
mkdir -p "${tmp_dir}/project/tools" "${tmp_dir}/project/build-linux" "${tmp_dir}/bin"
cp "${project_dir}/compile_linux.sh" "${project_dir}/compile_linux_tests.sh" \
    "${project_dir}/compile_linux_benchmarks.sh" "${tmp_dir}/project/"
cp "${project_dir}/tools/linux_linker.sh" "${tmp_dir}/project/tools/"

cat > "${tmp_dir}/bin/cmake" <<'MOCK'
#!/usr/bin/env bash
if [[ "${1:-}" == "--build" ]]; then
    exit 0
fi
printf '%s\n' "$*" >> "${MOCK_CMAKE_LOG}"
MOCK
cat > "${tmp_dir}/bin/ctest" <<'MOCK'
#!/usr/bin/env bash
if [[ " $* " == *" -N "* ]]; then
    printf 'Total Tests: 1\n'
fi
MOCK
cat > "${tmp_dir}/bin/python3" <<'MOCK'
#!/usr/bin/env bash
exit 0
MOCK
cat > "${tmp_dir}/bin/ld.lld" <<'MOCK'
#!/usr/bin/env bash
exit 0
MOCK
cat > "${tmp_dir}/project/build-linux/aowis-server-gui" <<'MOCK'
#!/usr/bin/env bash
exit 0
MOCK
chmod +x "${tmp_dir}/bin/"* "${tmp_dir}/project/build-linux/aowis-server-gui"
export PATH="${tmp_dir}/bin:${PATH}"
export MOCK_CMAKE_LOG="${tmp_dir}/cmake.log"
cd "${tmp_dir}/project"

# Native application, Debug/Release tests, and Debug/Release benchmarks.
: > "${MOCK_CMAKE_LOG}"
bash ./compile_linux.sh >/dev/null
bash ./compile_linux_tests.sh --both >/dev/null
bash ./compile_linux_benchmarks.sh --both >/dev/null
[[ "$(wc -l < "${MOCK_CMAKE_LOG}")" == 5 ]]
[[ "$(grep -Fc -- '-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld' "${MOCK_CMAKE_LOG}")" == 5 ]]
echo 'PASS: all five native Linux configurations default to LLD'

# Explicit opt-out resets CMake's cached linker flags as well.
: > "${MOCK_CMAKE_LOG}"
AOWIS_LINUX_LINKER=system bash ./compile_linux.sh >/dev/null
AOWIS_LINUX_LINKER=system bash ./compile_linux_tests.sh --debug >/dev/null
AOWIS_LINUX_LINKER=system bash ./compile_linux_benchmarks.sh --release >/dev/null
[[ "$(wc -l < "${MOCK_CMAKE_LOG}")" == 3 ]]
[[ "$(grep -Ec -- '(^| )-DCMAKE_EXE_LINKER_FLAGS=( |$)' "${MOCK_CMAKE_LOG}")" == 3 ]]
echo 'PASS: system linker override clears previously cached LLD flags'

if AOWIS_LINUX_LINKER=unsupported bash ./compile_linux.sh >"${tmp_dir}/error.log" 2>&1; then
    echo 'FAIL: unsupported linker value accepted' >&2
    exit 1
fi
grep -q 'unsupported AOWIS_LINUX_LINKER' "${tmp_dir}/error.log"
echo 'PASS: invalid linker selection is rejected'

if PATH="${tmp_dir}/missing" /bin/bash -c \
    'source "$1"; aowis_linux_executable_linker_flags' _ \
    "${project_dir}/tools/linux_linker.sh" >"${tmp_dir}/error.log" 2>&1; then
    echo 'FAIL: missing LLD accepted' >&2
    exit 1
fi
grep -q 'ld.lld is missing' "${tmp_dir}/error.log"
echo 'PASS: missing LLD produces actionable guidance'
