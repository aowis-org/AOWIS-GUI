#!/usr/bin/env bash
set -euo pipefail

# Resolve paths relative to this script, not the caller's working directory.
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
    cat <<'HELP'
Usage: ./git_submodule_update.sh [--model | --epanet | --render | --network | --config | --epanet-deps | --geographiclib | --bgfx | --all | --help]

  (no option)  Update AOWIS-owned direct submodules (MODEL, EPANET, Render, NETWORK, CONFIG)
               to their tracked remote branches. Do not recurse into EPANET's
               third-party submodules or update GeographicLib/bgfx.
  --model      Update only AOWIS-SERVER-MODEL.
  --epanet     Update only AOWIS-SERVER-EPANET, not its nested dependencies.
  --render     Update only AOWIS-GUI-Render, not its nested dependencies.
  --network    Update only AOWIS-NETWORK and initialize its pinned dependencies.
  --config     Update only AOWIS-CONFIG (no nested dependencies).
  --epanet-deps
               Update AOWIS-SERVER-EPANET, then check out its nested third-party
               submodules at the exact versions pinned by EPANET (not remote tip).
  --geographiclib
               Pin only GeographicLib to r2.7; do not update AOWIS/bgfx.
  --bgfx       Initialize/update bgfx, bx and bimg to their upstream master
               (shallow, sparse; see tools/bgfx_sparse_checkout.sh).
               Does not touch the AOWIS and GeographicLib submodules.
               Afterwards regenerate the committed bgfx shaders with
               ./compile_linux.sh --shader-tools and commit them together
               with the new gitlinks: shader binaries must match bgfx.
  --all        Update AOWIS submodules, their EPANET-pinned dependencies,
               GeographicLib and bgfx. Third-party updates are opt-in.
  --help       Show this help.

On the first --bgfx run, any missing bgfx/bx/bimg Git submodules are
registered via 'git submodule add'. Commit the resulting .gitmodules and
submodule gitlinks together so other clones receive their pinned versions.
HELP
}

model_path="external/AOWIS-SERVER-MODEL"
epanet_path="external/AOWIS-SERVER-EPANET"
network_path="external/AOWIS-NETWORK"
config_path="external/AOWIS-CONFIG"
render_path="external/AOWIS-GUI-Render"

# Deliberate release pin: a new GeographicLib version requires an explicit
# compatibility check on Linux, Windows (MinGW), and WebAssembly, followed by
# updating this tag and committing the superproject's submodule gitlink.
# Never follow GeographicLib's default/development branch with --remote.
geographiclib_tag="r2.7"
geographiclib_path="external/GeographicLib"

bgfx_paths=(
    external/bx
    external/bimg
    external/bgfx
)

bgfx_urls=(
    https://github.com/bkaradzic/bx.git
    https://github.com/bkaradzic/bimg.git
    https://github.com/bkaradzic/bgfx.git
)

# A .gitmodules entry by itself does not register a submodule: the index also
# needs a gitlink (mode 160000). The source ZIP cannot carry Git index entries,
# so bootstrap them on the first --bgfx invocation in the user's actual clone.
ensure_bgfx_submodules_registered() {
    local i path url tracked
    for i in "${!bgfx_paths[@]}"; do
        path="${bgfx_paths[$i]}"
        url="${bgfx_urls[$i]}"
        tracked="$(git ls-files --stage -- "$path")"

        if [[ "$tracked" == '160000 '* ]]; then
            continue
        fi

        if [[ -e "$path" ]] && [[ -n "$(ls -A -- "$path")" ]]; then
            printf 'Cannot register %s: path exists and is not a tracked submodule.\n' "$path" >&2
            return 1
        fi

        printf 'Registering %s as a Git submodule...\n' "$path"
        git submodule add --depth 1 -- "$url" "$path"
        git config -f .gitmodules "submodule.$path.shallow" true
    done
}

