#!/usr/bin/env bash
set -euo pipefail

# Regression: CTest must never run after a failed configure/build/preflight.
# In particular, a failed GUI link must not leave stale test binaries green.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
sandbox="$tmp/checkout with spaces"
mkdir -p "$sandbox/tools" "$tmp/bin"
cp "$root/compile_linux_tests.sh" "$sandbox/"
cp "$root/tools/linux_linker.sh" "$sandbox/tools/"

cat > "$tmp/bin/cmake" <<'MOCK'
#!/usr/bin/env bash
printf 'cmake %s\n' "$*" >> "$MOCK_EVENTS"
if [[ "${1:-}" == '--build' ]]; then
    [[ "${MOCK_FAIL:-}" != 'build' ]]
else
    [[ "${MOCK_FAIL:-}" != 'configure' ]]
fi
MOCK
cat > "$tmp/bin/python3" <<'MOCK'
#!/usr/bin/env bash
printf 'preflight %s\n' "$*" >> "$MOCK_EVENTS"
[[ "${MOCK_FAIL:-}" != 'preflight' ]]
MOCK
cat > "$tmp/bin/ctest" <<'MOCK'
#!/usr/bin/env bash
printf 'CTEST %s\n' "$*" >> "$MOCK_EVENTS"
[[ "${MOCK_FAIL:-}" != 'ctest' ]]
MOCK
cat > "$tmp/bin/ld.lld" <<'MOCK'
#!/usr/bin/env bash
exit 0
MOCK
chmod +x "$tmp/bin/"*
export PATH="$tmp/bin:$PATH"
export MOCK_EVENTS="$tmp/events.log"

# Each upstream failure must stop the script *before* CTest runs.
for phase in configure build preflight; do
    : > "$MOCK_EVENTS"
    if MOCK_FAIL="$phase" bash "$sandbox/compile_linux_tests.sh" >"$tmp/out" 2>&1; then
        echo "FAIL: $phase failure was accepted" >&2
        exit 1
    fi
    if grep -q '^CTEST ' "$MOCK_EVENTS"; then
        echo "FAIL: CTest was run after a $phase failure" >&2
        exit 1
    fi
    grep -q 'No tests were executed' "$tmp/out"
    printf 'PASS: %s failure stops before CTest\n' "$phase"
done

: > "$MOCK_EVENTS"
MOCK_FAIL=ctest bash "$sandbox/compile_linux_tests.sh" >"$tmp/out" 2>&1 && {
    echo 'FAIL: CTest failure did not propagate' >&2
    exit 1
}
[[ "$(grep -c '^CTEST ' "$MOCK_EVENTS")" -eq 1 ]]
echo 'PASS: CTest failure propagates'

: > "$MOCK_EVENTS"
bash "$sandbox/compile_linux_tests.sh" >"$tmp/out" 2>&1
[[ "$(grep -c '^CTEST ' "$MOCK_EVENTS")" -eq 1 ]]
[[ "$(grep -c '^cmake --build ' "$MOCK_EVENTS")" -eq 1 ]]
grep -q 'PASS: GUI and test executables built successfully' "$tmp/out"
echo 'PASS: successful build launches exactly one test run'

: > "$MOCK_EVENTS"
if MOCK_FAIL=build bash "$sandbox/compile_linux_tests.sh" --both >"$tmp/out" 2>&1; then
    echo 'FAIL: --both accepted a failed Debug build' >&2
    exit 1
fi
[[ "$(grep -c '^cmake --build ' "$MOCK_EVENTS")" -eq 1 ]]
! grep -q '^CTEST ' "$MOCK_EVENTS"
echo 'PASS: --both stops before Release after a failed Debug build'

# The script's target must also depend on the actual GUI executable, not just
# the test binaries. Fail fast if somebody removes this dependency.
grep -Eq '^[[:space:]]*add_dependencies\(aowis-tests aowis-server-gui\)' "$root/CMakeLists.txt"
echo 'PASS: aowis-tests includes full application compilation and linking'
