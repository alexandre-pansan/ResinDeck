"""ResinDeck web UI: file library, 3D/layer preview and printer manager.

Listens on 127.0.0.1 only. Started with `resindeck ui`.
"""
import asyncio
import hashlib
import ipaddress
import json
import os
import socket
import struct
import time
import uuid
import webbrowser
from functools import lru_cache
from pathlib import Path

import numpy as np
import websockets
from aiohttp import WSMsgType, web

import cxdlp
from halot import PORT, Halot, PrinterError
from resindeck import CONFIG, load_config

SETTINGS = CONFIG.parent / "settings.json"
CACHE = Path.home() / ".cache" / "resindeck"
STATIC = Path(__file__).parent / "static"
EXTENSIONS = (".cxdlpv4", ".cxdlp")
POLL_INTERVAL = 3
IDLE_STATES = ("", "PRINT_GENERAL", "PRINT_COMPLETE", "PRINT_STOP")


# --- settings -----------------------------------------------------------------

def load_settings():
    if SETTINGS.exists():
        return json.loads(SETTINGS.read_text())
    s = {"printers": [], "folders": []}
    cfg = load_config()  # migrate the CLI config
    if cfg["HOST"]:
        s["printers"].append({"id": uuid.uuid4().hex[:8], "name": "HALOT",
                              "host": cfg["HOST"], "password": cfg["PASSWORD"]})
    return s


def save_settings(s):
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    tmp = SETTINGS.with_suffix(".tmp")
    tmp.write_text(json.dumps(s, indent=2, ensure_ascii=False))
    os.chmod(tmp, 0o600)
    tmp.replace(SETTINGS)


# --- printers -------------------------------------------------------------------

class Printer:
    def __init__(self, cfg, notify):
        self.cfg = cfg
        self.notify = notify
        self.lock = asyncio.Lock()
        self.halot = None
        self.online = False
        self.version = None
        self.status = {}
        self.error = None
        self.upload = None  # {"file", "pct"}
        self.task = asyncio.create_task(self.loop())

    def public(self):
        return {
            "id": self.cfg["id"], "name": self.cfg["name"], "host": self.cfg["host"],
            "hasPassword": bool(self.cfg.get("password")), "online": self.online,
            "version": self.version, "status": self.status, "error": self.error,
            "upload": self.upload,
        }

    async def loop(self):
        while True:
            try:
                async with Halot(self.cfg["host"], self.cfg.get("password", "")) as h:
                    self.halot = h
                    async with self.lock:
                        r = await h.request({"cmd": "VERSION_CHECK"})
                    self.online, self.version, self.error = True, r.get("version"), None
                    self.notify()
                    while True:
                        await self.poll(h)
                        await asyncio.sleep(POLL_INTERVAL)
            except asyncio.CancelledError:
                raise
            except Exception:
                self.halot = None
                was_online = self.online
                self.online, self.error = False, "no_connection"
                if was_online or self.status:
                    self.status = {}
                self.notify()
                await asyncio.sleep(5)

    async def poll(self, h):
        async with self.lock:
            if not self.cfg.get("password"):
                await h.request({"cmd": "VERSION_CHECK"})  # keeps checking it is online
                self.status, self.error = {}, "no_password"
                self.notify()
                return
            try:
                st = await h.status()
                self.status, self.error = st, None
            except PrinterError as e:
                self.status, self.error = {}, e.code or str(e)
        self.notify()

    async def command(self, obj):
        h = self.halot
        if not h:
            raise PrinterError("The printer is offline.", "offline")
        async with self.lock:
            obj = {**obj, "token": h.tok()}
            return await h.request(obj)

    async def send(self, path: Path, start=False):
        h = self.halot
        if not h:
            raise PrinterError("The printer is offline.", "offline")
        st = self.status.get("printStatus", "")
        if st not in IDLE_STATES:
            raise PrinterError(f"The printer is busy ({st}).", "printer_busy", state=st)
        loop = asyncio.get_running_loop()

        def progress(pct):
            self.upload = {"file": path.name, "pct": pct}
            loop.call_soon(self.notify)

        async with self.lock:
            h.tok()  # fail early without a password
            progress(0)
            try:
                r = await h.upload(path, on_progress=progress)
            finally:
                self.upload = None
                self.notify()
            result = {"checkstate": r.get("checkstate")}
            if start:
                result["start"] = await h.request(
                    {"cmd": "START_PRINT", "filename": path.name, "token": h.tok()})
        return result

    async def close(self):
        self.task.cancel()
        try:
            await self.task
        except (asyncio.CancelledError, Exception):
            pass


