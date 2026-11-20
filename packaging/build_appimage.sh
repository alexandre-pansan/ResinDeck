#!/bin/sh
# Builds dist/ResinDeck-<version>-x86_64.AppImage from the native application (C++ and Qt).
# Needs CMake, Ninja and Qt; QT_DIR points at Qt (default ~/Qt/6.10.3/gcc_64, the version CI uses).
# linuxdeploy and its Qt plugin are downloaded into build/tools the first time.
set -eu
cd "$(dirname "$0")/.."
QT_DIR=${QT_DIR:-$HOME/Qt/6.10.3/gcc_64}
VERSION=$(sed -n 's/^project(ResinDeck VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
BUILD=build/native-release
APPDIR=build/ResinDeck-native.AppDir

cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_DIR"
cmake --build "$BUILD" --target resindeck resindeck-cli
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr

TOOLS=build/tools
mkdir -p "$TOOLS"
for tool in linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
            linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage; do
    file="$TOOLS/$(basename "$tool")"
    if [ ! -x "$file" ]; then
        curl -sSfL -o "$file" "https://github.com/$tool"
        chmod +x "$file"
    fi
done

# the Qt plugin finds Qt through qmake; Wayland is added next to the default X11 (xcb) plugin
export QMAKE="$QT_DIR/bin/qmake"
export LD_LIBRARY_PATH="$QT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
export EXTRA_QT_MODULES="waylandclient"
export APPIMAGE_EXTRACT_AND_RUN=1  # the tools are AppImages too: run them without FUSE (CI containers)

# OpenGL on Wayland (the 3D view) needs the wayland-egl plugin, which the Qt plugin leaves out;
# linuxdeploy brings in what it needs (libQt6OpenGL)
GL_PLUGINS="$APPDIR/usr/plugins/wayland-graphics-integration-client"
mkdir -p "$GL_PLUGINS"
cp "$QT_DIR/plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so" "$GL_PLUGINS/"

mkdir -p dist
OUTPUT="dist/ResinDeck-$VERSION-x86_64.AppImage" "$TOOLS/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" --plugin qt --output appimage --deploy-deps-only "$GL_PLUGINS" \
    --desktop-file "$APPDIR/usr/share/applications/resindeck.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/512x512/apps/resindeck.png"
echo "dist/ResinDeck-$VERSION-x86_64.AppImage"