update_geographiclib() {
    # First initialize the submodule at the revision pinned by the
    # superproject, then fetch exactly the stable release tag. The release
    # checkout contains VERSION, so GeographicLib does not default to -Werror
    # as its development checkout does.
    if [[ "$(git ls-files --stage -- "$geographiclib_path")" != '160000 '* ]]; then
        printf 'GeographicLib is not registered as a Git submodule: %s\n' \
            "$geographiclib_path" >&2
        return 1
    fi
    # Do not restore the old superproject-pinned development commit on every
    # run while the caller has not yet committed the newly selected gitlink.
    if [[ ! -e "$geographiclib_path/.git" ]]; then
        git submodule update --init -- "$geographiclib_path"
    fi
    git -C "$geographiclib_path" fetch --no-tags origin "tag" "$geographiclib_tag"
    git -C "$geographiclib_path" checkout --detach "refs/tags/$geographiclib_tag"

    local actual_version
    actual_version="$(cat "$geographiclib_path/VERSION")"
    if [[ "$actual_version" != "${geographiclib_tag#r}" ]]; then
        printf 'Unexpected GeographicLib VERSION: %s (expected %s)\n' \
            "$actual_version" "${geographiclib_tag#r}" >&2
        return 1
    fi
    printf 'GeographicLib pinned to stable %s. Commit the changed %s gitlink.\n' \
        "$geographiclib_tag" "$geographiclib_path"
}

update_model() {
    # Do not recurse into or update unrelated submodules when only MODEL is needed.
    git submodule update --init --remote -- "$model_path"
}

update_epanet() {
    # Follow AOWIS EPANET only. The --remote+--recursive combination would
    # advance its third-party dependencies (e.g. EPANET-MSX) unexpectedly.
    git submodule update --init --remote -- "$epanet_path"
}

update_render() {
    if [[ "$(git ls-files --stage -- "$render_path")" != '160000 '* ]]; then
        printf 'Missing Render Git submodule gitlink: %s\n' "$render_path" >&2
        printf 'Register with: git submodule add https://github.com/aowis-org/AOWIS-GUI-Render.git %s\n' "$render_path" >&2
        return 1
    fi
    git submodule sync -- "$render_path"
    git submodule update --init --remote --checkout -- "$render_path"
    # If Render acquires nested dependencies, use its pinned gitlinks.
    git -C "$render_path" submodule update --init --recursive --checkout
    "${PWD}/tools/verify_render_checkout.sh" "$render_path"
}

update_network() {
    git submodule sync -- "$network_path"
    git submodule update --init --remote --checkout -- "$network_path"
    git -C "$network_path" submodule sync --recursive
    git -C "$network_path" submodule update --init --recursive --checkout
}

update_config() {
    git submodule sync -- "$config_path"
    git submodule update --init --remote --checkout -- "$config_path"
}

update_epanet_deps() {
    update_epanet
    # Explicitly synchronize third-party dependencies to EPANET's recorded
    # gitlinks, not their upstream development branches.
    git -C "$epanet_path" submodule update --init --recursive --checkout
}

update_standard() {
    update_model
    update_epanet
    update_render
    update_network
    update_config
}

update_bgfx() {
    ensure_bgfx_submodules_registered
    # Never recursive: AOWIS needs none of bgfx's own submodules.
    git submodule update --init --remote --depth 1 -- "${bgfx_paths[@]}"
    tools/bgfx_sparse_checkout.sh
    printf 'bgfx updated. Regenerate the shader binaries: ./compile_linux.sh --shader-tools\n'
}

case "${1:-}" in
    '')
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        update_standard
        ;;
    --model)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        update_model
        ;;
    --epanet)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        update_epanet
        ;;
    --render)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        update_render
        ;;
    --network)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        update_network
        ;;
    --config)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        update_config
        ;;
    --epanet-deps)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        update_epanet_deps
        ;;
    --geographiclib)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        update_geographiclib
        ;;
    --bgfx)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        update_bgfx
        ;;
    --all)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        update_standard
        # --all explicitly includes third-party dependency synchronization.
        git -C "$epanet_path" submodule update --init --recursive --checkout
        update_geographiclib
        update_bgfx
        ;;
    --help|-h)
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        usage
        ;;
    *)
        printf 'Unknown option: %s\n' "$1" >&2
        usage >&2
        exit 2
        ;;
esac
