"""ResinDeck web UI: file library, 3D/layer preview and printer manager.

Started with `resindeck ui`. Listens on the local network so a phone can follow the print:
this computer has full access, other devices need the PIN shown in the interface. Set
RESINDECK_LAN=0 to listen on 127.0.0.1 only.
"""
import asyncio
import base64
import hashlib
import io
import ipaddress
import json
import os
import re
import secrets
import socket
import struct
import time
import uuid
import webbrowser
from functools import lru_cache
from pathlib import Path
from urllib.parse import urlsplit

import numpy as np
import websockets
from aiohttp import WSMsgType, web

import drivers
import formats
import lan
from drivers import PrinterError
from drivers.base import FINISHED
from resindeck import CONFIG, load_config

SETTINGS = CONFIG.parent / "settings.json"
CACHE = Path.home() / ".cache" / "resindeck"
STATIC = Path(__file__).parent / "static"
POLL_INTERVAL = 3
RETRY_DELAY = 5        # seconds between reconnection attempts
SEARCH_INTERVAL = 60   # at most one network search per minute for a printer that went missing
LAN = os.environ.get("RESINDECK_LAN", "1") != "0"
SESSION_COOKIE = "resindeck_session"


# --- settings -----------------------------------------------------------------

def load_settings():
    if SETTINGS.exists():
        s = json.loads(SETTINGS.read_text())
    else:
        s = {"printers": [], "folders": []}
        cfg = load_config()  # migrate the CLI config
        if cfg["HOST"]:
            s["printers"].append({"id": uuid.uuid4().hex[:8], "name": "HALOT", "driver": drivers.DEFAULT,
                                  "host": cfg["HOST"], "password": cfg["PASSWORD"]})
    for printer in s["printers"]:
        printer.setdefault("driver", drivers.DEFAULT)  # settings from before drivers existed
    s.setdefault("pin", new_pin())
    # devices that entered the PIN: {"id", "hash" (sha256 of the cookie), "name", "ip", "created"}
    s["sessions"] = [x for x in s.get("sessions", []) if isinstance(x, dict)]
    return s


def new_pin():
    return f"{secrets.randbelow(10 ** 6):06d}"


def save_settings(s):
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    tmp = SETTINGS.with_suffix(".tmp")
    tmp.write_text(json.dumps(s, indent=2, ensure_ascii=False))
    os.chmod(tmp, 0o600)
    tmp.replace(SETTINGS)


# --- printers -------------------------------------------------------------------

