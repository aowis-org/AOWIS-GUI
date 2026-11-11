#!/usr/bin/env bash
# Explains what the next build would rebuild, and why, without building
# anything. Run it right after a finished build, without changing files:
# any planned compile step means something rewrites inputs.
#
#   ./tools/explain_rebuild.sh [build-dir]
#     default build-dir: build-linux (./compile_linux.sh)
#     target: aowis-tests if the directory has it, else aowis-server-gui
#
# Writes build-dir/rebuild-explain.txt (upload that file).
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
build="${1:-$root/build-linux}"
[[ -f "$build/build.ninja" ]] || { printf 'No Ninja build in %s\n' "$build" >&2; exit 1; }
report="$build/rebuild-explain.txt"
raw="$(mktemp)"
trap 'rm -f "$raw" "$raw.plan" "$raw.configure"' EXIT

# -n: dry run; -d explain: Ninja's reason for every dirty edge (stderr).
# Repeat the configure step the compile scripts run every time (cached
# options), and record which build files it rewrites: a configure-generated
# file with a fresh timestamp is the usual cause of "rebuild everything".
marker="$(mktemp)"
sleep 1
cmake -S "$root" -B "$build" >"$raw.configure" 2>&1 || { cat "$raw.configure" >&2; exit 1; }
rewritten="$(find "$build" -type f -newer "$marker" \
    ! -path '*/CMakeFiles/CMakeConfigureLog.yaml' ! -name '.ninja_*' ! -name 'rebuild-explain.txt' \
    2>/dev/null | sed "s#^$build/##" | sort)"
rm -f "$marker" "$raw.configure"

target=aowis-server-gui
if ninja -C "$build" -t targets all 2>/dev/null | grep -q '^aowis-tests:'; then target=aowis-tests; fi
ninja -C "$build" -n -d explain "$target" >"$raw.plan" 2>"$raw" || true
planned="$(grep -c '^\[' "$raw.plan" || true)"
{
    printf 'AOWIS rebuild explanation (%s)\n' "$(date -Iseconds)"
    printf 'Build directory: %s (target %s)\n' "$build" "$target"
    printf 'Planned steps: %s\n\n' "$planned"
    printf '== Files rewritten by a plain re-configure (%s) ==\n' "$(printf '%s\n' "$rewritten" | grep -c . || true)"
    printf '%s\n' "$rewritten" | head -40
    printf '\n'
    printf '== Most frequent newest inputs (what triggered the rebuild) ==\n'
    sed -n 's/.*older than most recent input \([^ ]*\) .*/\1/p' "$raw" \
        | sed "s#^$root/##" | sort | uniq -c | sort -rn | head -25
    printf '\n== Missing outputs ==\n'
    sed -n "s/.*output \([^ ]*\) doesn't exist.*/\1/p" "$raw" | sed "s#^$build/##" | head -25
    printf '\n== Changed commands ==\n'
    grep 'command line changed' "$raw" | sed "s#$build/##" | head -25 || true
    printf '\n== Planned steps (first 40) ==\n'
    head -40 "$raw.plan"
    printf '\n== First 60 raw reasons ==\n'
    sed "s#$root/##g" "$raw" | head -60
} >"$report"
cat "$report"
printf '\nReport written to %s\n' "$report"
