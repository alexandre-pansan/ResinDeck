import base64
import struct
import zlib

import pytest

import halot
from halot import Halot, PrinterError, fmt_status, token


# --- token ---------------------------------------------------------------------------

def test_token_is_des_of_the_padded_password():
    # regression values: the printer accepts exactly these bytes
    assert token("abc") == "E/8A0f6OHM4="
    assert token("secret1", lower_key=False) == base64.b64encode(base64.b64decode(token("secret1"))[:8]).decode()


def test_token_appends_lower_key_only_for_long_passwords():
    assert len(base64.b64decode(token("abcd"))) == 8
    long_tok = base64.b64decode(token("abcde"))
    assert len(long_tok) == 16
    assert long_tok[8:] == halot.LOWER_KEY
    assert len(base64.b64decode(token("abcde", lower_key=False))) == 8


@pytest.mark.parametrize("password", ["", "123456789"])
def test_token_rejects_bad_lengths(password):
    with pytest.raises(PrinterError) as e:
        token(password)
    assert e.value.code == "password_length"


def test_printer_error_keeps_code_and_params():
    e = PrinterError("busy", "printer_busy", state="PRINT_PROCESSING")
    assert str(e) == "busy" and e.code == "printer_busy" and e.params == {"state": "PRINT_PROCESSING"}


# --- status --------------------------------------------------------------------------

async def test_version_needs_no_password(printer):
    async with Halot("127.0.0.1", "") as h:
        r = await h.request({"cmd": "VERSION_CHECK"})
    assert r["version"] == "1"


async def test_status(printer):
    async with Halot("127.0.0.1", "secret1") as h:
        r = await h.status()
    assert r["printStatus"] == "PRINT_GENERAL"


async def test_status_falls_back_to_the_short_token(printer):
    printer.short_token_only = True
    async with Halot("127.0.0.1", "secret1") as h:
        r = await h.status()
        assert r["printStatus"] == "PRINT_GENERAL"
        assert h.lower_key is False


async def test_status_wrong_password(printer):
    async with Halot("127.0.0.1", "nope123") as h:
        with pytest.raises(PrinterError) as e:
            await h.status()
    assert e.value.code == "wrong_password"


async def test_commands_need_a_password(printer):
    async with Halot("127.0.0.1", "") as h:
        with pytest.raises(PrinterError) as e:
            h.tok()
    assert e.value.code == "no_password"


async def test_non_json_and_binary_replies_are_skipped(printer, monkeypatch):
    async with Halot("127.0.0.1", "secret1") as h:
        replies = iter([b"\x00\x01", "not json", '{"cmd": "OTHER"}', '{"cmd": "GET_PRINT_STATUS", "printStatus": "X"}'])

        async def fake_recv():
            return next(replies)

        monkeypatch.setattr(h.ws, "recv", fake_recv)
        monkeypatch.setattr(h, "send_json", lambda obj: _noop())
        r = await h.request({"cmd": "GET_PRINT_STATUS"})
    assert r["printStatus"] == "X"


async def _noop():
    return None


# --- upload --------------------------------------------------------------------------

@pytest.fixture
def small_chunks(monkeypatch):
    monkeypatch.setattr(halot, "CHUNK", 512)


async def test_upload_compressed(printer, sliced, small_chunks):
    seen = []
    async with Halot("127.0.0.1", "secret1") as h:
        r = await h.upload(sliced, on_progress=seen.append)
    assert int(r["checkstate"]) == 1
    assert printer.files["part.cxdlpv4"] == sliced.read_bytes()
    assert seen == sorted(seen) and seen[-1] == 100
    start = next(c for c in printer.commands if c["cmd"] == "START_FILE")
    data = sliced.read_bytes()
    assert start["key"] == format(int.from_bytes(data[-4:], "little", signed=True), "x")
    assert start["size"] == str(len(data))


async def test_upload_uncompressed_with_inline_check(printer, sliced, small_chunks):
    printer.compress = False
    printer.check_inline = True
    async with Halot("127.0.0.1", "secret1") as h:
        await h.upload(sliced)
    assert printer.files["part.cxdlpv4"] == sliced.read_bytes()


async def test_upload_printer_busy(printer, sliced):
    printer.busy = True
    async with Halot("127.0.0.1", "secret1") as h:
        with pytest.raises(PrinterError) as e:
            await h.upload(sliced)
    assert e.value.code == "printer_receiving"


async def test_upload_refused(printer, sliced):
    printer.refuse = True
    async with Halot("127.0.0.1", "secret1") as h:
        with pytest.raises(PrinterError) as e:
            await h.upload(sliced)
    assert e.value.code == "file_refused"


async def test_upload_stalled_is_cancelled(printer, sliced, small_chunks):
    printer.stall = True
    async with Halot("127.0.0.1", "secret1") as h:
        with pytest.raises(PrinterError) as e:
            await h.upload(sliced)
    assert e.value.code == "upload_stalled"
    assert printer.cancelled == ["part.cxdlpv4"]


async def test_upload_rejected_checkstate_is_cancelled(printer, sliced, small_chunks):
    printer.checkstate = 5
    async with Halot("127.0.0.1", "secret1") as h:
        with pytest.raises(PrinterError) as e:
            await h.upload(sliced)
    assert e.value.code == "file_rejected" and e.value.params == {"checkstate": "5"}
    assert printer.cancelled == ["part.cxdlpv4"]


def test_qcompress_framing_matches_qt():
    # qCompress: big-endian uncompressed length, then a zlib stream
    data = b"layer" * 100
    framed = struct.pack(">I", len(data)) + zlib.compress(data)
    assert struct.unpack(">I", framed[:4])[0] == len(data)
    assert zlib.decompress(framed[4:]) == data


# --- fmt_status ----------------------------------------------------------------------

def test_fmt_status_idle():
    assert fmt_status({"printStatus": "PRINT_GENERAL"}) == "State:     PRINT_GENERAL"


def test_fmt_status_printing():
    text = fmt_status({"printStatus": "PRINT_PROCESSING", "filename": "a.cxdlpv4", "sliceLayerCount": "200",
                       "curSliceLayer": "50", "printRemainTime": "3725"})
    assert "File:      a.cxdlpv4" in text
    assert "Layer:     50/200 (25%)" in text
    assert "Remaining: 1h02min" in text
