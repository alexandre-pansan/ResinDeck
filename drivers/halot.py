"""Driver for Creality HALOT resin printers (tested on the HALOT-R6).

WebSocket on port 18188. Based on the MIT-licensed
https://github.com/mrtska/HalotMageProSharp plus what HALOT BOX 4.7 does on the
HALOT-R6. Details in docs/PROTOCOL.md.

`Halot` speaks the protocol; `HalotDriver` adapts it to the Driver contract (drivers/base.py).
"""
import asyncio
import base64
import json
import struct
import sys
import zlib
from pathlib import Path

import websockets
from cryptography.hazmat.decrepit.ciphers.algorithms import TripleDES
from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, modes

from drivers import base
from drivers.base import Driver, PrinterError, Status, tcp_open

PORT = 18188
DES_KEY = b"a85e9d68"
CHUNK = 1 << 20
CHECK_OK = (1, 0)  # R6 answers 1 on success; HalotMageProSharp documents 0
LOWER_KEY = bytes([0xAC, 0x47, 0xB0, 0x02, 0x88, 0x15, 0x6E, 0x5C])


def token(password: str, lower_key=True) -> str:
    """HalotMageProSharp appends LOWER_KEY for passwords over 4 chars; HALOT BOX 4.7 (R6) was
    seen sending only the 8 DES bytes, so both forms are tried (see Halot.status)."""
    if not 1 <= len(password) <= 8:
        raise PrinterError("The printer password must be 1 to 8 characters long.", "password_length", max=8)
    padder = padding.PKCS7(64).padder()
    data = padder.update(password.encode("ascii")) + padder.finalize()
    # 3DES with K1 == K2 == K3 is single DES
    enc = Cipher(TripleDES(DES_KEY * 3), modes.CBC(b"\0" * 8)).encryptor()
    ct = (enc.update(data) + enc.finalize())[:8]
    if lower_key and len(password) > 4:
        ct += LOWER_KEY
    return base64.b64encode(ct).decode()


class Halot:
    def __init__(self, host, password):
        self.url = f"ws://{host}:{PORT}"
        self.password = password
        self.lower_key = True
        self.ws = None

    async def __aenter__(self):
        self.ws = await websockets.connect(
            self.url, ping_interval=None, max_size=None, open_timeout=10
        )
        return self

    async def __aexit__(self, *exc):
        await self.ws.close()

    def tok(self):
        if not self.password:
            raise PrinterError("The printer password is not set.", "no_password")
        return token(self.password, self.lower_key)

    async def send_json(self, obj):
        await self.ws.send(json.dumps(obj))

    async def recv_json(self, timeout=10):
        while True:
            msg = await asyncio.wait_for(self.ws.recv(), timeout)
            if isinstance(msg, bytes):
                continue
            try:
                return json.loads(msg)
            except json.JSONDecodeError:
                print("(non-JSON reply)", msg[:200], file=sys.stderr)

    async def request(self, obj, want=None, timeout=10):
        await self.send_json(obj)
        want = want or obj.get("cmd")
        while True:
            r = await self.recv_json(timeout)
            if r.get("cmd") == want or (want == "VERSION_CHECK" and "version" in r):
                return r

    async def status(self):
        r = await self.request({"cmd": "GET_PRINT_STATUS", "token": self.tok()})
        if r.get("printStatus") == "TOKEN_ERROR" and len(self.password) > 4:
            self.lower_key = not self.lower_key
            r = await self.request({"cmd": "GET_PRINT_STATUS", "token": self.tok()})
        if r.get("printStatus") == "TOKEN_ERROR":
            raise PrinterError("Wrong password (TOKEN_ERROR). Check the network password on the printer screen.",
                             "wrong_password")
        return r

    async def upload(self, path: Path, timeout=60, on_progress=None):
        """Protocol as done by HALOT BOX 4.7 (cxWebsocket.dll):
        - key: the file's trailing CRC (last 4 bytes) read as a signed little-endian int, in hex;
        - data in 1 MB chunks, each waiting for its START_DATA ack ("received" so far);
        - when START_FILE answers compress=true, every chunk goes through qCompress
          (big-endian uncompressed length + zlib);
        - CHECK_DATA ends it; on the HALOT-R6 checkstate 1 means the file was accepted.
        A single uncompressed frame is silently ignored (received stays 0)."""
        data = path.read_bytes()
        size = len(data)
        key = format(int.from_bytes(data[-4:], "little", signed=True), "x")
        await self.send_json({
            "cmd": "START_FILE", "filename": path.name, "key": key,
            "offset": "0", "size": str(size),
        })
        r = await self.request_any(("START_FILE",), 30)
        if int(r.get("errorcode", 0)) == 1:
            raise PrinterError("The printer is still receiving another file. Cancel that upload "
                             "(or restart the printer) and try again.", "printer_receiving")
        if int(r.get("errorcode", 0)) != 0:
            raise PrinterError(f"The printer refused the file: {r}", "file_refused", reply=str(r))
        compress = bool(r.get("compress"))
        try:
            last = prev = -1
            stalled = 0
            for offset in range(0, size, CHUNK):
                chunk = data[offset:offset + CHUNK]
                if compress:
                    chunk = struct.pack(">I", len(chunk)) + zlib.compress(chunk)
                await self.ws.send(chunk)
                while True:
                    r = await self.request_any(("START_DATA", "CHECK_DATA"), timeout)
                    if r.get("key") in (None, key):
                        break
                if r["cmd"] == "CHECK_DATA":
                    break
                if int(r.get("errorcode", 0)) != 0:
                    raise PrinterError(f"Upload error: {r}", "upload_error", reply=str(r))
                received = int(r.get("received") or 0)
                stalled = stalled + 1 if received <= prev else 0
                prev = max(prev, received)
                if stalled >= 3:
                    raise PrinterError(f"The printer stopped accepting data ({received} "
                                     f"of {size} bytes received).", "upload_stalled",
                                     received=received, size=size)
                pct = received * 100 // size
                if pct != last and on_progress:
                    last = pct
                    on_progress(pct)
            if r["cmd"] != "CHECK_DATA":
                r = await self.request_any(("CHECK_DATA",), timeout)
            if int(r.get("checkstate", -1)) not in CHECK_OK:
                raise PrinterError(f"The printer rejected the uploaded file (checkstate={r.get('checkstate')}).",
                                 "file_rejected", checkstate=r.get("checkstate"))
            return r
        except BaseException:
            await self.cancel_upload(path.name, key)
            raise

    async def cancel_upload(self, filename, key):
        try:
            await self.send_json({"cmd": "CANCEL_FILE", "key": key, "filename": filename})
            await self.request_any(("CANCEL_FILE",), 5)
        except Exception:
            pass

    async def request_any(self, cmds, timeout):
        while True:
            r = await self.recv_json(timeout)
            if r.get("cmd") in cmds:
                return r