class Printer:
    """Keeps a connection to one printer through its driver, polls its status and runs commands."""

    def __init__(self, cfg, notify, save=lambda: None):
        self.cfg = cfg
        self.notify = notify
        self.save = save  # persists cfg after the MAC address or a new IP was found
        self.driver = drivers.get(cfg.get("driver"))
        self.failures = 0
        self.last_search = None
        self.lock = asyncio.Lock()
        self.conn = None  # connected driver instance
        self.online = False
        self.version = None
        self.status = None
        self.error = None
        self.upload = None  # {"file", "pct"}
        self.seen_active = False  # saw the current job printing since ResinDeck started
        self.task = asyncio.create_task(self.loop())

    def public(self):
        return {
            "id": self.cfg["id"], "name": self.cfg["name"], "host": self.cfg["host"], "mac": self.cfg.get("mac"),
            "driver": self.driver.id, "driverName": self.driver.name, "extensions": list(self.driver.extensions),
            "hasPassword": bool(self.cfg.get("password")), "needsPassword": self.driver.password,
            "online": self.online, "version": self.version,
            "status": self.status.public() if self.status else None,
            "error": self.error, "upload": self.upload, "job": self.cfg.get("job"),
        }

    async def loop(self):
        while True:
            try:
                async with self.driver(self.cfg["host"], self.cfg.get("password", "")) as d:
                    if not await self.check_mac():
                        raise ConnectionError("another device answers at this address")
                    self.conn, self.failures = d, 0
                    self.online, self.version, self.error = True, d.version, None
                    self.notify()
                    while True:
                        await self.poll()
                        await asyncio.sleep(POLL_INTERVAL)
            except asyncio.CancelledError:
                raise
            except Exception:
                self.conn = None
                self.online, self.status, self.error = False, None, "no_connection"
                self.failures += 1
                self.notify()
                if self.failures >= 2 and await self.relocate():
                    continue  # try the new address right away
                await asyncio.sleep(RETRY_DELAY)

    # DHCP may give the printer another IP address: remember its MAC address, which does not change,
    # and look for it on the network when the printer stops answering at the old address.

    async def check_mac(self):
        """Learn the MAC address on the first connection; False if a different device answers."""
        ip = await asyncio.to_thread(lan.resolve, self.cfg["host"])
        mac = ip and await asyncio.to_thread(lan.mac_of, ip)
        if not mac:
            return True  # not on this network segment, or the system does not say: trust the address
        if not self.cfg.get("mac"):
            self.cfg["mac"] = mac
            self.save()
            return True
        return mac == self.cfg["mac"]

    async def relocate(self):
        """Find the printer's MAC address at another IP. True if the address changed."""
        mac = self.cfg.get("mac")
        now = time.monotonic()
        if not mac or (self.last_search is not None and now - self.last_search < SEARCH_INTERVAL):
            return False
        self.last_search = now
        ip = await find_by_mac(mac, self.driver, exclude=self.cfg["host"])
        if not ip:
            return False
        print(f"{self.cfg['name']}: {mac} moved from {self.cfg['host']} to {ip}")
        self.cfg["host"] = ip
        self.save()
        self.notify()
        return True

    async def poll(self):
        d = self.conn
        async with self.lock:
            if self.driver.password and not self.cfg.get("password"):
                await d.ping()  # keeps checking it is online
                self.status, self.error = None, "no_password"
            else:
                try:
                    self.status, self.error = await d.status(), None
                except PrinterError as e:
                    self.status, self.error = None, e.code or str(e)
        self.track_job()
        self.notify()

    def track_job(self):
        """Printers report how long is left, not when the print began: note when each job starts and ends.
        Saved as cfg["job"] = {"file", "started", "approx", "ended"}: Unix times. `started` is None when unknown;
        `ended` is None while printing, and False once over without a known finish time (stopped, failed,
        or finished while ResinDeck was closed)."""
        st = self.status
        if not st:
            return
        job = self.cfg.get("job")
        now = time.time()
        if not st.ready:
            if job and job["file"] == st.file and job["ended"] is None:
                self.seen_active = True
                return
            started, approx = now, False
            if st.layer > 1:  # already printing when first seen: work the start out from the progress
                approx = True
                done = st.layer - 1
                started = now - st.remaining * done / (st.layers - done) if st.remaining and st.layers > done else None
            self.cfg["job"] = {"file": st.file, "started": started, "approx": approx, "ended": None}
            self.seen_active = True
            self.save()
        elif job and job["ended"] is None:
            job["ended"] = now if self.seen_active and st.state == FINISHED else False
            self.seen_active = False
            self.save()

    def connected(self):
        if not self.conn:
            raise PrinterError("The printer is offline.", "offline")
        return self.conn

    async def command(self, action, *args):
        """Run a driver command (start, pause, resume, stop) and refresh the status."""
        d = self.connected()
        async with self.lock:
            d.need_password()
            r = await getattr(d, action)(*args)
        await self.poll()
        return r

    async def send(self, path: Path, start=False):
        d = self.connected()
        if path.suffix.lower() not in self.driver.extensions:
            raise PrinterError(f"{self.driver.name} printers do not accept {path.suffix} files.",
                               "file_not_supported", printer=self.driver.name, ext=path.suffix)
        if self.status and not self.status.ready:
            st = self.status.code or self.status.state
            raise PrinterError(f"The printer is busy ({st}).", "printer_busy", state=st)
        loop = asyncio.get_running_loop()

        def progress(pct):
            self.upload = {"file": path.name, "pct": pct}
            loop.call_soon(self.notify)

        async with self.lock:
            d.need_password()
            progress(0)
            try:
                result = dict(await d.upload(path, on_progress=progress))
            finally:
                self.upload = None
                self.notify()
            if start:
                result["start"] = await d.start(path.name)
        return result

    async def close(self):
        self.task.cancel()
        try:
            await self.task
        except (asyncio.CancelledError, Exception):
            pass


