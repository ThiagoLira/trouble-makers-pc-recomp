#!/usr/bin/env bash
# Build and run Trouble Makers.
#
#   ./run.sh                          build, then open the launcher (pick a ROM)
#   ./run.sh path/to/rom.z64          build, then boot straight into that ROM
#   ./run.sh rom.z64 --fps 240 --widescreen --window 1920x1080
#                                     ...plus any game option (see --help)
#
# Environment overrides:
#   TM_ROM=path/to/rom.z64   default ROM used when none is passed on the CLI
#   BUILD_DIR=build          build directory (default: build)
#   BUILD_TYPE=Release       CMake build type
#   JOBS=N                   parallel build jobs (default: all cores)
#
# One-time setup (generating the translated code + cloning RT64) is NOT done
# here — see the README's "One-time setup" section. This script only
# configures/builds the `troublemakers` target and launches it.
set -euo pipefail

# Always operate from the repository root (this script's directory).
cd "$(dirname "$(readlink -f "$0")")"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# Fail early with a clear message if the one-time setup hasn't been run, rather
# than a confusing CMake error deep in the build.
missing=""
[ -d RecompiledFuncs ]              || missing="$missing RecompiledFuncs/ (run N64Recomp)"
[ -f src/rsp/generated/aspMain.cpp ] || missing="$missing src/rsp/generated/aspMain.cpp (run RSPRecomp)"
[ -f lib/rt64/CMakeLists.txt ]      || missing="$missing lib/rt64/ (clone + patch RT64)"
[ -f input/troublemakers.elf ]      || missing="$missing input/troublemakers.elf (from the decomp build)"
if [ -n "$missing" ]; then
    echo "run.sh: one-time setup is incomplete — missing:$missing" >&2
    echo "run.sh: see the 'One-time setup' section in README.md." >&2
    exit 1
fi

# (Re)configure when there is no cache, or when the cache belongs to a
# different source tree — a moved/renamed checkout leaves a stale CMakeCache
# that refuses to build ("directory ... is different than ..."). Wipe and
# reconfigure in that case.
configure=1
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cached_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" || true)"
    if [ "$cached_home" = "$PWD" ]; then
        configure=0
    else
        echo "run.sh: stale CMake cache in '$BUILD_DIR' (was '$cached_home') — reconfiguring." >&2
        rm -rf "$BUILD_DIR"
    fi
fi
if [ "$configure" = 1 ]; then
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

cmake --build "$BUILD_DIR" --target troublemakers -j "$JOBS"

game="$BUILD_DIR/src/game/troublemakers"

# Default to XWayland. The native Wayland backend (via SDL/libdecor) both
# mishandles HiDPI (tiny launcher, offset clicks) and crashes in libdecor-gtk
# on window move/resize. XWayland sidesteps both. Overridable:
# SDL_VIDEODRIVER=wayland ./run.sh ...
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"

# No ROM on the command line? Fall back to $TM_ROM if set; otherwise launch with
# no arguments and let the in-app launcher handle ROM selection.
if [ "$#" -eq 0 ] && [ -n "${TM_ROM:-}" ]; then
    set -- "$TM_ROM"
fi

echo "run.sh: launching $game $*"
exec "$game" "$@"
