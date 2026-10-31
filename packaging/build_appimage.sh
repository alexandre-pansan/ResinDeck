#!/bin/sh
# Builds dist/ResinDeck-<version>-x86_64.AppImage. Needs the packages in requirements-desktop.txt.
# Usage: packaging/build_appimage.sh [python]   (default: .venv/bin/python)
set -eu
cd "$(dirname "$0")/.."
PY=${1:-.venv/bin/python}
VERSION=$("$PY" -c 'import re; print(re.search(r"__version__ = \"(.+)\"", open("resindeck.py").read())[1])')

"$PY" -m PyInstaller --noconfirm --distpath build/dist --workpath build/work packaging/resindeck.spec

APPDIR=build/ResinDeck.AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
cp -r build/dist/resindeck/. "$APPDIR/usr/bin/"
cp packaging/icon.png "$APPDIR/resindeck.png"
cat > "$APPDIR/resindeck.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ResinDeck
Comment=Preview, send and monitor resin 3D prints
Exec=resindeck
Icon=resindeck
Categories=Graphics;3DGraphics;Utility;
Terminal=false
EOF
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/resindeck" "$@"
EOF
chmod +x "$APPDIR/AppRun"

TOOL=build/tools/appimagetool-x86_64.AppImage
if [ ! -x "$TOOL" ]; then
    mkdir -p build/tools
    curl -sSfL -o "$TOOL" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$TOOL"
fi

mkdir -p dist
OUT="dist/ResinDeck-$VERSION-x86_64.AppImage"
# extract-and-run avoids needing FUSE on the build machine
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$TOOL" --no-appstream "$APPDIR" "$OUT"
echo "Built $OUT"
