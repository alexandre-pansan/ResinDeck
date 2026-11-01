import struct
import zlib

import numpy as np
import pytest

import cxdlp
from cxdlp_writer import layer_images, preview_image, write_cxdlp


def test_load_header(sliced):
    c = cxdlp.load(sliced)
    assert c.printer == "HALOT-R6"
    assert (c.res_x, c.res_y) == (60, 40)
    assert c.bed == pytest.approx((6.0, 4.0, 50.0))
    assert c.layer_height == pytest.approx(0.05)
    assert len(c.layers) == 12
    assert c.layers[-1].z == pytest.approx(0.6)
    assert c.pixel_mm == pytest.approx((0.1, 0.1))
    assert not c.pixel_pair


def test_info(sliced):
    info = cxdlp.load(sliced).info()
    assert info["file"] == "part.cxdlpv4"
    assert info["layerCount"] == 12
    assert info["printTime"] == 1234
    assert info["antiAlias"] == 4
    assert info["exposure"] == pytest.approx(2.5)
    assert info["bottomExposure"] == pytest.approx(30.0)
    assert info["volumeMl"] == pytest.approx(1.25)
    assert info["previews"] == 2


def test_preview_roundtrip(sliced):
    img = cxdlp.load(sliced).preview_rgb()
    assert np.array_equal(img, preview_image())


@pytest.mark.parametrize("n", [0, 5, 11])
def test_layer_roundtrip(sliced, n):
    expected = layer_images()[n]
    got = cxdlp.load(sliced).layer_image(n)
    assert got.shape == (40, 60)
    assert np.array_equal(got > 127, expected > 127)


def test_long_runs_use_multibyte_strides(tmp_path):
    # 600x400 = 240 000 black pixels in one run: needs the 3-byte stride form
    blank = [np.zeros((400, 600), dtype=np.uint8)]
    big = [np.zeros((400, 600), dtype=np.uint8)]
    big[0][100:300, 200:400] = 255
    for layers in (blank, big):
        c = cxdlp.load(write_cxdlp(tmp_path / "big.cxdlpv4", res=(600, 400), bed=(60.0, 40.0, 50.0), layers=layers))
        assert np.array_equal(c.layer_image(0) > 127, layers[0] > 127)


def test_voxels(sliced):
    grid, meta = cxdlp.load(sliced).voxels(step=2)
    assert grid.dtype == bool and grid.any()
    assert meta["shape"] == list(grid.shape)
    assert meta["voxel"][:2] == pytest.approx([0.2, 0.2])
    assert meta["bed"] == pytest.approx([6.0, 4.0, 50.0])
    # the box starts at x = 20 px - growth, y = 10 px
    assert meta["origin"][1] == pytest.approx(1.0)


def test_voxels_of_an_empty_file(tmp_path):
    empty = [np.zeros((40, 60), dtype=np.uint8)] * 3
    grid, meta = cxdlp.load(write_cxdlp(tmp_path / "e.cxdlpv4", layers=empty)).voxels(step=2)
    assert grid.size == 0 and meta == {}


def test_voxels_reports_progress(sliced):
    seen = []
    cxdlp.load(sliced).voxels(step=2, on_progress=seen.append)
    assert seen and all(0 <= p < 1 for p in seen)


def test_not_a_cxdlp(tmp_path):
    with pytest.raises(ValueError):
        cxdlp.load(write_cxdlp(tmp_path / "x.cxdlpv4", magic=b"NOTSLICED\0"))


def test_encrypted_layers_are_refused(tmp_path):
    c = cxdlp.load(write_cxdlp(tmp_path / "enc.cxdlpv4", encrypted=True))
    with pytest.raises(NotImplementedError):
        c.layer_image(0)


def test_checksum(tmp_path, sliced):
    assert cxdlp.verify_checksum(sliced)
    assert not cxdlp.verify_checksum(write_cxdlp(tmp_path / "bad.cxdlpv4", corrupt_crc=True))


def test_pixel_pair_decoding():
    # 4x1 image = 2 units of two pixels; code 0x80 | right << 3 | left, then a run byte
    rle = bytes([0x80 | (7 << 3) | 0, 0x00, 0x80 | (0 << 3) | 7])
    assert cxdlp._is_pixel_pair(rle, 4, 1)
    img = cxdlp._decode_pixel_pair(rle, 4, 1)
    assert img.tolist() == [[0, 255, 255, 0]]


def test_pixel_pair_detection_rejects_rle7():
    assert not cxdlp._is_pixel_pair(bytes([0x7F, 0x7F]), 2, 1)   # no 0x80 flag
    assert not cxdlp._is_pixel_pair(bytes([0x80, 0x05]), 4, 1)   # run longer than the row
    assert not cxdlp._is_pixel_pair(b"\x80", 3, 1)              # odd width
    assert not cxdlp._is_pixel_pair(bytes([0x80]), 4, 2)          # does not cover the image


def _png_pixels(png):
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks = 8, {}
    while pos < len(png):
        (length,) = struct.unpack(">I", png[pos:pos + 4])
        tag = png[pos + 4:pos + 8]
        data = png[pos + 8:pos + 8 + length]
        assert struct.unpack(">I", png[pos + 8 + length:pos + 12 + length])[0] == zlib.crc32(tag + data)
        chunks[tag] = data
        pos += 12 + length
    w, h, depth, color = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
    return w, h, color, zlib.decompress(chunks[b"IDAT"])


def test_png_gray_and_rgb():
    gray = np.arange(12, dtype=np.uint8).reshape(3, 4)
    w, h, color, raw = _png_pixels(cxdlp.png_bytes(gray))
    assert (w, h, color) == (4, 3, 0)
    assert raw[1:5] == bytes([0, 1, 2, 3])  # filter byte, then the first row

    rgb = preview_image(4, 2)
    w, h, color, raw = _png_pixels(cxdlp.png_bytes(rgb))
    assert (w, h, color) == (4, 2, 2)
    assert len(raw) == 2 * (1 + 4 * 3)
