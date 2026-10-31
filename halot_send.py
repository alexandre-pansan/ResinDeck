"""halot-send: talk to a Creality HALOT printer over Wi-Fi/LAN from Linux.

Protocol (WebSocket on port 18188) based on the MIT-licensed
https://github.com/mrtska/HalotMageProSharp, plus what HALOT BOX 4.7 does on the
HALOT-R6 (see docs/PROTOCOL.md).

Usage:
  halot-send version
  halot-send status
  halot-send send FILE.cxdlpv4 [--print]
  halot-send print NAME_ON_PRINTER.cxdlpv4
  halot-send pause | stop
  halot-send [ui]          (web interface in the browser)
Config: ~/.config/halot-send/config  (HOST=..., PASSWORD=...)
"""
import argparse
import asyncio
import base64
import json
import os
import struct
import sys
import zlib
from pathlib import Path

import websockets
from cryptography.hazmat.decrepit.ciphers.algorithms import TripleDES
from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, modes

CONFIG = Path.home() / ".config" / "halot-send" / "config"
PORT = 18188
DES_KEY = b"a85e9d68"
CHUNK = 1 << 20
CHECK_OK = (1, 0)  # R6 answers 1 on success; HalotMageProSharp documents 0
LOWER_KEY = bytes([0xAC, 0x47, 0xB0, 0x02, 0x88, 0x15, 0x6E, 0x5C])


class HalotError(Exception):
    """An error meant for the user. `code` and `params` let the web UI translate it."""

    def __init__(self, message, code=None, **params):
        super().__init__(message)
        self.code = code
        self.params = params


def token(password: str, lower_key=True) -> str:
    """HalotMageProSharp appends LOWER_KEY for passwords over 4 chars; HALOT BOX 4.7 (R6) was
    seen sending only the 8 DES bytes, so both forms are tried (see Halot.status)."""
    if not 1 <= len(password) <= 8:
        raise HalotError("The printer password must be 1 to 8 characters long.", "password_length")
    padder = padding.PKCS7(64).padder()
    data = padder.update(password.encode("ascii")) + padder.finalize()
    # 3DES with K1 == K2 == K3 is single DES
    enc = Cipher(TripleDES(DES_KEY * 3), modes.CBC(b"\0" * 8)).encryptor()
    ct = (enc.update(data) + enc.finalize())[:8]
    if lower_key and len(password) > 4:
        ct += LOWER_KEY
    return base64.b64encode(ct).decode()