async def probe(host, timeout=1.5):
    try:
        async with websockets.connect(f"ws://{host}:{PORT}", open_timeout=timeout) as ws:
            await ws.send(json.dumps({"cmd": "VERSION_CHECK"}))
            msg = await asyncio.wait_for(ws.recv(), timeout)
            return json.loads(msg).get("version") is not None
    except Exception:
        return False


async def port_open(host, timeout=0.6):
    try:
        _, w = await asyncio.wait_for(asyncio.open_connection(host, PORT), timeout)
        w.close()
        return True
    except Exception:
        return False


def local_networks():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
    except OSError:
        return []
    finally:
        s.close()
    return [ipaddress.ip_network(f"{ip}/24", strict=False)]


# --- files ----------------------------------------------------------------------

@lru_cache(maxsize=32)
def _load(path, mtime):
    return cxdlp.load(path)


def load_file(path: Path):
    return _load(str(path), path.stat().st_mtime)


def voxel_cache_path(path: Path):
    st = path.stat()
    key = hashlib.sha1(f"{path}|{st.st_mtime}|{st.st_size}".encode()).hexdigest()[:16]
    return CACHE / f"vox-{key}.bin"


def build_voxels(path: Path, on_progress=None):
    """Return the voxel payload: uint32 meta length, JSON meta, packed bits."""
    cache = voxel_cache_path(path)
    if cache.exists():
        return cache.read_bytes()
    grid, meta = load_file(path).voxels(on_progress=on_progress)
    meta_b = json.dumps(meta).encode()
    payload = struct.pack("<I", len(meta_b)) + meta_b + np.packbits(grid, axis=None).tobytes()
    CACHE.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(payload)
    return payload


class App:
    def __init__(self):
        self.settings = load_settings()
        self.printers = {}
        self.clients = set()
        self.voxel_jobs = {}
        self._notify_pending = False

    async def start(self):
        save_settings(self.settings)
        for cfg in self.settings["printers"]:
            self.printers[cfg["id"]] = Printer(cfg, self.notify)

    async def stop(self):
        for p in self.printers.values():
            await p.close()

    # push updates (coalesced) to every open browser tab
    def notify(self):
        if not self._notify_pending:
            self._notify_pending = True
            asyncio.get_running_loop().call_later(0.1, self._flush)

    def _flush(self):
        self._notify_pending = False
        self.broadcast({"type": "printers", "printers": self.printer_list()})

    def broadcast(self, msg):
        data = json.dumps(msg)
        for ws in list(self.clients):
            asyncio.create_task(self._send(ws, data))

    async def _send(self, ws, data):
        try:
            await ws.send_str(data)
        except Exception:
            self.clients.discard(ws)

    def printer_list(self):
        return [self.printers[c["id"]].public() for c in self.settings["printers"]]

    def resolve(self, raw):
        """Only allow files inside the configured library folders."""
        if not raw:
            raise web.HTTPBadRequest(text="missing path")
        p = Path(raw).resolve()
        for folder in self.settings["folders"]:
            if p.is_relative_to(Path(folder).resolve()):
                if not p.is_file():
                    raise web.HTTPNotFound(text="file not found")
                return p
        raise web.HTTPForbidden(text="file is outside the library folders")

    def printer(self, request):
        p = self.printers.get(request.match_info["id"])
        if not p:
            raise web.HTTPNotFound(text="printer not found")
        return p


def json_error(fn):
    async def wrapper(request):
        try:
            return await fn(request)
        except PrinterError as e:
            return web.json_response({"error": str(e), "code": e.code, "params": e.params},
                                     status=400)
        except (OSError, asyncio.TimeoutError, websockets.WebSocketException) as e:
            return web.json_response({"error": f"Communication failure: {e!r}",
                                      "code": "comm_failure", "params": {"detail": repr(e)}},
                                     status=502)
    return wrapper


routes = web.RouteTableDef()


def app_of(request) -> App:
    return request.app["halot"]


@routes.get("/")
async def index(request):
    return web.FileResponse(STATIC / "index.html")


@routes.get("/ws")
async def ws_handler(request):
    app = app_of(request)
    ws = web.WebSocketResponse(heartbeat=30)
    await ws.prepare(request)
    app.clients.add(ws)
    await ws.send_json({"type": "printers", "printers": app.printer_list()})
    try:
        async for msg in ws:
            if msg.type == WSMsgType.ERROR:
                break
    finally:
        app.clients.discard(ws)
    return ws


