#!/usr/bin/env bash
# Package build/src/game/mm_game as an AppImage. Adapted from the reference
# project's .github/linux/appimage.sh (Zelda64Recomp).
#
# Run from the repository root after building:
#   ./.github/linux/appimage.sh [path/to/mm_game]
#
# Notes:
#  - linuxdeploy and its GTK plugin are downloaded on first use; the GTK
#    plugin bundles the libraries the nativefiledialog file picker needs.
#  - The resulting AppImage only runs on glibc >= the build machine's:
#    package release builds on the oldest supported distro.
#  - Portable mode: put a portable.txt next to the AppImage and config/saves
#    are kept there (AppRun forwards the directory via APP_FOLDER_PATH).
#  - On bleeding-edge hosts (Arch etc.) run with NO_STRIP=1: linuxdeploy's
#    bundled strip predates .relr.dyn sections and dies on modern system libs.
set -euo pipefail

BINARY="${1:-build/src/game/mm_game}"
APP_NAME="MischiefMakersRecompiled"

if [ ! -x "$BINARY" ]; then
  echo "error: $BINARY not found or not executable (build mm_game first)" >&2
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
[ -f linuxdeploy-plugin-gtk.sh ] || \
  curl -sSfLO "https://github.com/linuxdeploy/linuxdeploy-plugin-gtk/raw/master/linuxdeploy-plugin-gtk.sh"
chmod a+x linuxdeploy*

rm -rf AppDir
mkdir -p AppDir/usr/bin
cp "$BINARY" AppDir/usr/bin/
cp icons/512.png "AppDir/$APP_NAME.png"
cp ".github/linux/$APP_NAME.desktop" AppDir/

# Extract linuxdeploy (running the AppImage directly requires FUSE; extracting
# works everywhere, including containers/CI).
"./linuxdeploy-$LINUX_DEPLOY_ARCH.AppImage" --appimage-extract > /dev/null
rm -rf deploy && mv squashfs-root deploy

./deploy/AppRun --appdir=AppDir/ \
  -d "AppDir/$APP_NAME.desktop" \
  -i "AppDir/$APP_NAME.png" \
  -e AppDir/usr/bin/mm_game \
  --plugin gtk

# Replace the generated exec line with a portable-mode-aware launch:
# a portable.txt next to the AppImage keeps config/saves in that directory
# (mm::get_app_folder_path honors APP_FOLDER_PATH — see src/game/app_dirs.cpp).
sed -i 's/^exec/#exec/' AppDir/AppRun
cat >> AppDir/AppRun <<'EOF'
if [ -f "portable.txt" ]; then
    APP_FOLDER_PATH=$PWD
    cd "$this_dir"/usr/bin/
    APP_FOLDER_PATH=$APP_FOLDER_PATH exec ./mm_game "$@"
else
    cd "$this_dir"/usr/bin/
    exec ./mm_game "$@"
fi
EOF

# Same conflict-prone libraries the reference strips: host GIO modules and
# wayland client libs bundled by the GTK plugin break on newer distros.
rm -rf AppDir/usr/lib/libgmodule*
rm -rf AppDir/usr/lib/gio/modules/*.so
rm -rf AppDir/usr/lib/libwayland*

OUTPUT="$APP_NAME-$ARCH.AppImage" ./deploy/usr/bin/linuxdeploy-plugin-appimage --appdir=AppDir/
echo "AppImage built:"
ls -la "$APP_NAME-$ARCH.AppImage"