def load_config():
    cfg = {}
    if CONFIG.exists():
        for line in CONFIG.read_text().splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    cfg["HOST"] = os.environ.get("HALOT_HOST", cfg.get("HOST", ""))
    cfg["PASSWORD"] = os.environ.get("HALOT_PASSWORD", cfg.get("PASSWORD", ""))
    return cfg


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
            raise HalotError("The printer password is not set.", "no_password")
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
            raise HalotError("Wrong password (TOKEN_ERROR). Check the network password on the printer screen.",
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
            raise HalotError("The printer is still receiving another file. Cancel that upload "
                             "(or restart the printer) and try again.", "printer_receiving")
        if int(r.get("errorcode", 0)) != 0:
            raise HalotError(f"The printer refused the file: {r}", "file_refused", reply=str(r))
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
                    raise HalotError(f"Upload error: {r}", "upload_error", reply=str(r))
                received = int(r.get("received") or 0)
                stalled = stalled + 1 if received <= prev else 0
                prev = max(prev, received)
                if stalled >= 3:
                    raise HalotError(f"The printer stopped accepting data ({received} "
                                     f"of {size} bytes received).", "upload_stalled",
                                     received=received, size=size)
                pct = received * 100 // size
                if pct != last and on_progress:
                    last = pct
                    on_progress(pct)
            if r["cmd"] != "CHECK_DATA":
                r = await self.request_any(("CHECK_DATA",), timeout)
            if int(r.get("checkstate", -1)) not in CHECK_OK:
                raise HalotError(f"The printer rejected the uploaded file (checkstate={r.get('checkstate')}).",
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


def fmt_status(r):
    st = r.get("printStatus", "?")
    lines = [f"State:     {st}"]
    if r.get("filename"):
        lines.append(f"File:      {r['filename']}")
    total = int(r.get("sliceLayerCount") or 0)
    if total:
        cur = int(r.get("curSliceLayer") or 0)
        lines.append(f"Layer:     {cur}/{total} ({cur * 100 // total}%)")
    rem = r.get("printRemainTime")
    if rem not in (None, "", "0", 0):
        s = int(rem)
        lines.append(f"Remaining: {s // 3600}h{s % 3600 // 60:02d}min")
    return "\n".join(lines)


async def main():
    ap = argparse.ArgumentParser(prog="halot-send", description="Control a Creality HALOT resin printer over the network.")
    ap.add_argument("--host", help="printer IP (default: from config)")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("version", help="firmware version (no password needed)")
    sub.add_parser("status", help="print status")
    s = sub.add_parser("send", help="upload a .cxdlpv4 to the printer")
    s.add_argument("file", type=Path)
    s.add_argument("--print", action="store_true", help="start printing after the upload")
    s.add_argument("-y", "--yes", action="store_true", help="do not ask for confirmation before printing")
    p = sub.add_parser("print", help="print a file that is already on the printer")
    p.add_argument("filename")
    p.add_argument("-y", "--yes", action="store_true")
    sub.add_parser("pause", help="pause / resume")
    sub.add_parser("stop", help="cancel the print")
    sub.add_parser("raw-status", help="raw status reply (debug)")
    u = sub.add_parser("ui", help="open the web interface in the browser (default)")
    u.add_argument("--port", type=int, default=8765)
    u.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()

    if a.cmd in (None, "ui"):
        sys.path.insert(0, str(Path(__file__).resolve().parent))  # the launcher runs with -I
        import server
        await server.run(getattr(a, "port", 8765), open_browser=not getattr(a, "no_browser", False))
        return

    cfg = load_config()
    host = a.host or cfg["HOST"]
    if not host:
        sys.exit(f"No printer IP. Put HOST=... in {CONFIG} or use --host")

    def confirm(name):
        if getattr(a, "yes", False):
            return True
        ans = input(f"Start printing '{name}' now? Check the resin and the build plate. [y/N] ")
        return ans.strip().lower() in ("y", "yes", "s", "sim")

    try:
        async with Halot(host, cfg["PASSWORD"]) as h:
            if a.cmd == "version":
                print(await h.request({"cmd": "VERSION_CHECK"}))
            elif a.cmd == "status":
                print(fmt_status(await h.status()))
            elif a.cmd == "raw-status":
                print(json.dumps(await h.status(), indent=2, ensure_ascii=False))
            elif a.cmd == "send":
                if not a.file.is_file():
                    sys.exit(f"File not found: {a.file}")
                if a.file.suffix.lower() not in (".cxdlpv4", ".cxdlp"):
                    print("Warning: the HALOT expects a .cxdlpv4 file.", file=sys.stderr)
                st = await h.status()  # validates password before uploading
                if st.get("printStatus") not in (None, "", "PRINT_GENERAL", "PRINT_COMPLETE", "PRINT_STOP"):
                    print(f"Warning: the printer is in '{st.get('printStatus')}'.", file=sys.stderr)
                print(f"Uploading {a.file.name} ({a.file.stat().st_size / 1e6:.1f} MB) to {host}...")
                r = await h.upload(a.file, on_progress=lambda pct: print(f"\r  uploading... {pct:3d}%", end="", flush=True))
                print()
                print(f"Upload finished (checkstate={r.get('checkstate')}).")
                if a.print and confirm(a.file.name):
                    print(await h.request({"cmd": "START_PRINT", "filename": a.file.name, "token": h.tok()}))
            elif a.cmd == "print":
                if confirm(a.filename):
                    print(await h.request({"cmd": "START_PRINT", "filename": a.filename, "token": h.tok()}))
            elif a.cmd == "pause":
                print(await h.request({"cmd": "PRINT_PAUSE", "token": h.tok()}))
            elif a.cmd == "stop":
                print(await h.request({"cmd": "PRINT_STOP", "token": h.tok()}))
    except HalotError as e:
        sys.exit(str(e))
    except (OSError, asyncio.TimeoutError, websockets.WebSocketException) as e:
        sys.exit(f"Could not talk to {host}: {e!r}")


if __name__ == "__main__":
    asyncio.run(main())
