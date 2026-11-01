import asyncio
import json
import struct
from types import SimpleNamespace

import pytest

import server

PHONE_UA = ("Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/131.0 Mobile Safari/537.36")
PHONE = {"X-Test-Remote": "1", "User-Agent": PHONE_UA}


@pytest.fixture
def library(tmp_path, sliced):
    return sliced.parent


@pytest.fixture
async def client(aiohttp_client, tmp_path, monkeypatch, library):
    monkeypatch.setattr(server, "SETTINGS", tmp_path / "config" / "settings.json")
    monkeypatch.setattr(server, "CACHE", tmp_path / "cache")
    monkeypatch.setattr(server, "POLL_INTERVAL", 0.05)
    # the test client always connects from 127.0.0.1: requests carrying X-Test-Remote act as a
    # device on the network, everything else as this computer
    real_is_local = server.is_local
    monkeypatch.setattr(server, "is_local",
                        lambda r: "X-Test-Remote" not in r.headers and real_is_local(r))
    server.save_settings({"printers": [], "folders": [str(library)]})
    app = server.make_app()
    app[server.APP].port = 8765
    c = await aiohttp_client(app)
    c.state = app[server.APP]
    return c


async def pair(client, headers=PHONE):
    pin = client.state.settings["pin"]
    r = await client.post("/api/login", json={"pin": pin}, headers=headers)
    assert r.status == 200
    client.session.cookie_jar.clear()  # each simulated device sends only its own cookie
    return {**headers, "Cookie": f"{server.SESSION_COOKIE}={r.cookies[server.SESSION_COOKIE].value}"}


# --- helpers -------------------------------------------------------------------------

@pytest.mark.parametrize("host,ok", [
    ("127.0.0.1:8765", True), ("[::1]:8765", True), ("localhost:8765", True),
    ("resindeck.local:8765", True), ("192.168.0.5:8765", True), ("evil.example:8765", False),
    ("127.0.0.1.nip.io:8765", False),
])
def test_trusted_host(host, ok):
    assert server.trusted_host(host) is ok


def test_is_local_uses_own_addresses():
    assert server.is_local(SimpleNamespace(remote="127.0.0.1"))
    assert server.is_local(SimpleNamespace(remote="::1"))
    assert not server.is_local(SimpleNamespace(remote="192.0.2.1"))  # TEST-NET, never ours
    assert not server.is_local(SimpleNamespace(remote="not an ip"))


def test_own_address_is_cached(monkeypatch):
    server._own.clear()
    assert server.own_address("127.0.0.1")
    monkeypatch.setattr(server.socket, "socket", None)  # a second lookup would crash
    assert server.own_address("127.0.0.1")


@pytest.mark.parametrize("ua,name", [
    (PHONE_UA, "Android phone · Chrome"),
    ("Mozilla/5.0 (Linux; Android 13; SM-X200) AppleWebKit/537.36 Chrome/120 Safari/537.36", "Android tablet · Chrome"),
    ("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 Version/17.0 "
     "Mobile/15E148 Safari/604.1",
     "iPhone · Safari"),
    ("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:132.0) Gecko/20100101 Firefox/132.0", "Windows · Firefox"),
    ("", "Unknown device"),
])
def test_device_name(ua, name):
    assert server.device_name(ua) == name


def test_new_pin_is_six_digits():
    pins = {server.new_pin() for _ in range(50)}
    assert all(len(p) == 6 and p.isdigit() for p in pins) and len(pins) > 1


def test_settings_are_private(tmp_path, monkeypatch):
    monkeypatch.setattr(server, "SETTINGS", tmp_path / "s" / "settings.json")
    server.save_settings({"printers": [{"password": "x"}], "folders": []})
    assert server.SETTINGS.stat().st_mode & 0o077 == 0 or server.os.name == "nt"
    s = server.load_settings()
    assert len(s["pin"]) == 6 and s["sessions"] == []


