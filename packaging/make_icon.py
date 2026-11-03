"""Draws the ResinDeck icon: a stack of cured layers above a resin vat.

Writes packaging/icon.png (512 px), packaging/icon.ico (Windows) and static/favicon.png.
Run: .venv/bin/python packaging/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
BG = (25, 29, 35)
ACCENT = (59, 140, 255)
ACCENT_DARK = (42, 111, 214)
VAT = (139, 148, 163)


def draw(size=512):
    s = size / 512
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=112 * s, fill=BG)

    # layers, narrowing towards the top like a printed part
    widths = [300, 268, 236, 204, 172]
    y = 330
    for i, w in enumerate(widths):
        x0 = (512 - w) / 2
        color = ACCENT if i % 2 == 0 else ACCENT_DARK
        d.rounded_rectangle([x0 * s, (y - 34) * s, (x0 + w) * s, y * s], radius=10 * s, fill=color)
        y -= 46

    # vat below the part
    d.rounded_rectangle([96 * s, 398 * s, 416 * s, 420 * s], radius=11 * s, fill=VAT)
    d.rounded_rectangle([96 * s, 346 * s, 118 * s, 420 * s], radius=11 * s, fill=VAT)
    d.rounded_rectangle([394 * s, 346 * s, 416 * s, 420 * s], radius=11 * s, fill=VAT)
    return img


def main():
    big = draw(512)
    big.save(ROOT / "packaging" / "icon.png")
    sizes = [(s, s) for s in (16, 24, 32, 48, 64, 128, 256)]
    big.save(ROOT / "packaging" / "icon.ico", sizes=sizes)
    draw(64).save(ROOT / "static" / "favicon.png")


if __name__ == "__main__":
    main()
