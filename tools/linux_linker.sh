#!/usr/bin/env bash
# Shared native Linux linker selection for GUI, tests, and benchmark builds.
# GNU ld has crashed linking the large desktop GUI; prefer LLD by default.
# Sourced by the native Linux compile scripts (not by WASM/Docker scripts).

aowis_linux_executable_linker_flags()
{
    case "${AOWIS_LINUX_LINKER:-lld}" in
        lld)
            if ! command -v ld.lld >/dev/null 2>&1; then
                printf '%s\n' 'ERROR: LLD is the default native Linux linker, but ld.lld is missing.' >&2
                printf '%s\n' 'Install LLD, or set AOWIS_LINUX_LINKER=system to use the toolchain default.' >&2
                return 1
            fi
            printf '%s' '-fuse-ld=lld'
            ;;
        system)
            # Pass an explicit empty flag to CMake, clearing a previous cached
            # -fuse-ld=lld value when reusing the same build directory.
            printf '%s' ''
            ;;
        *)
            printf 'ERROR: unsupported AOWIS_LINUX_LINKER=%s (choose lld or system).\n' "${AOWIS_LINUX_LINKER}" >&2
            return 2
            ;;
    esac
}