def test_old_session_format_is_dropped(tmp_path, monkeypatch):
    monkeypatch.setattr(server, "SETTINGS", tmp_path / "settings.json")
    server.SETTINGS.write_text(json.dumps({"printers": [], "folders": [], "pin": "123456",
                                           "sessions": ["deadbeef", {"id": "a", "hash": "h"}]}))
    assert server.load_settings()["sessions"] == [{"id": "a", "hash": "h"}]


def test_cli_config_is_migrated(tmp_path, monkeypatch):
    monkeypatch.setattr(server, "SETTINGS", tmp_path / "none.json")
    monkeypatch.setattr(server, "load_config", lambda: {"HOST": "10.0.0.9", "PASSWORD": "pw"})
    s = server.load_settings()
    assert s["printers"][0]["host"] == "10.0.0.9" and s["printers"][0]["password"] == "pw"


# --- pages and headers ---------------------------------------------------------------

async def test_index_has_security_headers(client):
    r = await client.get("/")
    assert r.status == 200
    assert r.headers["X-Frame-Options"] == "DENY"
    assert r.headers["X-Content-Type-Options"] == "nosniff"
    csp = r.headers["Content-Security-Policy"]
    assert "frame-ancestors 'none'" in csp and "'sha256-" in csp and "'unsafe-eval'" not in csp


async def test_csp_hash_matches_the_import_map(client):
    import base64
    import hashlib
    import re

    html = (server.STATIC / "index.html").read_text(encoding="utf-8")
    script = re.search(r'<script type="importmap">(.*?)</script>', html, re.S).group(1)
    digest = base64.b64encode(hashlib.sha256(script.encode()).digest()).decode()
    assert f"'sha256-{digest}'" in server.content_security_policy()


async def test_static_files_are_served_with_headers(client):
    r = await client.get("/static/app.js")
    assert r.status == 200 and r.headers["X-Content-Type-Options"] == "nosniff"


async def test_errors_also_get_headers(client):
    r = await client.get("/api/file/info", params={"path": "/etc/passwd"})
    assert r.status == 403 and r.headers["X-Frame-Options"] == "DENY"


# --- access control ------------------------------------------------------------------

async def test_local_session(client):
    assert await (await client.get("/api/session")).json() == {"local": True, "authorized": True}


async def test_remote_needs_the_pin(client):
    assert await (await client.get("/api/session", headers=PHONE)).json() == {"local": False, "authorized": False}
    for method, path in [("GET", "/api/printers"), ("GET", "/api/library"), ("GET", "/ws"),
                         ("POST", "/api/printers/x/stop")]:
        r = await client.request(method, path, headers=PHONE)
        assert r.status == 401, path
        assert (await r.json())["code"] == "pin_required"


async def test_pairing(client):
    headers = await pair(client)
    assert (await (await client.get("/api/session", headers=headers)).json())["authorized"]
    assert (await client.get("/api/printers", headers=headers)).status == 200
    [device] = client.state.device_list()
    assert device["name"] == "Android phone · Chrome"
    assert device["seen"] is not None
    stored = client.state.settings["sessions"][0]
    assert headers["Cookie"].split("=", 1)[1] not in json.dumps(stored)  # only the hash is kept


async def test_wrong_pin_and_rate_limit(client):
    pin = client.state.settings["pin"]
    wrong = "000000" if pin != "000000" else "111111"
    for _ in range(5):
        r = await client.post("/api/login", json={"pin": wrong}, headers=PHONE)
        assert (await r.json())["code"] == "pin_wrong"
    r = await client.post("/api/login", json={"pin": pin}, headers=PHONE)
    assert (await r.json())["code"] == "pin_wait"


@pytest.mark.parametrize("body", ["not json", "[1, 2]", '"text"'])
async def test_login_rejects_bad_bodies(client, body):
    r = await client.post("/api/login", data=body, headers={**PHONE, "Content-Type": "application/json"})
    assert r.status == 400 and (await r.json())["code"] == "bad_request"


async def test_session_cookie_flags(client):
    pin = client.state.settings["pin"]
    r = await client.post("/api/login", json={"pin": pin}, headers=PHONE)
    cookie = r.cookies[server.SESSION_COOKIE]
    assert cookie["httponly"] and cookie["samesite"] == "Strict"


