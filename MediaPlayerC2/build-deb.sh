#!/bin/bash
# Build a .deb package:   ./build-deb.sh [version]
set -euo pipefail

NAME=simple-mediaplayer
VERSION=${1:-1.0.0}
ARCH=$(dpkg --print-architecture)
ROOT="build/${NAME}_${VERSION}_${ARCH}"

# 1. compile
make clean
make

# 2. copy files into a fake root filesystem (binary -> /usr/bin, launcher -> /usr/share/applications)
rm -rf build
mkdir -p "$ROOT/DEBIAN"
make install DESTDIR="$ROOT" PREFIX=/usr
strip --strip-unneeded "$ROOT/usr/bin/mediaplayer"

# 3. package metadata
SIZE=$(du -sk "$ROOT/usr" | cut -f1)
cat > "$ROOT/DEBIAN/control" << CONTROL
Package: ${NAME}
Version: ${VERSION}
Section: video
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${SIZE}
Depends: libgtk-3-0 | libgtk-3-0t64, libgstreamer1.0-0, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-gtk3
Recommends: gstreamer1.0-plugins-bad, gstreamer1.0-plugins-ugly, gstreamer1.0-libav
Maintainer: Roshan <46742307+roshanlalanthaUOM@users.noreply.github.com>
Description: Simple GTK audio/video player
 A small media player built with GTK3 and GStreamer.
 Supports a playlist, seek bar, volume control, fullscreen
 and drag and drop.
CONTROL

# 4. build the package
dpkg-deb --root-owner-group --build "$ROOT"
mv "$ROOT.deb" .
echo
echo "Created: ${NAME}_${VERSION}_${ARCH}.deb"
echo "Install with: sudo apt install ./${NAME}_${VERSION}_${ARCH}.deb"
