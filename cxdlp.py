"""Reader for Creality .cxdlpv4 sliced files (magic "CXSW3DV2").

Layout based on UVtools' CrealityCXDLPv4File.cs (AGPL, format description only).
"""
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

HEADER_TAIL = struct.Struct("<HHfffffIIIIIIIIIIHHIII")
PREVIEW = struct.Struct("<8I")
PRINT_PARAMS = struct.Struct("<10fI2f4I")
LAYER_DEF = struct.Struct("<3f7I")
LAYER_DEF_EX_SIZE = 44


@dataclass
class Layer:
    z: float
    exposure: float
    light_off: float
    address: int
    size: int
    data_type: int


@dataclass
class CXDLP:
    path: Path
    printer: str
    res_x: int
    res_y: int
    bed: tuple
    print_height: float
    layer_height: float
    bottom_layers: int
    print_time: int
    anti_alias: int
    encryption_key: int
    params: dict = field(default_factory=dict)
    previews: list = field(default_factory=list)  # (w, h, offset, length)
    layers: list = field(default_factory=list)
    pixel_pair: bool = False

    @property
    def pixel_mm(self):
        return self.bed[0] / self.res_x, self.bed[1] / self.res_y

    def info(self):
        return {
            "file": self.path.name,
            "size": self.path.stat().st_size,
            "printer": self.printer,
            "resolution": [self.res_x, self.res_y],
            "bed": [round(b, 2) for b in self.bed],
            "printHeight": round(self.print_height, 3),
            "layerHeight": round(self.layer_height, 4),
            "layerCount": len(self.layers),
            "bottomLayers": self.bottom_layers,
            "printTime": self.print_time,
            "antiAlias": self.anti_alias,
            "previews": len(self.previews),
            **{k: round(v, 3) if isinstance(v, float) else v for k, v in self.params.items()},
        }

    # --- images -----------------------------------------------------------

    def preview_rgb(self, index=-1):
        """Decode a preview (Chitu RGB15 RLE) into an (h, w, 3) uint8 array."""
        w, h, off, length = self.previews[index]
        with open(self.path, "rb") as f:
            f.seek(off)
            raw = f.read(length)
        words = np.frombuffer(raw[: len(raw) // 2 * 2], dtype="<u2")
        out = np.zeros(w * h, dtype=np.uint16)
        pos = i = 0
        n = len(words)
        while i < n and pos < w * h:
            dot = int(words[i])
            i += 1
            rep = 1
            if dot & 0x20 and i < n:
                rep += int(words[i]) & 0xFFF
                i += 1
            out[pos:pos + rep] = dot
            pos += rep
        r = ((out >> 11) & 0x1F) << 3
        g = ((out >> 6) & 0x1F) << 3
        b = (out & 0x1F) << 3
        return np.stack([r, g, b], axis=-1).astype(np.uint8).reshape(h, w, 3)

    def layer_rle(self, index, f=None):
        L = self.layers[index]
        own = f is None
        f = f or open(self.path, "rb")
        try:
            f.seek(L.address + LAYER_DEF_EX_SIZE)
            data = f.read(L.size - LAYER_DEF_EX_SIZE)
        finally:
            if own:
                f.close()
        if self.encryption_key:
            raise NotImplementedError("encrypted layers are not supported")
        return data

    def layer_image(self, index, f=None):
        """Decode one layer into an (res_y, res_x) uint8 array."""
        rle = self.layer_rle(index, f)
        n = self.res_x * self.res_y
        if self.pixel_pair:
            return _decode_pixel_pair(rle, self.res_x, self.res_y)
        return _decode_rle7(rle, n).reshape(self.res_y, self.res_x)


    def voxels(self, step=6, on_progress=None):
        """Downsample the layers into a boolean (z, y, x) grid cropped to the model.

        Returns (grid, meta) where meta has the voxel size and origin in mm.
        """
        k = max(1, round(step * self.pixel_mm[0] / max(self.layer_height, 1e-3)))
        ry, rx = self.res_y // step * step, self.res_x // step * step
        idx = range(0, len(self.layers), k)
        vol = np.zeros((len(idx), ry // step, rx // step), dtype=bool)
        with open(self.path, "rb") as f:
            for j, i in enumerate(idx):
                img = self.layer_image(i, f)[:ry, :rx]
                vol[j] = (img.reshape(ry // step, step, rx // step, step) > 127).any(axis=(1, 3))
                if on_progress and j % 20 == 0:
                    on_progress(j / len(idx))
        nz = np.argwhere(vol)
        if not len(nz):
            return vol[:0, :0, :0], {}
        lo, hi = nz.min(0), nz.max(0) + 1
        grid = vol[lo[0]:hi[0], lo[1]:hi[1], lo[2]:hi[2]]
        px, py = self.pixel_mm
        meta = {
            "shape": list(grid.shape),  # z, y, x
            "voxel": [step * px, step * py, k * self.layer_height],  # x, y, z in mm
            "origin": [lo[2] * step * px, lo[1] * step * py, lo[0] * k * self.layer_height],
            "layersPerVoxel": k,
            "bed": list(self.bed),
        }
        return grid, meta


def _decode_rle7(rle, n):
    out = np.zeros(n, dtype=np.uint8)
    pos = i = 0
    ln = len(rle)
    while i < ln:
        code = rle[i]
        i += 1
        stride = 1
        if code & 0x80:
            code &= 0x7F
            s = rle[i]
            if not s & 0x80:
                stride = s
                i += 1
            elif s & 0xC0 == 0x80:
                stride = ((s & 0x3F) << 8) | rle[i + 1]
                i += 2
            elif s & 0xE0 == 0xC0:
                stride = ((s & 0x1F) << 16) | (rle[i + 1] << 8) | rle[i + 2]
                i += 3
            else:
                stride = ((s & 0x0F) << 24) | (rle[i + 1] << 16) | (rle[i + 2] << 8) | rle[i + 3]
                i += 4
        if code:
            out[pos:pos + stride] = (code << 1) | 1
        pos += stride
    return out


def _gray3(g):
    return (g << 5) | (g << 2) | (g >> 1)


def _decode_pixel_pair(rle, w, h):
    out = np.zeros(w * h, dtype=np.uint8)
    unit = i = 0
    ln = len(rle)
    while i < ln:
        code = rle[i]
        i += 1
        rep = 0
        while i < ln and not rle[i] & 0x80:
            rep = (rep << 7) | rle[i]
            i += 1
        count = rep + 1
        left, right = _gray3(code & 7), _gray3((code >> 3) & 7)
        if left or right:
            out[unit * 2:(unit + count) * 2:2] = left
            out[unit * 2 + 1:(unit + count) * 2:2] = right
        unit += count
    return out.reshape(h, w)


def _is_pixel_pair(rle, w, h):
    if w % 2:
        return False
    per_row, total = w // 2, w // 2 * h
    unit = i = 0
    ln = len(rle)
    while i < ln:
        code = rle[i]
        i += 1
        if not code & 0x80 or code & 0x40 or unit >= total:
            return False
        remaining = per_row - unit % per_row
        rep = 0
        while i < ln and not rle[i] & 0x80:
            rep = (rep << 7) | rle[i]
            i += 1
            if rep >= remaining:
                return False
        unit += rep + 1
    return unit == total


def load(path) -> CXDLP:
    path = Path(path)
    with open(path, "rb") as f:
        magic_size = struct.unpack(">I", f.read(4))[0]
        magic = f.read(magic_size).rstrip(b"\0").decode("ascii", "replace")
        if not magic.startswith("CXSW3D"):
            raise ValueError(f"not a CXDLPv4 file ({magic!r})")
        struct.unpack(">H", f.read(2))  # version
        name_len = struct.unpack(">I", f.read(4))[0]
        printer = f.read(name_len).rstrip(b"\0").decode("ascii", "replace")
        (rx, ry, bx, by, bz, ph, lh, bottom, prev_s, layers_off, layer_count,
         prev_l, ptime, _proj, params_off, _params_size, aa, _pwm, _bpwm,
         key, _slicer_off, _slicer_size) = HEADER_TAIL.unpack(f.read(HEADER_TAIL.size))

        c = CXDLP(path, printer, rx, ry, (bx, by, bz), ph, lh, bottom, ptime, aa, key)

        for off in (prev_s, prev_l):
            if off:
                f.seek(off)
                w, h, img_off, img_len, *_ = PREVIEW.unpack(f.read(PREVIEW.size))
                c.previews.append((w, h, img_off, img_len))

        if params_off:
            f.seek(params_off)
            p = PRINT_PARAMS.unpack(f.read(PRINT_PARAMS.size))
            c.params = {
                "bottomLiftHeight": p[0], "bottomLiftSpeed": p[1],
                "liftHeight": p[2], "liftSpeed": p[3], "retractSpeed": p[4],
                "volumeMl": p[5], "weightG": p[6], "cost": p[7],
                "bottomLightOff": p[8], "lightOff": p[9],
                "exposure": p[11], "bottomExposure": p[12],
            }

        f.seek(layers_off)
        table = f.read(LAYER_DEF.size * layer_count)
        for z, exp, off_s, addr, size, dtype, *_ in LAYER_DEF.iter_unpack(table):
            c.layers.append(Layer(z, exp, off_s, addr, size, dtype))

        if c.layers and not key:
            c.pixel_pair = _is_pixel_pair(c.layer_rle(0, f), rx, ry)
    return c


def verify_checksum(path) -> bool:
    data = Path(path).read_bytes()
    crc = zlib.crc32(data[:-4], 0xFFFFFFFF) ^ 0xFFFFFFFF
    return crc == struct.unpack(">I", data[-4:])[0]


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