def local_networks():
    ip = lan.local_ip()
    return [ipaddress.ip_network(f"{ip}/24", strict=False)] if ip else []


async def sweep(driver_classes):
    """Ask every address on the local network whether it is a printer of these families.
    Returns [(driver, host)]. As a side effect the ARP table learns every device that answered."""
    hosts = [str(h) for net in local_networks() for h in net.hosts()]
    sem = asyncio.Semaphore(128)

    async def check(driver, host):
        async with sem:
            return (driver, host) if await driver.probe(host) else None

    jobs = [check(d, h) for d in driver_classes for h in hosts]
    return [r for r in await asyncio.gather(*jobs) if r]


async def find_by_mac(mac, driver, exclude=None):
    """The IP address where the printer with this MAC address answers now, or None."""
    def candidates(table):
        return [ip for ip, m in table.items() if m == mac and ip != exclude]

    ips = candidates(await asyncio.to_thread(lan.neighbors))
    if not ips:
        await sweep([driver])  # fills the ARP table
        ips = candidates(await asyncio.to_thread(lan.neighbors))
    for ip in ips:
        if await driver.probe(ip):
            return ip
    return None


# --- files ----------------------------------------------------------------------

@lru_cache(maxsize=32)
def _load(path, mtime):
    return formats.load(path)


def load_file(path: Path):
    return _load(str(path), path.stat().st_mtime)


def voxel_cache_path(path: Path):
    st = path.stat()
    key = hashlib.sha1(f"{path}|{st.st_mtime}|{st.st_size}".encode(), usedforsecurity=False).hexdigest()[:16]
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
        self.port = None
        self.mdns = False
        self.login_failures = {}  # ip -> (count, blocked until)
        self.seen = {}  # device id -> last request time (wall clock)
        self.quit = asyncio.Event()

    async def start(self):
        save_settings(self.settings)
        for cfg in self.settings["printers"]:
            self.printers[cfg["id"]] = Printer(cfg, self.notify, self.save)

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

    def broadcast(self, msg, local_only=False):
        data = json.dumps(msg)
        for ws in list(self.clients):
            if not local_only or ws.local:
                asyncio.create_task(self._send(ws, data))

    async def drop_device(self, dev_id):
        """Close the live connections of a device (of every device when dev_id is None)."""
        for ws in [w for w in self.clients if w.device and dev_id in (None, w.device)]:
            await ws.close()
        self.devices_changed()

    def devices_changed(self):
        self.broadcast({"type": "devices"}, local_only=True)

    def device_list(self):
        online = {ws.device for ws in self.clients if ws.device}
        return [{"id": d["id"], "name": d["name"], "ip": d["ip"], "created": d["created"],
                 "seen": self.seen.get(d["id"]), "online": d["id"] in online}
                for d in reversed(self.settings["sessions"])]

    async def _send(self, ws, data):
        try:
            await ws.send_str(data)
        except Exception:
            self.clients.discard(ws)

    def save(self):
        save_settings(self.settings)

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


async def read_json(request):
    """The request body as a JSON object; a PrinterError (HTTP 400) when it is not one."""
    try:
        body = await request.json()
    except ValueError:
        body = None
    if not isinstance(body, dict):
        raise PrinterError("Invalid request.", "bad_request")
    return body


# --- access from other devices ---------------------------------------------------

_own = {}  # ip -> (is one of this machine's addresses, checked at)


