#!/usr/bin/env bash
set -euo pipefail

# Initialize every pinned submodule revision.
#
# Usage: ./git_submodule_init.sh [--shader-tools | --render]
#
#   --shader-tools  also check out the bgfx shader-compiler sources
#                   (only for regenerating AOWIS-GUI-Render shaders).
#   --render        initialize just the pinned AOWIS-GUI-Render submodule.
#
# bx, bimg and bgfx are fetched shallow (only the pinned commit) and checked
# out sparse (only what Render/cmake/aowis_bgfx.cmake builds), see
# tools/bgfx_sparse_checkout.sh. bgfx.cmake is no longer used.

# Resolve paths relative to this script, not the caller's working directory.
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

sparse_option=""
render_only=false
case "${1:-}" in
    '') ;;
    --shader-tools) sparse_option="--shader-tools" ;;
    --render) render_only=true ;;
    *)
        printf 'Usage: %s [--shader-tools | --render]\n' "$0" >&2
        exit 2
        ;;
esac
if [[ $# -gt 1 ]]; then
    printf 'Usage: %s [--shader-tools | --render]\n' "$0" >&2
    exit 2
fi

render_path=external/AOWIS-GUI-Render
# .gitmodules configuration is not sufficient: a real submodule must have a
# Git index entry with mode 160000. Do not silently use a sibling checkout.
if [[ "$(git ls-files --stage -- "$render_path")" != '160000 '* ]]; then
    printf 'Missing Render Git submodule gitlink: %s\n' "$render_path" >&2
    printf 'Register with: git submodule add https://github.com/aowis-org/AOWIS-GUI-Render.git %s\n' "$render_path" >&2
    exit 1
fi
if "$render_only"; then
    git submodule sync --recursive
    git submodule update --init --recursive --checkout -- "$render_path"
    "${PWD}/tools/verify_render_checkout.sh" "$render_path"
    exit 0
fi

# A .gitmodules entry alone is not a Git submodule: the superproject index
# also needs a gitlink (mode 160000). ZIP patches cannot carry gitlinks.
# For the first checkout using the bgfx ZIP patch, register missing gitlinks
# through the updater. Once committed, normal clones skip this bootstrap.
bgfx_paths=(external/bx external/bimg external/bgfx)
for path in "${bgfx_paths[@]}"; do
    if [[ "$(git ls-files --stage -- "$path")" != '160000 '* ]]; then
        printf 'Missing bgfx submodule gitlink: %s. Registering bgfx dependencies first...\n' "$path"
        ./git_submodule_update.sh --bgfx
        break
    fi
done

# Sync URLs from .gitmodules and check out exactly the revisions pinned by
# the superproject; do not silently advance existing submodules to --remote.
git submodule sync --recursive
git submodule update --init --recursive -- \
    "$render_path" \
    external/GeographicLib \
    external/AOWIS-SERVER-MODEL \
    external/AOWIS-SERVER-EPANET
# Never recursive: AOWIS needs none of bgfx's own submodules. --depth 1 only
# affects new clones; existing full clones are left as they are.
git submodule update --init --depth 1 -- "${bgfx_paths[@]}"
"${PWD}/tools/verify_render_checkout.sh" "$render_path"
tools/bgfx_sparse_checkout.sh ${sparse_option}
