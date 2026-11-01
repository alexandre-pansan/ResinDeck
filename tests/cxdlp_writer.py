"""Builds small synthetic .cxdlpv4 files with the layout cxdlp.load() reads."""
import struct
import zlib

import numpy as np

from cxdlp import HEADER_TAIL, LAYER_DEF, LAYER_DEF_EX_SIZE, PREVIEW, PRINT_PARAMS

MAGIC = b"CXSW3DV2\0"


def rle7(img):
    """7-bit run-length encoding of a uint8 image (inverse of cxdlp._decode_rle7)."""
    flat = np.asarray(img, dtype=np.uint8).ravel() >> 1
    out = bytearray()
    i = 0
    while i < len(flat):
        code = int(flat[i])
        j = i
        while j < len(flat) and flat[j] == code:
            j += 1
        n = j - i
        if n == 1:
            out.append(code)
        else:
            out.append(code | 0x80)
            if n < 0x80:
                out.append(n)
            elif n < 0x4000:
                out += bytes([0x80 | n >> 8, n & 0xFF])
            elif n < 0x200000:
                out += bytes([0xC0 | n >> 16, (n >> 8) & 0xFF, n & 0xFF])
            else:
                out += bytes([0xE0 | n >> 24, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF])
        i = j
    return bytes(out)


def rgb15(img):
    """Preview encoding: RGB 5-5-5 words, bit 0x20 marks a run (inverse of preview_rgb)."""
    h, w, _ = img.shape
    px = img.reshape(-1, 3).astype(np.uint16)
    dots = ((px[:, 0] >> 3) << 11) | ((px[:, 1] >> 3) << 6) | (px[:, 2] >> 3)
    out = []
    i = 0
    while i < len(dots):
        j = i
        while j < len(dots) and dots[j] == dots[i] and j - i < 0x1000:
            j += 1
        if j - i == 1:
            out.append(int(dots[i]))
        else:
            out += [int(dots[i]) | 0x20, (j - i - 1) & 0xFFF]
        i = j
    return struct.pack(f"<{len(out)}H", *out)


def layer_images(res_x=60, res_y=40, count=12):
    """A box at the plate centre that gets one pixel wider every layer."""
    images = []
    for n in range(count):
        img = np.zeros((res_y, res_x), dtype=np.uint8)
        img[10:30, 20 - n // 2:40 + n // 2] = 255
        images.append(img)
    return images


def preview_image(w=16, h=12):
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[:, : w // 2] = (248, 0, 0)
    img[:, w // 2:] = (0, 0, 248)
    return img


def write_cxdlp(path, res=(60, 40), bed=(6.0, 4.0, 50.0), layer_height=0.05, layers=None,
                preview=None, magic=MAGIC, corrupt_crc=False, encrypted=False):
    rx, ry = res
    layers = layer_images(rx, ry) if layers is None else layers
    preview = preview_image() if preview is None else preview
    name = b"HALOT-R6\0"
    head = struct.pack(">I", len(magic)) + magic + struct.pack(">H", 4) + struct.pack(">I", len(name)) + name
    tail_at = len(head)

    body = bytearray(head + b"\0" * HEADER_TAIL.size)

    def place(data):
        off = len(body)
        body.extend(data)
        return off

    pw, ph = preview.shape[1], preview.shape[0]
    img = rgb15(preview)
    img_off = len(body) + PREVIEW.size
    prev_s = place(PREVIEW.pack(pw, ph, img_off, len(img), 0, 0, 0, 0) + img)
    prev_l = place(PREVIEW.pack(pw, ph, len(body) + PREVIEW.size, len(img), 0, 0, 0, 0) + img)

    params = (5.0, 60.0, 4.0, 90.0, 150.0, 1.25, 1.5, 0.1, 1.0, 0.5, 0, 2.5, 30.0, 0, 0, 0, 0)
    params_off = place(PRINT_PARAMS.pack(*params))

    table_off = len(body)
    body.extend(b"\0" * LAYER_DEF.size * len(layers))
    entries = []
    for n, image in enumerate(layers):
        data = b"\0" * LAYER_DEF_EX_SIZE + rle7(image)
        addr = place(data)
        entries.append(LAYER_DEF.pack((n + 1) * layer_height, 2.5, 0.5, addr, len(data), 0, 0, 0, 0, 0))
    body[table_off:table_off + LAYER_DEF.size * len(layers)] = b"".join(entries)

    tail = HEADER_TAIL.pack(rx, ry, *bed, len(layers) * layer_height, layer_height, 2, prev_s, table_off,
                            len(layers), prev_l, 1234, 0, params_off, PRINT_PARAMS.size, 4, 255, 255,
                            1 if encrypted else 0, 0, 0)
    body[tail_at:tail_at + HEADER_TAIL.size] = tail

    crc = zlib.crc32(bytes(body), 0xFFFFFFFF) ^ 0xFFFFFFFF
    if corrupt_crc:
        crc ^= 1
    body.extend(struct.pack(">I", crc))
    path.write_bytes(bytes(body))
    return path
