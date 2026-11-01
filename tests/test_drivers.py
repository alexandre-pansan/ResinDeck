"""The driver contract, and proof that a new driver works in ResinDeck without other changes."""
import asyncio

import pytest

import drivers
import server
from drivers import base
from drivers.base import Driver, PrinterError, Status, tcp_open


class DummyDriver(Driver):
    """A minimal driver, the kind a contributor would write for another brand."""
    id = "dummy"
    name = "Dummy Printer"
    models = "Dummy 1"
    extensions = (".goo",)
    password = False
    log = []

    async def connect(self):
        self.version = "9.9"
        self.state = base.IDLE
        self.file = ""

    async def close(self):
        pass

    async def ping(self):
        pass

    async def status(self):
        return Status(state=self.state, file=self.file, layer=3, layers=10, remaining=60, code=self.state.upper())

    async def upload(self, path, on_progress=None):
        on_progress(100)
        self.log.append(("upload", path.name))
        return {"stored": path.name}

    async def start(self, filename):
        self.state, self.file = base.PRINTING, filename
        return {"ok": True}

    async def pause(self):
        self.state = base.PAUSED
        return {"ok": True}

    async def resume(self):
        self.state = base.PRINTING
        return {"ok": True}

    async def stop(self):
        self.state = base.STOPPED
        return {"ok": True}

    @classmethod
    async def probe(cls, host):
        return host == "127.0.0.1"


# --- registry ------------------------------------------------------------------------

def test_registry():
    assert drivers.get("halot").name == "Creality HALOT"
    assert drivers.get(None) is drivers.get("") is drivers.DRIVERS[drivers.DEFAULT]
    with pytest.raises(PrinterError) as e:
        drivers.get("nope")
    assert e.value.code == "unknown_driver"


def test_every_driver_fills_in_the_contract():
    for d in drivers.DRIVERS.values():
        assert d.id and d.name and d.extensions, d
        assert all(ext.startswith(".") and ext == ext.lower() for ext in d.extensions)
        for method in ("connect", "close", "ping", "status", "upload", "start", "pause", "resume", "stop", "probe"):
            assert getattr(d, method) is not getattr(Driver, method), f"{d.id} does not implement {method}"


# --- base ----------------------------------------------------------------------------

def test_status():
    assert Status().ready and Status(state=base.FINISHED).ready
    assert not Status(state=base.PRINTING).ready
    assert "raw" not in Status(raw={"x": 1}).public()


async def test_base_methods_must_be_implemented():
    d = Driver("h")
    for method in ("connect", "close", "ping", "status", "start", "pause", "resume", "stop"):
        with pytest.raises(NotImplementedError):
            await (getattr(d, method)("x") if method == "start" else getattr(d, method)())
    with pytest.raises(NotImplementedError):
        await d.upload("x")
    with pytest.raises(NotImplementedError):
        await Driver.probe("h")


def test_password_helpers():
    class Limited(Driver):
        password_max = 4

    Limited.check_password("1234")
    with pytest.raises(PrinterError):
        Limited.check_password("12345")
    Driver.check_password("x" * 100)  # no limit
    with pytest.raises(PrinterError) as e:
        Driver("h").need_password()
    assert e.value.code == "no_password"
    DummyDriver("h").need_password()  # needs none


def test_describe():
    assert DummyDriver.describe() == {"id": "dummy", "name": "Dummy Printer", "models": "Dummy 1",
                                      "extensions": [".goo"], "password": False, "passwordMax": 0}


async def test_tcp_open():
    srv = await asyncio.start_server(lambda r, w: w.close(), "127.0.0.1", 0)
    port = srv.sockets[0].getsockname()[1]
    assert await tcp_open("127.0.0.1", port)
    srv.close()
    await srv.wait_closed()
    assert not await tcp_open("127.0.0.1", port)


# --- a new driver in the running app -------------------------------------------------

@pytest.fixture
async def app(aiohttp_client, tmp_path, monkeypatch):
    monkeypatch.setitem(drivers.DRIVERS, "dummy", DummyDriver)
    monkeypatch.setattr(server, "SETTINGS", tmp_path / "settings.json")
    monkeypatch.setattr(server, "POLL_INTERVAL", 0.05)
    (tmp_path / "lib").mkdir()
    (tmp_path / "lib" / "part.goo").write_bytes(b"goo")
    server.save_settings({"printers": [], "folders": [str(tmp_path / "lib")]})
    return await aiohttp_client(server.make_app())


async def test_new_driver_end_to_end(app, tmp_path, monkeypatch):
    listed = await (await app.get("/api/drivers")).json()
    assert {"halot", "dummy"} <= {d["id"] for d in listed}

    r = await app.post("/api/printers", json={"driver": "dummy", "host": "127.0.0.1"})
    pid = (await r.json())["id"]
    p = app.server.app[server.APP].printers[pid]
    while not (p.online and p.status):
        await asyncio.sleep(0.02)
    [public] = await (await app.get("/api/printers")).json()
    assert public["driverName"] == "Dummy Printer" and public["name"] == "Dummy Printer"
    assert public["version"] == "9.9" and public["status"]["state"] == "idle" and public["needsPassword"] is False

    monkeypatch.setattr(server.formats, "supported", lambda path: True)  # no reader for .goo yet
    r = await app.post(f"/api/printers/{pid}/send", json={"path": str(tmp_path / "lib" / "part.goo"), "print": True})
    assert await r.json() == {"stored": "part.goo", "start": {"ok": True}}
    assert ("upload", "part.goo") in DummyDriver.log
    await app.post(f"/api/printers/{pid}/pause")
    assert p.status.state == "paused"
    await app.post(f"/api/printers/{pid}/resume")
    await app.post(f"/api/printers/{pid}/stop")
    assert p.status.state == "stopped"

    import ipaddress

    from drivers import halot

    monkeypatch.setattr(server, "local_networks", lambda: [ipaddress.ip_network("127.0.0.0/30")])
    monkeypatch.setattr(halot, "PORT", 1)  # nothing there: only the dummy answers
    found = await (await app.post("/api/printers/scan")).json()
    assert [(f["host"], f["driver"]) for f in found] == [("127.0.0.1", "dummy")]


async def test_files_the_printer_does_not_accept(app, sliced, tmp_path):
    lib = tmp_path / "lib"
    (lib / "part.cxdlpv4").write_bytes(sliced.read_bytes())
    r = await app.post("/api/printers", json={"driver": "dummy", "host": "127.0.0.1"})
    pid = (await r.json())["id"]
    p = app.server.app[server.APP].printers[pid]
    while not p.online:
        await asyncio.sleep(0.02)
    r = await app.post(f"/api/printers/{pid}/send", json={"path": str(lib / "part.cxdlpv4")})
    body = await r.json()
    assert body["code"] == "file_not_supported" and body["params"]["ext"] == ".cxdlpv4"


async def test_unknown_driver_is_refused(app):
    r = await app.post("/api/printers", json={"driver": "nope", "host": "1.2.3.4"})
    assert (await r.json())["code"] == "unknown_driver"


def test_old_settings_get_the_halot_driver(tmp_path, monkeypatch):
    import json

    monkeypatch.setattr(server, "SETTINGS", tmp_path / "settings.json")
    server.SETTINGS.write_text(json.dumps({"printers": [{"id": "a", "name": "R6", "host": "1.2.3.4"}],
                                           "folders": []}))
    assert server.load_settings()["printers"][0]["driver"] == "halot"