def own_address(ip):
    """True when the address belongs to this machine: only local addresses can be bound."""
    hit = _own.get(ip)
    if hit and time.monotonic() - hit[1] < 60:
        return hit[0]
    try:
        with socket.socket(socket.AF_INET6 if ":" in ip else socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind((ip, 0))
        ok = True
    except OSError:
        ok = False
    _own[ip] = (ok, time.monotonic())
    return ok


def is_local(request):
    """Requests from this computer, also through resindeck.local or its own LAN address."""
    try:
        ip = ipaddress.ip_address(request.remote or "")
    except ValueError:
        return False
    return ip.is_loopback or own_address(str(ip))


def trusted_host(host):
    """Block DNS rebinding: a page on another site must not reach the API through its own name."""
    name = urlsplit(f"//{host}").hostname or ""
    try:
        ipaddress.ip_address(name)
        return True
    except ValueError:
        return name in ("localhost", socket.gethostname().lower()) or name.endswith(".local")


def session_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


def device_of(request):
    """The paired device making the request, or None."""
    token = request.cookies.get(SESSION_COOKIE)
    if not token:
        return None
    h = session_hash(token)
    return next((d for d in app_of(request).settings["sessions"] if d["hash"] == h), None)


def authorized(request):
    if is_local(request):
        return True
    device = device_of(request)
    if device:
        app_of(request).seen[device["id"]] = time.time()
    return device is not None


def device_name(ua):
    """A readable name from the User-Agent, like "Android phone · Chrome"."""
    os_name = next((n for key, n in (("iPhone", "iPhone"), ("iPad", "iPad"), ("Android", "Android"),
                                      ("Windows", "Windows"), ("Mac OS", "Mac"), ("CrOS", "ChromeOS"),
                                      ("Linux", "Linux")) if key in ua), "")
    if os_name == "Android":
        os_name += " phone" if "Mobile" in ua else " tablet"
    browser = next((n for key, n in (("EdgA", "Edge"), ("Edg/", "Edge"), ("SamsungBrowser", "Samsung"),
                                      ("Firefox", "Firefox"), ("FxiOS", "Firefox"), ("CriOS", "Chrome"),
                                      ("Chrome", "Chrome"), ("Safari", "Safari")) if key in ua), "")
    return " · ".join(x for x in (os_name, browser) if x) or "Unknown device"


OPEN_PATHS = ("/api/session", "/api/login")


@web.middleware
async def guard(request, handler):
    if is_local(request) and not trusted_host(request.host):
        raise web.HTTPForbidden(text="unexpected Host header")
    # browsers send Origin on cross-site fetches and on every WebSocket handshake, which CORS
    # does not protect: without this a page on any site could read /ws
    origin = request.headers.get("Origin")
    if origin and urlsplit(origin).netloc != request.host:
        raise web.HTTPForbidden(text="cross-origin request")
    protected = request.path.startswith("/api/") or request.path == "/ws"
    if protected and request.path not in OPEN_PATHS and not authorized(request):
        return web.json_response({"error": "Enter the PIN shown on the computer.",
                                  "code": "pin_required", "params": {}}, status=401)
    return await handler(request)


def local_only(request):
    if not is_local(request):
        raise web.HTTPForbidden(text="only from the computer running ResinDeck")


@lru_cache(maxsize=1)
def content_security_policy():
    # the import map is the only inline script: allow exactly that one by its hash
    html = (STATIC / "index.html").read_text(encoding="utf-8")
    m = re.search(r'<script type="importmap">(.*?)</script>', html, re.S)
    script = "'self'"
    if m:
        digest = base64.b64encode(hashlib.sha256(m.group(1).encode()).digest()).decode()
        script += f" 'sha256-{digest}'"
    return ("default-src 'self'; "
            f"script-src {script}; "
            "style-src 'self' 'unsafe-inline'; "  # progress bars set style="width:…"
            "img-src 'self' data: blob:; connect-src 'self'; object-src 'none'; "
            "base-uri 'none'; form-action 'self'; frame-ancestors 'none'")


@web.middleware
async def security_headers(request, handler):
    try:
        res = await handler(request)
    except web.HTTPException as e:
        res = e
    if not res.prepared:  # WebSocket and streamed responses already sent their headers
        res.headers.setdefault("X-Content-Type-Options", "nosniff")
        res.headers.setdefault("X-Frame-Options", "DENY")
        res.headers.setdefault("Referrer-Policy", "no-referrer")
        res.headers.setdefault("Cross-Origin-Resource-Policy", "same-origin")
        res.headers.setdefault("Content-Security-Policy", content_security_policy())
    if isinstance(res, web.HTTPException):
        raise res
    return res


routes = web.RouteTableDef()


APP = web.AppKey("halot", App)


def app_of(request) -> App:
    return request.app[APP]


@routes.get("/")
async def index(request):
    return web.FileResponse(STATIC / "index.html")


@routes.get("/ws")
async def ws_handler(request):
    app = app_of(request)
    ws = web.WebSocketResponse(heartbeat=30)
    await ws.prepare(request)
    ws.local = is_local(request)
    ws.device = None if ws.local else device_of(request)["id"]
    app.clients.add(ws)
    if ws.device:
        app.devices_changed()
    await ws.send_json({"type": "printers", "printers": app.printer_list()})
    try:
        async for msg in ws:
            if msg.type == WSMsgType.ERROR:
                break
    finally:
        app.clients.discard(ws)
        if ws.device:
            app.devices_changed()
    return ws


# session

@routes.get("/api/session")
async def session(request):
    return web.json_response({"local": is_local(request), "authorized": authorized(request)})


@routes.post("/api/login")
@json_error
async def login(request):
    app = app_of(request)
    ip = request.remote or ""
    count, until = app.login_failures.get(ip, (0, 0))
    if time.monotonic() < until:
        raise PrinterError("Too many attempts. Wait a minute.", "pin_wait")
    pin = str((await read_json(request)).get("pin") or "").strip()
    if not secrets.compare_digest(pin, app.settings["pin"]):
        count += 1
        app.login_failures[ip] = (count, time.monotonic() + 60 if count >= 5 else 0)
        raise PrinterError("Wrong PIN.", "pin_wrong")
    app.login_failures.pop(ip, None)
    token = secrets.token_urlsafe(32)
    device = {"id": uuid.uuid4().hex[:8], "hash": session_hash(token),
              "name": device_name(request.headers.get("User-Agent", "")), "ip": ip,
              "created": time.time()}
    app.settings["sessions"] = app.settings["sessions"][-49:] + [device]
    save_settings(app.settings)
    app.devices_changed()
    res = web.json_response({"ok": True})
    res.set_cookie(SESSION_COOKIE, token, max_age=365 * 86400, httponly=True, samesite="Strict")
    return res


def remote_urls(app):
    ip = lan.local_ip() if LAN else None
    urls = []
    if ip and app.mdns:
        urls.append(f"http://{lan.MDNS_NAME}:{app.port}/")
    if ip:
        urls.append(f"http://{ip}:{app.port}/")
    return ip, urls


@routes.get("/api/remote")
async def remote_info(request):
    local_only(request)
    app = app_of(request)
    ip, urls = remote_urls(app)
    return web.json_response({"enabled": bool(ip), "urls": urls, "pin": app.settings["pin"],
                              "devices": app.device_list()})


@routes.delete("/api/remote/devices/{id}")
async def remote_device_delete(request):
    """Disconnect one device: it has to enter the PIN again."""
    local_only(request)
    app = app_of(request)
    dev_id = request.match_info["id"]
    app.settings["sessions"] = [d for d in app.settings["sessions"] if d["id"] != dev_id]
    save_settings(app.settings)
    app.seen.pop(dev_id, None)
    await app.drop_device(dev_id)
    return web.json_response({"ok": True})


@routes.get("/api/remote/qr.svg")
async def remote_qr(request):
    local_only(request)
    import segno
    app = app_of(request)
    _, urls = remote_urls(app)
    if not urls:
        raise web.HTTPNotFound()
    url = urls[0]  # resindeck.local, or the IP when mDNS is not available; the PIN is typed
    buf = io.BytesIO()
    segno.make(url, error="m").save(buf, kind="svg", scale=6, border=2, dark="#000", light="#fff")
    return web.Response(body=buf.getvalue(), content_type="image/svg+xml",
                        headers={"Cache-Control": "no-store"})


@routes.post("/api/remote/pin")
async def remote_new_pin(request):
    """New PIN and sign out every other device."""
    local_only(request)
    app = app_of(request)
    app.settings["pin"] = new_pin()
    app.settings["sessions"] = []
    save_settings(app.settings)
    app.seen.clear()
    await app.drop_device(None)
    return web.json_response({"pin": app.settings["pin"]})


@routes.post("/api/quit")
async def quit_app(request):
    """Stop ResinDeck (the tray menu is not available on every desktop)."""
    local_only(request)
    asyncio.get_running_loop().call_later(0.2, app_of(request).quit.set)
    return web.json_response({"ok": True})


# printers

@routes.get("/api/printers")
async def printers_list(request):
    return web.json_response(app_of(request).printer_list())


def clean_printer(body, old=None):
    old = old or {}
    driver = drivers.get(str(body.get("driver") or old.get("driver") or drivers.DEFAULT))
    name = str(body.get("name") or old.get("name") or driver.name).strip()[:40]
    host = str(body.get("host") or "").strip()
    if not host:
        raise PrinterError("Enter the printer IP address.", "no_host")
    cfg = {"id": old.get("id") or uuid.uuid4().hex[:8], "name": name, "driver": driver.id, "host": host,
           "password": old.get("password", "")}
    if host == old.get("host"):  # a new address may be another printer: learn its MAC again
        cfg.update({k: old[k] for k in ("mac", "job") if old.get(k)})
    if body.get("password") is not None and body["password"] != "":
        driver.check_password(str(body["password"]))
        cfg["password"] = str(body["password"])
    if body.get("clearPassword"):
        cfg["password"] = ""
    return cfg


@routes.get("/api/drivers")
async def drivers_list(request):
    return web.json_response([d.describe() for d in drivers.DRIVERS.values()])


@routes.post("/api/printers")
@json_error
async def printer_add(request):
    app = app_of(request)
    local_only(request)
    cfg = clean_printer(await read_json(request))
    app.settings["printers"].append(cfg)
    save_settings(app.settings)
    app.printers[cfg["id"]] = Printer(cfg, app.notify, app.save)
    app.notify()
    return web.json_response({"id": cfg["id"]})


@routes.put("/api/printers/{id}")
@json_error
async def printer_edit(request):
    local_only(request)
    app = app_of(request)
    old = app.printer(request)
    cfg = clean_printer(await read_json(request), old.cfg)
    app.settings["printers"] = [cfg if c["id"] == cfg["id"] else c for c in app.settings["printers"]]
    save_settings(app.settings)
    await old.close()
    app.printers[cfg["id"]] = Printer(cfg, app.notify, app.save)
    app.notify()
    return web.json_response({"ok": True})


@routes.delete("/api/printers/{id}")
@json_error
async def printer_delete(request):
    local_only(request)
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
    local_only(request)
    printers = app_of(request).settings["printers"]
    known_hosts = {c["host"] for c in printers}
    known_macs = {c.get("mac") for c in printers} - {None}
    found = await sweep(drivers.DRIVERS.values())
    table = await asyncio.to_thread(lan.neighbors)
    return web.json_response([{"host": h, "mac": table.get(h), "driver": d.id, "driverName": d.name,
                               "known": h in known_hosts or table.get(h) in known_macs}
                              for d, h in found])


@routes.post("/api/printers/{id}/send")
@json_error
async def printer_send(request):
    app = app_of(request)
    p = app.printer(request)
    body = await read_json(request)
    path = app.resolve(body.get("path"))
    return web.json_response(await p.send(path, start=bool(body.get("print"))))


@routes.post("/api/printers/{id}/{action:print|pause|resume|stop}")
@json_error
async def printer_action(request):
    p = app_of(request).printer(request)
    action = request.match_info["action"]
    if action == "print":
        filename = str((await read_json(request)).get("filename") or "")
        if not filename:
            raise PrinterError("Invalid request.", "bad_request")
        r = await p.command("start", filename)
    else:
        r = await p.command(action)
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
            if formats.supported(f) and f.is_file():
                st = f.stat()
                files.append({"path": str(f), "name": f.name, "folder": folder,
                              "size": st.st_size, "mtime": st.st_mtime})
    files.sort(key=lambda f: -f["mtime"])
    return web.json_response({"folders": app.settings["folders"], "files": files})


@routes.put("/api/library/folders")
@json_error
async def library_folders(request):
    local_only(request)  # a paired phone must not point the library at other folders
    app = app_of(request)
    folders = []
    raw = (await read_json(request)).get("folders", [])
    if not isinstance(raw, list):
        raise PrinterError("Invalid request.", "bad_request")
    for f in raw:
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
        if not formats.supported(name):
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
        raise PrinterError(str(e), "bad_file", detail=str(e)) from e
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
    png = await asyncio.to_thread(lambda: formats.png_bytes(c.preview_rgb(index)))
    return web.Response(body=png, content_type="image/png",
                        headers={"Cache-Control": "max-age=3600"})


@routes.get("/api/file/layer")
@json_error
async def file_layer(request):
    app = app_of(request)
    path = app.resolve(request.query.get("path"))
    c = await asyncio.to_thread(load_file, path)
    try:
        n = int(request.query.get("n", 0))
    except ValueError:
        n = -1
    if not 0 <= n < len(c.layers):
        raise web.HTTPBadRequest(text="invalid layer")
    png = await asyncio.to_thread(lambda: formats.png_bytes(c.layer_image(n)))
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
    app = web.Application(client_max_size=1 << 30, middlewares=[security_headers, guard])
    app[APP] = halot
    app.add_routes(routes)
    app.router.add_static("/static", STATIC)

    async def on_startup(_):
        await halot.start()

    async def on_cleanup(_):
        await halot.stop()

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    return app


async def browser_url(port):
    """resindeck.local when it resolves to this computer, so the address bar shows the same
    address phones use; 127.0.0.1 otherwise (no mDNS, or another computer took the name)."""
    if LAN:
        ip = await asyncio.to_thread(lan.resolve, lan.MDNS_NAME)
        if ip and (ipaddress.ip_address(ip).is_loopback or own_address(ip)):
            return f"http://{lan.MDNS_NAME}:{port}/"
    return f"http://127.0.0.1:{port}/"


async def run(port=8765, open_browser=True):
    url = f"http://127.0.0.1:{port}/"
    web_app = make_app()
    state = web_app[APP]
    state.port = port
    runner = web.AppRunner(web_app)
    await runner.setup()
    try:
        if LAN:
            # on purpose: phones reach it over the LAN; guard() requires the PIN for them
            await web.TCPSite(runner, "0.0.0.0", port).start()  # nosec B104
        else:
            await web.TCPSite(runner, "127.0.0.1", port).start()
    except OSError:
        await runner.cleanup()
        print(f"ResinDeck is already running at {url}")
        if open_browser:
            webbrowser.open(await browser_url(port))
        return
    announcer = lan.Announcer(port)
    ip = lan.local_ip() if LAN else None
    if ip:
        state.mdns = await announcer.start(ip)
    print(f"ResinDeck running at {url}  (Ctrl+C to quit)")
    for u in remote_urls(state)[1]:
        print(f"  on the network: {u}  (PIN {state.settings['pin']})")
    if open_browser:
        webbrowser.open(await browser_url(port) if state.mdns else url)
    try:
        await state.quit.wait()
    finally:
        await announcer.stop()
        await runner.cleanup()