async def test_paired_devices_cannot_manage_the_computer(client, library):
    headers = await pair(client)
    for method, path, body in [
        ("GET", "/api/remote", None), ("GET", "/api/remote/qr.svg", None), ("POST", "/api/remote/pin", None),
        ("DELETE", "/api/remote/devices/x", None), ("POST", "/api/quit", None),
        ("PUT", "/api/library/folders", {"folders": ["/"]}),
        ("POST", "/api/printers", {"host": "10.0.0.2"}), ("PUT", "/api/printers/x", {"host": "1"}),
        ("DELETE", "/api/printers/x", None), ("POST", "/api/printers/scan", None),
    ]:
        r = await client.request(method, path, json=body, headers=headers)
        assert r.status == 403, path
    assert client.state.settings["folders"] == [str(library)]


async def test_disconnect_one_device(client):
    headers = await pair(client)
    other = await pair(client, {**PHONE, "User-Agent": "Mozilla/5.0 (iPhone) Safari/604.1"})
    ws = await client.ws_connect("/ws", headers=headers)
    assert (await ws.receive_json())["type"] == "printers"
    devices = (await (await client.get("/api/remote")).json())["devices"]
    phone = next(d for d in devices if d["name"].startswith("Android"))
    assert phone["online"]

    assert (await client.delete(f"/api/remote/devices/{phone['id']}")).status == 200
    msg = await ws.receive()
    assert msg.type.name in ("CLOSE", "CLOSED", "CLOSING")
    assert (await client.get("/api/printers", headers=headers)).status == 401
    assert (await client.get("/api/printers", headers=other)).status == 200


async def test_new_pin_signs_everyone_out(client):
    headers = await pair(client)
    r = await client.post("/api/remote/pin")
    new = (await r.json())["pin"]
    assert len(new) == 6 and new == client.state.settings["pin"]
    assert client.state.settings["sessions"] == []
    assert (await client.get("/api/printers", headers=headers)).status == 401


async def test_remote_info_and_qr(client, monkeypatch):
    monkeypatch.setattr(server.lan, "local_ip", lambda: "192.168.1.20")
    client.state.mdns = True
    info = await (await client.get("/api/remote")).json()
    assert info["enabled"] and info["pin"] == client.state.settings["pin"]
    assert info["urls"] == ["http://resindeck.local:8765/", "http://192.168.1.20:8765/"]
    r = await client.get("/api/remote/qr.svg")
    assert r.status == 200 and r.content_type == "image/svg+xml"
    svg = await r.text()
    assert svg.startswith("<?xml") and "xmlns" in svg
    assert client.state.settings["pin"] not in svg  # the PIN is typed, never in the QR


async def test_remote_disabled_without_network(client, monkeypatch):
    monkeypatch.setattr(server.lan, "local_ip", lambda: None)
    info = await (await client.get("/api/remote")).json()
    assert not info["enabled"] and info["urls"] == []
    assert (await client.get("/api/remote/qr.svg")).status == 404


# --- browser attacks -----------------------------------------------------------------

async def test_dns_rebinding_is_blocked(client):
    r = await client.get("/api/printers", headers={"Host": "attacker.example:8765"})
    assert r.status == 403


@pytest.mark.parametrize("method,path", [("POST", "/api/printers/x/stop"), ("GET", "/api/library")])
async def test_cross_origin_requests_are_blocked(client, method, path):
    r = await client.request(method, path, headers={"Origin": "http://evil.example"})
    assert r.status == 403


async def test_cross_site_websocket_is_blocked(client):
    import aiohttp

    with pytest.raises(aiohttp.WSServerHandshakeError) as e:
        await client.ws_connect("/ws", headers={"Origin": "http://evil.example"})
    assert e.value.status == 403


async def test_same_origin_requests_pass(client):
    host = f"{client.server.host}:{client.server.port}"
    r = await client.get("/api/library", headers={"Origin": f"http://{host}"})
    assert r.status == 200


# --- library -------------------------------------------------------------------------

