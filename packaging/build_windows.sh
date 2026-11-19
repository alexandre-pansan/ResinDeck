#!/bin/sh
# Builds the Windows packages of the native application, in Git Bash or CI:
#   dist/ResinDeck-<version>-windows-x64.zip        (portable: unzip and run resindeck.exe)
#   dist/ResinDeck-<version>-windows-x64-setup.exe  (installer, when Inno Setup's iscc is on PATH)
# Needs CMake, Visual Studio 2022 and Qt for MSVC; QT_DIR points at Qt (e.g. C:/Qt/6.10.3/msvc2022_64).
set -eu
cd "$(dirname "$0")/.."
QT_DIR=${QT_DIR:?set QT_DIR to the Qt folder, e.g. C:/Qt/6.10.3/msvc2022_64}
VERSION=$(sed -n 's/^project(ResinDeck VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
BUILD=build/windows-release
OUT=build/windows/ResinDeck

cmake -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_DIR"
cmake --build "$BUILD" --config Release --target resindeck resindeck-cli
rm -rf build/windows
cmake --install "$BUILD" --config Release --prefix build/windows/install
mkdir -p "$OUT"
cp -r build/windows/install/bin/. "$OUT/"
# Qt's DLLs and plugins next to the executable
"$QT_DIR/bin/windeployqt" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "$OUT/resindeck.exe"

mkdir -p dist
rm -f "dist/ResinDeck-$VERSION-windows-x64.zip"
(cd build/windows && powershell -NoProfile -Command "Compress-Archive -Path ResinDeck -DestinationPath ../../dist/ResinDeck-$VERSION-windows-x64.zip")
if command -v iscc >/dev/null 2>&1; then
    # "-D", not "/D": Git Bash turns arguments that start with / into Windows paths
    iscc "-DVersion=$VERSION" packaging/resindeck.iss
fi
ls dist