# printers

@routes.get("/api/printers")
async def printers_list(request):
    return web.json_response(app_of(request).printer_list())


def clean_printer(body, old=None):
    name = str(body.get("name") or (old or {}).get("name") or "HALOT").strip()[:40]
    host = str(body.get("host") or "").strip()
    if not host:
        raise PrinterError("Enter the printer IP address.", "no_host")
    cfg = {"id": (old or {}).get("id") or uuid.uuid4().hex[:8], "name": name, "host": host,
           "password": (old or {}).get("password", "")}
    if body.get("password") is not None and body["password"] != "":
        if len(body["password"]) > 8:
            raise PrinterError("The password has at most 8 characters.", "password_length")
        cfg["password"] = body["password"]
    if body.get("clearPassword"):
        cfg["password"] = ""
    return cfg


@routes.post("/api/printers")
@json_error
async def printer_add(request):
    app = app_of(request)
    cfg = clean_printer(await request.json())
    app.settings["printers"].append(cfg)
    save_settings(app.settings)
    app.printers[cfg["id"]] = Printer(cfg, app.notify)
    app.notify()
    return web.json_response({"id": cfg["id"]})


@routes.put("/api/printers/{id}")
@json_error
async def printer_edit(request):
    app = app_of(request)
    old = app.printer(request)
    cfg = clean_printer(await request.json(), old.cfg)
    app.settings["printers"] = [cfg if c["id"] == cfg["id"] else c for c in app.settings["printers"]]
    save_settings(app.settings)
    await old.close()
    app.printers[cfg["id"]] = Printer(cfg, app.notify)
    app.notify()
    return web.json_response({"ok": True})


@routes.delete("/api/printers/{id}")
@json_error
async def printer_delete(request):
    app = app_of(request)
    p = app.printer(request)
    await p.close()
    del app.printers[p.cfg["id"]]
    app.settings["printers"] = [c for c in app.settings["printers"] if c["id"] != p.cfg["id"]]
    save_settings(app.settings)
    app.notify()
    return web.json_response({"ok": True})


@routes.post("/api/printers/scan")
@json_error
async def printer_scan(request):
    known = {c["host"] for c in app_of(request).settings["printers"]}
    hosts = [str(h) for net in local_networks() for h in net.hosts()]
    sem = asyncio.Semaphore(128)

    async def check(h):
        async with sem:
            return h if await port_open(h) else None

    open_hosts = [h for h in await asyncio.gather(*map(check, hosts)) if h]
    found = [h for h in open_hosts if await probe(h)]
    return web.json_response([{"host": h, "known": h in known} for h in found])


@routes.post("/api/printers/{id}/send")
@json_error
async def printer_send(request):
    app = app_of(request)
    p = app.printer(request)
    body = await request.json()
    path = app.resolve(body.get("path"))
    return web.json_response(await p.send(path, start=bool(body.get("print"))))


@routes.post("/api/printers/{id}/{action:print|pause|stop}")
@json_error
async def printer_action(request):
    p = app_of(request).printer(request)
    action = request.match_info["action"]
    if action == "print":
        body = await request.json()
        r = await p.command({"cmd": "START_PRINT", "filename": body["filename"]})
    elif action == "pause":
        r = await p.command({"cmd": "PRINT_PAUSE"})
    else:
        r = await p.command({"cmd": "PRINT_STOP"})
    if p.halot:
        await p.poll(p.halot)
    return web.json_response(r)


# library

@routes.get("/api/library")
async def library(request):
    app = app_of(request)
    files = []
    for folder in app.settings["folders"]:
        d = Path(folder)
        if not d.is_dir():
            continue
        for f in d.iterdir():
            if f.suffix.lower() in EXTENSIONS and f.is_file():
                st = f.stat()
                files.append({"path": str(f), "name": f.name, "folder": folder,
                              "size": st.st_size, "mtime": st.st_mtime})
    files.sort(key=lambda f: -f["mtime"])
    return web.json_response({"folders": app.settings["folders"], "files": files})


@routes.put("/api/library/folders")
@json_error
async def library_folders(request):
    app = app_of(request)
    folders = []
    for f in (await request.json()).get("folders", []):
        p = Path(os.path.expanduser(str(f).strip()))
        if not p.is_dir():
            raise PrinterError(f"Folder does not exist: {p}", "folder_missing", folder=str(p))
        folders.append(str(p.resolve()))
    app.settings["folders"] = list(dict.fromkeys(folders))
    save_settings(app.settings)
    return web.json_response({"folders": app.settings["folders"]})


