#!/usr/bin/env bash
# Finds what truncates build-linux/.ninja_deps ("ninja: warning: premature end
# of file; recovering"), which makes every build recompile ~130 objects.
# Builds step by step and checks the deps log after each step:
#   A  current state
#   B  build              (repairs; may rebuild everything once)
#   C  build again        (expected: 0 compile steps, log intact)
#   D  cmake configure    (what compile_linux.sh runs first every time)
#   E  build again        (compile steps here = configure is the culprit)
#
#   bash tools/diagnose_rebuild.sh [build-dir]   (default: build-linux)
# Writes build-dir/rebuild-diagnosis.txt (upload that file).
set -uo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
build="${1:-$root/build-linux}"
[[ -f "$build/CMakeCache.txt" ]] || { printf 'No configured build in %s\n' "$build" >&2; exit 1; }
report="$build/rebuild-diagnosis.txt"
ninja_bin="$(sed -n 's/^CMAKE_MAKE_PROGRAM:[A-Z]*=//p' "$build/CMakeCache.txt")"
[[ -x "$ninja_bin" ]] || ninja_bin="$(command -v ninja)"
: >"$report"
say() { printf '%s\n' "$*" | tee -a "$report"; }

deps_state() {
    local warn size
    warn="$("$ninja_bin" -C "$build" -t deps 2>&1 >/dev/null | grep -c 'premature end' || true)"
    size="$(stat -c %s "$build/.ninja_deps" 2>/dev/null || echo missing)"
    say "    .ninja_deps: ${size} bytes, truncation warning: $([[ "$warn" -gt 0 ]] && echo YES || echo no)"
}
build_step() {
    local log="$build/.diagnose-$1.log"
    "$ninja_bin" -C "$build" aowis-server-gui >"$log" 2>&1
    local rc=$?
    local compiles links
    compiles="$(grep -c 'Building C' "$log" || true)"
    links="$(grep -c 'Linking' "$log" || true)"
    say "    exit $rc, compile steps $compiles, link steps $links$(grep -q 'premature end' "$log" && echo ', ninja warned: premature end of file')"
    [[ $rc -eq 0 ]] || { say "    build failed, see $log"; }
}

say "AOWIS rebuild diagnosis $(date -Iseconds)"
say "build dir: $build"
say "make program: $ninja_bin ($("$ninja_bin" --version 2>/dev/null))"
say "ninja on PATH: $(type -ap ninja samu 2>/dev/null | tr '\n' ' ')"
say "cmake: $(cmake --version | head -1)"
say "filesystem: $(df -T "$build" | awk 'NR==2 {print $2" ("$7")"}')"
say "other build/IDE processes:"
# An IDE building or configuring the same directory concurrently corrupts it.
ps -eo pid=,comm=,args= | awk '$2 ~ /^(ninja|samu|cmake|cmake-gui|qtcreator|clion|code|codium|clangd|kdevelop)$/' \
    | cut -c1-200 | sed 's/^/    /' | tee -a "$report" || true
say ""
say "A current state"; deps_state
say "B build"; build_step B; deps_state
say "C build again"; build_step C; deps_state
say "D cmake configure (as compile_linux.sh)"
cmake -S "$root" -B "$build" >"$build/.diagnose-D.log" 2>&1; say "    exit $?"; deps_state
say "E build again"; build_step E; deps_state
say ""
say "Report written to $report"
