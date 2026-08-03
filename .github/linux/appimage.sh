#!/usr/bin/env bash
# Package build/src/game/troublemakers as an AppImage. Adapted from the reference
# project's .github/linux/appimage.sh (Zelda64Recomp).
#
# Run from the repository root after building:
#   ./.github/linux/appimage.sh [path/to/troublemakers]
#   ./.github/linux/appimage.sh --steam-deck [path/to/troublemakers]
#
# Notes:
#  - linuxdeploy is downloaded on first use. No GTK plugin: the file picker
#    (nativefiledialog-extended) is built with NFD_PORTAL, so dialogs go
#    through the host's xdg-desktop-portal over D-Bus and no GTK/gdk-pixbuf
#    stack is bundled (bundled pixbuf used to crash on host SVG icon themes).
#  - The resulting AppImage only runs on glibc >= the build machine's:
#    package release builds on the oldest supported distro.
#  - Portable mode: put a portable.txt next to the AppImage and config/saves
#    are kept there (AppRun forwards the directory via APP_FOLDER_PATH).
#  - On bleeding-edge hosts (Arch etc.) run with NO_STRIP=1: linuxdeploy's
#    bundled strip predates .relr.dyn sections and dies on modern system libs.
set -euo pipefail

STEAM_DECK_BUILD=0
if [ "${1:-}" = "--steam-deck" ]; then
  STEAM_DECK_BUILD=1
  shift
fi
if [ "$#" -gt 1 ]; then
  echo "usage: $0 [--steam-deck] [path/to/troublemakers]" >&2
  exit 1
fi

BINARY="${1:-build/src/game/troublemakers}"
PACKAGE_NAME="TroubleMakers"
if [ "$STEAM_DECK_BUILD" -eq 1 ]; then
  PACKAGE_NAME="TroubleMakers-SteamDeck"
fi

if [ ! -x "$BINARY" ]; then
  echo "error: $BINARY not found or not executable (build troublemakers first)" >&2
  exit 1
fi

ARCH=$(uname -m)
case "$ARCH" in
  x86_64)  LINUX_DEPLOY_ARCH="x86_64" ;;
  aarch64) LINUX_DEPLOY_ARCH="aarch64" ;;
  *) echo "Unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

[ -f "linuxdeploy-$LINUX_DEPLOY_ARCH.AppImage" ] || \
  curl -sSfLO "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$LINUX_DEPLOY_ARCH.AppImage"
chmod a+x linuxdeploy*

rm -rf AppDir
mkdir -p AppDir/usr/bin
cp "$BINARY" AppDir/usr/bin/
cp -R assets AppDir/usr/bin/
cp icons/512.png "AppDir/$PACKAGE_NAME.png"
cp ".github/linux/$PACKAGE_NAME.desktop" AppDir/
if [ "$STEAM_DECK_BUILD" -eq 1 ]; then
  mkdir -p AppDir/usr/share/troublemakers
  : > AppDir/usr/share/troublemakers/steam-deck
fi

# Extract linuxdeploy (running the AppImage directly requires FUSE; extracting
# works everywhere, including containers/CI).
"./linuxdeploy-$LINUX_DEPLOY_ARCH.AppImage" --appimage-extract > /dev/null
rm -rf deploy && mv squashfs-root deploy

./deploy/AppRun --appdir=AppDir/ \
  -d "AppDir/$PACKAGE_NAME.desktop" \
  -i "AppDir/$PACKAGE_NAME.png" \
  -e AppDir/usr/bin/troublemakers

# linuxdeploy may generate AppRun as a symlink to the game executable when no
# runtime hooks are needed. Always replace it with a real portable-mode-aware
# launcher instead of trying to edit what may be an ELF binary through a symlink.
rm -f AppDir/AppRun
cat > AppDir/AppRun <<'EOF'
#!/bin/sh
set -e

this_dir="$(readlink -f "$(dirname "$0")")"
if [ -n "${APPIMAGE:-}" ]; then
    portable_dir="$(dirname "$(readlink -f "$APPIMAGE")")"
else
    portable_dir="$PWD"
fi

# The bundled classic SDL2 mishandles HiDPI on the native Wayland backend
# (tiny UI, mouse clicks land offset from what's drawn). Default to x11 so the
# compositor does the scaling via XWayland — consistent and correct. Power
# users can still force native Wayland with SDL_VIDEODRIVER=wayland.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"

# The dedicated Deck artifact follows RecompFrontend's SteamDeck=1 detection
# path even when built/tested away from Valve hardware. It must still be added
# to Steam and launched from Gaming Mode so Steam selects its gamepad profile
# instead of the Desktop keyboard/mouse profile.
if [ -f "$this_dir/usr/share/troublemakers/steam-deck" ]; then
    export SteamDeck=1
    set -- --steam-deck "$@"
fi

cd "$this_dir"/usr/bin/
if [ -f "$portable_dir/portable.txt" ]; then
    APP_FOLDER_PATH="$portable_dir" exec ./troublemakers "$@"
else
    exec ./troublemakers "$@"
fi
EOF
chmod a+x AppDir/AppRun

# Bundled wayland client libs break on newer distros (SDL dlopens the host's
# at runtime anyway); strip them if linuxdeploy pulled any in.
rm -rf AppDir/usr/lib/libwayland*

OUTPUT="$PACKAGE_NAME-$ARCH.AppImage" ./deploy/usr/bin/linuxdeploy-plugin-appimage --appdir=AppDir/
echo "AppImage built:"
ls -la "$PACKAGE_NAME-$ARCH.AppImage"
