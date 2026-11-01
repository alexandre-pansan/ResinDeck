"""Readers for sliced files, picked by file extension.

A reader module exposes `load(path)`, returning an object with:
  info() -> dict                         header values shown in the interface
  layers                                 list of objects with a `z` attribute (mm)
  previews                               list (may be empty)
  preview_rgb(index) -> (h, w, 3) uint8  thumbnail
  layer_image(n) -> (h, w) uint8         one layer, 0 = no light, 255 = full exposure
  voxels(on_progress=None) -> (grid, meta)  boolean (z, y, x) grid for the 3D view
formats/cxdlp.py is the reference. To add a format, write the module and list its extensions
in READERS below.
"""
import struct
import zlib
from pathlib import Path

import numpy as np

from formats import cxdlp

READERS = {".cxdlpv4": cxdlp, ".cxdlp": cxdlp}
EXTENSIONS = tuple(READERS)


def supported(path):
    return Path(path).suffix.lower() in READERS


def load(path):
    """Read a sliced file; ValueError if it is not a supported format."""
    reader = READERS.get(Path(path).suffix.lower())
    if reader is None:
        raise ValueError(f"unsupported file type: {Path(path).suffix}")
    return reader.load(path)


def png_bytes(img: np.ndarray) -> bytes:
    """Minimal PNG encoder for gray (h, w) or RGB (h, w, 3) uint8 arrays."""
    h, w = img.shape[:2]
    color = 2 if img.ndim == 3 else 0
    raw = np.zeros((h, img[0].size + 1), dtype=np.uint8)
    raw[:, 1:] = img.reshape(h, -1)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw.tobytes(), 6))
            + chunk(b"IEND", b""))
