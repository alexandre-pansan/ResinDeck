# PyInstaller spec for the ResinDeck desktop app.
# Linux: a folder that build_appimage.sh turns into an AppImage. Windows: a single .exe.
# Run from the repository root: pyinstaller packaging/resindeck.spec
import sys
from pathlib import Path

root = Path(SPECPATH).parent
windows = sys.platform == "win32"

a = Analysis(
    [str(root / "desktop.py")],
    pathex=[str(root)],
    datas=[
        (str(root / "static"), "static"),
        (str(root / "packaging" / "icon.png"), "packaging"),
    ],
    hiddenimports=["pystray._win32"] if windows else ["pystray._xorg"],
    excludes=["tkinter"],
)
pyz = PYZ(a.pure)

if windows:
    exe = EXE(
        pyz, a.scripts, a.binaries, a.datas,
        name="ResinDeck",
        icon=str(root / "packaging" / "icon.ico"),
        console=False,
        upx=False,
    )
else:
    exe = EXE(pyz, a.scripts, exclude_binaries=True, name="resindeck", console=True, upx=False)
    coll = COLLECT(exe, a.binaries, a.datas, name="resindeck", upx=False)