async def test_library_lists_sliced_files(client, library):
    (library / "notes.txt").write_text("ignored")
    data = await (await client.get("/api/library")).json()
    assert data["folders"] == [str(library)]
    assert [f["name"] for f in data["files"]] == ["part.cxdlpv4"]


async def test_set_folders(client, tmp_path):
    other = tmp_path / "other"
    other.mkdir()
    r = await client.put("/api/library/folders", json={"folders": [str(other), str(other)]})
    assert (await r.json())["folders"] == [str(other.resolve())]
    r = await client.put("/api/library/folders", json={"folders": [str(tmp_path / "missing")]})
    assert (await r.json())["code"] == "folder_missing"
    r = await client.put("/api/library/folders", json={"folders": "not a list"})
    assert (await r.json())["code"] == "bad_request"


@pytest.mark.parametrize("path", ["/etc/passwd", "{lib}/../outside.cxdlpv4", "{lib}/link.cxdlpv4", ""])
async def test_files_outside_the_library_are_refused(client, library, tmp_path, path):
    outside = tmp_path.parent / "outside.cxdlpv4"
    outside.write_bytes(b"x")
    try:
        (library / "link.cxdlpv4").symlink_to(outside)
    except OSError:  # Windows without the symlink privilege
        if "link" in path:
            pytest.skip("symlinks not available")
    r = await client.get("/api/file/info", params={"path": path.format(lib=library)})
    assert r.status in (400, 403)


async def test_missing_file(client, library):
    r = await client.get("/api/file/info", params={"path": str(library / "gone.cxdlpv4")})
    assert r.status == 404


def multipart(filename, content):
    """A multipart body as browsers send it (aiohttp's FormData would escape the slashes)."""
    boundary = "resindeckboundary"
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{filename}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n").encode() + content + f"\r\n--{boundary}--\r\n".encode()
    return body, {"Content-Type": f"multipart/form-data; boundary={boundary}"}


async def test_upload(client, library, sliced):
    body, headers = multipart("../../evil.cxdlpv4", sliced.read_bytes())
    r = await client.post("/api/library/upload", data=body, headers=headers)
    saved = (await r.json())["saved"]
    assert saved == [str(library / "evil.cxdlpv4")]  # the path in the name is dropped
    assert not list(library.glob("*.part"))


async def test_upload_only_sliced_files(client):
    import aiohttp

    data = aiohttp.FormData()
    data.add_field("file", b"#!/bin/sh", filename="run.sh")
    r = await client.post("/api/library/upload", data=data)
    assert (await r.json())["code"] == "only_cxdlp"


async def test_upload_needs_a_folder(client):
    import aiohttp

    client.state.settings["folders"] = []
    data = aiohttp.FormData()
    data.add_field("file", b"x", filename="a.cxdlpv4")
    assert (await (await client.post("/api/library/upload", data=data)).json())["code"] == "no_folder"


# --- files ---------------------------------------------------------------------------

async def test_file_info(client, sliced):
    info = await (await client.get("/api/file/info", params={"path": str(sliced)})).json()
    assert info["layerCount"] == 12 and len(info["layers"]) == 12 and info["hasVoxels"] is False


async def test_file_info_bad_file(client, library):
    (library / "junk.cxdlpv4").write_bytes(b"\0\0\0\x04JUNK" + b"\0" * 200)
    r = await client.get("/api/file/info", params={"path": str(library / "junk.cxdlpv4")})
    assert r.status == 400 and (await r.json())["code"] == "bad_file"


@pytest.mark.parametrize("size", ["small", "large"])
async def test_preview(client, sliced, size):
    r = await client.get("/api/file/preview", params={"path": str(sliced), "size": size})
    assert r.status == 200 and r.content_type == "image/png"
    assert (await r.read())[:4] == b"\x89PNG"


@pytest.mark.parametrize("n,status", [("0", 200), ("11", 200), ("12", 400), ("-1", 400), ("abc", 400)])
async def test_layer(client, sliced, n, status):
    r = await client.get("/api/file/layer", params={"path": str(sliced), "n": n})
    assert r.status == status