# printStatus values seen from HALOT BOX, HalotMageProSharp and the R6
STATES = {
    "": base.IDLE,
    "PRINT_GENERAL": base.IDLE,
    "PRINT_PROCESSING": base.PRINTING,
    "PRINT_PROGRESS": base.PRINTING,
    "PRINT_PRINTING": base.PRINTING,
    "PRINT_PAUSING": base.PAUSING,
    "PRINT_PAUSE": base.PAUSED,
    "PRINT_STOPING": base.STOPPING,  # sic, as the firmware spells it
    "PRINT_STOPPING": base.STOPPING,
    "PRINT_STOP": base.STOPPED,
    "PRINT_COMPLETING": base.FINISHING,
    "PRINT_COMPLETE": base.FINISHED,
    "PRINT_END": base.FINISHED,
    "PRINT_FAIL": base.FAILED,
}


def _int(value):
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def parse_status(r):
    code = r.get("printStatus") or ""
    return Status(state=STATES.get(code, base.UNKNOWN), file=r.get("filename") or "",
                  layer=_int(r.get("curSliceLayer")), layers=_int(r.get("sliceLayerCount")),
                  remaining=_int(r.get("printRemainTime")), code=code, raw=r)


class HalotDriver(Driver):
    id = "halot"
    name = "Creality HALOT"
    models = "HALOT-R6 (tested); other HALOT models with network printing should work"
    extensions = (".cxdlpv4", ".cxdlp")
    password = True
    password_max = 8

    async def connect(self):
        self.halot = Halot(self.host, self.secret)
        await self.halot.__aenter__()
        r = await self.halot.request({"cmd": "VERSION_CHECK"})
        self.version = r.get("version")

    async def close(self):
        await self.halot.__aexit__()

    async def ping(self):
        await self.halot.request({"cmd": "VERSION_CHECK"})

    async def status(self):
        return parse_status(await self.halot.status())

    async def upload(self, path, on_progress=None):
        self.halot.tok()  # fail before sending anything when the password is missing
        r = await self.halot.upload(Path(path), on_progress=on_progress)
        return {"checkstate": r.get("checkstate")}

    async def command(self, obj):
        return await self.halot.request({**obj, "token": self.halot.tok()})

    async def start(self, filename):
        return await self.command({"cmd": "START_PRINT", "filename": filename})

    async def pause(self):
        return await self.command({"cmd": "PRINT_PAUSE"})

    async def resume(self):
        return await self.command({"cmd": "PRINT_PAUSE"})  # the same command toggles

    async def stop(self):
        return await self.command({"cmd": "PRINT_STOP"})

    @classmethod
    def check_password(cls, password):
        if len(password) > cls.password_max:
            raise PrinterError("The password has at most 8 characters.", "password_length", max=8)

    @classmethod
    async def probe(cls, host, timeout=1.5):
        if not await tcp_open(host, PORT):
            return False
        try:
            async with websockets.connect(f"ws://{host}:{PORT}", open_timeout=timeout) as ws:
                await ws.send(json.dumps({"cmd": "VERSION_CHECK"}))
                msg = await asyncio.wait_for(ws.recv(), timeout)
                return json.loads(msg).get("version") is not None
        except (OSError, asyncio.TimeoutError, websockets.WebSocketException, ValueError):
            return False
