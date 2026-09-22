#!/usr/bin/env bash
# build-appimage.sh
#
# Builds a portable AppImage for simplebrowser using linuxdeploy.
# Run this on an Ubuntu machine (must have the same build deps as
# building the app normally, plus internet access to fetch linuxdeploy).
#
# Result: simplebrowser-x86_64.AppImage in this directory.

set -euo pipefail

APP=simplebrowser
APPDIR="AppDir"
TOOLS_DIR="appimage-tools"

echo "==> 1. Building the app"
make clean || true
make

echo "==> 2. Fetching linuxdeploy + plugins (cached in $TOOLS_DIR)"
mkdir -p "$TOOLS_DIR"
cd "$TOOLS_DIR"

fetch_if_missing() {
    local file="$1" url="$2"
    if [ ! -f "$file" ]; then
        echo "    downloading $file"
        wget -q "$url" -O "$file"
        chmod +x "$file"
    fi
}

fetch_if_missing "linuxdeploy-x86_64.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"

fetch_if_missing "linuxdeploy-plugin-gtk.sh" \
    "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"

cd ..

echo "==> 3. Assembling AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

cp "$APP" "$APPDIR/usr/bin/"
cp "$APP.desktop" "$APPDIR/usr/share/applications/"
cp "$APP.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/"
# AppImage tooling also looks for the icon at the AppDir root
cp "$APP.png" "$APPDIR/$APP.png"
cp "$APP.desktop" "$APPDIR/$APP.desktop"

echo "==> 4. Running linuxdeploy (bundles GTK, WebKit, and shared libs)"
export DEPLOY_GTK_VERSION=3
chmod +x "$TOOLS_DIR/linuxdeploy-plugin-gtk.sh"
export PATH="$PWD/$TOOLS_DIR:$PATH"

"$TOOLS_DIR/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" \
    --plugin gtk \
    --output appimage \
    --desktop-file "$APPDIR/$APP.desktop" \
    --icon-file "$APPDIR/$APP.png" \
    --executable "$APPDIR/usr/bin/$APP"

echo "==> Done. Look for ${APP}-x86_64.AppImage in this directory."
echo "    Run it with: ./${APP}-x86_64.AppImage"