async def test_voxels_are_built_once_and_cached(client, sliced, monkeypatch):
    r = await client.get("/api/file/voxels", params={"path": str(sliced)})
    payload = await r.read()
    (meta_len,) = struct.unpack("<I", payload[:4])
    meta = json.loads(payload[4:4 + meta_len])
    assert meta["shape"] and len(payload) > 4 + meta_len
    assert server.voxel_cache_path(sliced).exists()

    monkeypatch.setattr(server, "load_file", lambda p: pytest.fail("rebuilt instead of cached"))
    again = await (await client.get("/api/file/voxels", params={"path": str(sliced)})).read()
    assert again == payload


# --- printers ------------------------------------------------------------------------

async def wait_for(cond, timeout=5):
    async def loop():
        while not cond():
            await asyncio.sleep(0.02)
    await asyncio.wait_for(loop(), timeout)


async def add_printer(client, password="secret1"):
    r = await client.post("/api/printers", json={"name": "R6", "host": "127.0.0.1", "password": password})
    pid = (await r.json())["id"]
    p = client.state.printers[pid]
    await wait_for(lambda: p.online and (p.status or p.error))
    return pid, p


async def test_printer_crud_and_status(client, printer):
    pid, p = await add_printer(client)
    [public] = await (await client.get("/api/printers")).json()
    assert public["online"] and public["hasPassword"] and public["status"]["state"] == "idle"
    assert public["driver"] == "halot" and public["driverName"] == "Creality HALOT"
    assert "password" not in json.dumps(public).replace("hasPassword", "")

    r = await client.put(f"/api/printers/{pid}", json={"name": "Renamed", "host": "127.0.0.1"})
    assert r.status == 200
    assert client.state.settings["printers"][0]["password"] == "secret1"  # kept when left empty
    assert client.state.settings["printers"][0]["name"] == "Renamed"

    r = await client.put(f"/api/printers/{pid}", json={"host": "127.0.0.1", "clearPassword": True})
    assert client.state.settings["printers"][0]["password"] == ""

    assert (await client.delete(f"/api/printers/{pid}")).status == 200
    assert await (await client.get("/api/printers")).json() == []


async def test_printer_validation(client):
    r = await client.post("/api/printers", json={"name": "x"})
    assert (await r.json())["code"] == "no_host"
    r = await client.post("/api/printers", json={"host": "1.2.3.4", "password": "123456789"})
    assert (await r.json())["code"] == "password_length"
    assert (await client.put("/api/printers/nope", json={"host": "1"})).status == 404


async def test_printer_without_password(client, printer):
    pid, p = await add_printer(client, password="")
    assert p.error == "no_password"


async def test_printer_offline(client):
    r = await client.post("/api/printers", json={"host": "127.0.0.1", "password": "x"})
    pid = (await r.json())["id"]
    p = client.state.printers[pid]
    await wait_for(lambda: p.error == "no_connection")
    r = await client.post(f"/api/printers/{pid}/pause")
    assert (await r.json())["code"] == "offline"


async def test_send_and_print(client, printer, sliced, monkeypatch):
    from drivers import halot

    monkeypatch.setattr(halot, "CHUNK", 1024)
    pid, p = await add_printer(client)
    r = await client.post(f"/api/printers/{pid}/send", json={"path": str(sliced), "print": True})
    body = await r.json()
    assert r.status == 200, body
    assert int(body["checkstate"]) == 1 and body["start"]["errorcode"] == "0"
    assert printer.files["part.cxdlpv4"] == sliced.read_bytes()
    assert printer.status["printStatus"] == "PRINT_PROCESSING"

    await wait_for(lambda: p.status and p.status.state == "printing")
    r = await client.post(f"/api/printers/{pid}/send", json={"path": str(sliced)})
    assert (await r.json())["code"] == "printer_busy"


async def test_send_outside_the_library(client, printer, tmp_path):
    pid, _ = await add_printer(client)
    r = await client.post(f"/api/printers/{pid}/send", json={"path": "/etc/hostname"})
    assert r.status == 403