@routes.post("/api/library/upload")
@json_error
async def library_upload(request):
    app = app_of(request)
    if not app.settings["folders"]:
        raise PrinterError("Set up a library folder first.", "no_folder")
    dest_dir = Path(app.settings["folders"][0])
    reader = await request.multipart()
    saved = []
    while (part := await reader.next()) is not None:
        name = Path(part.filename or "").name
        if not name.lower().endswith(EXTENSIONS):
            raise PrinterError(f"Only .cxdlpv4 files: {name}", "only_cxdlp", name=name)
        dest = dest_dir / name
        tmp = dest.with_suffix(dest.suffix + ".part")
        with open(tmp, "wb") as f:
            while chunk := await part.read_chunk(1 << 20):
                f.write(chunk)
        tmp.replace(dest)
        saved.append(str(dest))
    return web.json_response({"saved": saved})


@routes.get("/api/file/info")
@json_error
async def file_info(request):
    app = app_of(request)
    path = app.resolve(request.query.get("path"))
    try:
        c = await asyncio.to_thread(load_file, path)
    except ValueError as e:
        raise PrinterError(str(e), "bad_file", detail=str(e))
    info = c.info()
    info["hasVoxels"] = voxel_cache_path(path).exists()
    info["layers"] = [round(L.z, 4) for L in c.layers]
    return web.json_response(info)


@routes.get("/api/file/preview")
@json_error
async def file_preview(request):
    app = app_of(request)
    path = app.resolve(request.query.get("path"))
    c = await asyncio.to_thread(load_file, path)
    if not c.previews:
        raise web.HTTPNotFound()
    index = -1 if request.query.get("size") != "small" else 0
    png = await asyncio.to_thread(lambda: cxdlp.png_bytes(c.preview_rgb(index)))
    return web.Response(body=png, content_type="image/png",
                        headers={"Cache-Control": "max-age=3600"})


@routes.get("/api/file/layer")
@json_error
async def file_layer(request):
    app = app_of(request)
    path = app.resolve(request.query.get("path"))
    c = await asyncio.to_thread(load_file, path)
    n = int(request.query.get("n", 0))
    if not 0 <= n < len(c.layers):
        raise web.HTTPBadRequest(text="invalid layer")
    png = await asyncio.to_thread(lambda: cxdlp.png_bytes(c.layer_image(n)))
    return web.Response(body=png, content_type="image/png",
                        headers={"Cache-Control": "max-age=3600"})


@routes.get("/api/file/voxels")
@json_error
async def file_voxels(request):
    app = app_of(request)
    path = app.resolve(request.query.get("path"))
    key = str(path)
    job = app.voxel_jobs.get(key)
    if job is None:
        loop = asyncio.get_running_loop()
        last = [0.0]

        def progress(pct):
            if time.monotonic() - last[0] > 0.3:
                last[0] = time.monotonic()
                loop.call_soon_threadsafe(app.broadcast,
                                          {"type": "voxels", "path": key, "pct": pct})

        job = asyncio.ensure_future(asyncio.to_thread(build_voxels, path, progress))
        app.voxel_jobs[key] = job
        job.add_done_callback(lambda _: app.voxel_jobs.pop(key, None))
    payload = await asyncio.shield(job)
    return web.Response(body=payload, content_type="application/octet-stream")


def make_app():
    halot = App()
    app = web.Application(client_max_size=1 << 30)
    app["halot"] = halot
    app.add_routes(routes)
    app.router.add_static("/static", STATIC)

    async def on_startup(_):
        await halot.start()

    async def on_cleanup(_):
        await halot.stop()

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    return app


async def run(port=8765, open_browser=True):
    url = f"http://127.0.0.1:{port}/"
    runner = web.AppRunner(make_app())
    await runner.setup()
    try:
        await web.TCPSite(runner, "127.0.0.1", port).start()
    except OSError:
        await runner.cleanup()
        print(f"ResinDeck is already running at {url}")
        if open_browser:
            webbrowser.open(url)
        return
    print(f"ResinDeck running at {url}  (Ctrl+C to quit)")
    if open_browser:
        webbrowser.open(url)
    try:
        await asyncio.Event().wait()
    finally:
        await runner.cleanup()