async def test_print_pause_stop(client, printer):
    pid, p = await add_printer(client)
    r = await client.post(f"/api/printers/{pid}/print", json={"filename": "part.cxdlpv4"})
    assert r.status == 200 and p.status.state == "printing"
    await client.post(f"/api/printers/{pid}/pause")
    assert p.status.state == "paused"
    await client.post(f"/api/printers/{pid}/resume")
    await client.post(f"/api/printers/{pid}/stop")
    assert p.status.state == "stopped"
    r = await client.post(f"/api/printers/{pid}/print", json={})
    assert (await r.json())["code"] == "bad_request"


async def test_paired_phone_can_control_prints(client, printer):
    pid, p = await add_printer(client)
    headers = await pair(client)
    r = await client.post(f"/api/printers/{pid}/pause", headers=headers)
    assert r.status == 200


async def test_websocket_pushes_printer_updates(client, printer):
    ws = await client.ws_connect("/ws")
    assert (await ws.receive_json())["printers"] == []
    await client.post("/api/printers", json={"host": "127.0.0.1", "password": "secret1"})

    async def until_online():
        while True:
            msg = await ws.receive_json()
            if msg["type"] == "printers" and msg["printers"] and msg["printers"][0]["online"]:
                return msg
    assert await asyncio.wait_for(until_online(), 5)
    await ws.close()


async def test_scan_finds_the_printer(client, printer, monkeypatch):
    import ipaddress

    monkeypatch.setattr(server, "local_networks", lambda: [ipaddress.ip_network("127.0.0.0/30")])
    found = await (await client.post("/api/printers/scan")).json()
    assert {"host": "127.0.0.1", "driver": "halot", "driverName": "Creality HALOT", "known": False} in found


def test_local_networks(monkeypatch):
    monkeypatch.setattr(server.lan, "local_ip", lambda: "192.168.15.13")
    assert [str(n) for n in server.local_networks()] == ["192.168.15.0/24"]
    monkeypatch.setattr(server.lan, "local_ip", lambda: None)
    assert server.local_networks() == []


# --- quit / run ----------------------------------------------------------------------

async def test_quit(client):
    assert (await client.post("/api/quit")).status == 200
    await asyncio.wait_for(client.state.quit.wait(), 2)


async def test_run_serves_until_quit(tmp_path, monkeypatch, unused_tcp_port):
    import aiohttp

    monkeypatch.setattr(server, "SETTINGS", tmp_path / "settings.json")
    monkeypatch.setattr(server, "LAN", False)
    opened = []
    monkeypatch.setattr(server.webbrowser, "open", opened.append)
    task = asyncio.create_task(server.run(unused_tcp_port, open_browser=True))
    url = f"http://127.0.0.1:{unused_tcp_port}"
    async with aiohttp.ClientSession() as s:
        for _ in range(100):
            try:
                async with s.get(f"{url}/api/session") as r:
                    assert (await r.json())["local"]
                    break
            except aiohttp.ClientConnectionError:
                await asyncio.sleep(0.05)
        # a second instance only opens the browser
        await server.run(unused_tcp_port, open_browser=True)
        async with s.post(f"{url}/api/quit") as r:
            assert r.status == 200
    await asyncio.wait_for(task, 5)
    assert opened == [f"{url}/", f"{url}/"]


async def test_browser_url(monkeypatch):
    monkeypatch.setattr(server, "LAN", True)
    monkeypatch.setattr(server.lan, "resolve", lambda name: "127.0.0.1")
    assert await server.browser_url(8765) == "http://resindeck.local:8765/"
    monkeypatch.setattr(server.lan, "resolve", lambda name: "192.0.2.1")  # someone else's
    assert await server.browser_url(8765) == "http://127.0.0.1:8765/"
    monkeypatch.setattr(server.lan, "resolve", lambda name: None)
    assert await server.browser_url(8765) == "http://127.0.0.1:8765/"
    monkeypatch.setattr(server, "LAN", False)
    assert await server.browser_url(8765) == "http://127.0.0.1:8765/"
