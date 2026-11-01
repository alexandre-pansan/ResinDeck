import ipaddress
import socket
import time

import lan


def test_local_ip_is_an_ipv4_address_or_none():
    ip = lan.local_ip()
    assert ip is None or ipaddress.ip_address(ip).version == 4


def test_local_ip_without_network(monkeypatch):
    class NoRoute:
        def __init__(self, *a):
            pass

        def connect(self, addr):
            raise OSError("network unreachable")

        def close(self):
            pass

    monkeypatch.setattr(lan.socket, "socket", NoRoute)
    assert lan.local_ip() is None


def test_resolve(monkeypatch):
    monkeypatch.setattr(lan.socket, "gethostbyname", lambda name: "10.1.2.3")
    assert lan.resolve("printer.local") == "10.1.2.3"


def test_resolve_unknown_name(monkeypatch):
    def fail(name):
        raise socket.gaierror("unknown")

    monkeypatch.setattr(lan.socket, "gethostbyname", fail)
    assert lan.resolve("nothing.local") is None


def test_resolve_gives_up_after_the_timeout(monkeypatch):
    monkeypatch.setattr(lan.socket, "gethostbyname", lambda name: time.sleep(2) or "10.0.0.1")
    t = time.monotonic()
    assert lan.resolve("slow.local", timeout=0.1) is None
    assert time.monotonic() - t < 1


async def test_announcer_failure_is_not_fatal(monkeypatch, capsys):
    import zeroconf.asyncio

    def broken(*a, **k):
        raise OSError("port 5353 in use")

    monkeypatch.setattr(zeroconf.asyncio, "AsyncZeroconf", broken)
    a = lan.Announcer(8765)
    assert await a.start("192.0.2.10") is False
    assert "mDNS unavailable" in capsys.readouterr().out
    await a.stop()


async def test_announcer_registers_and_unregisters(monkeypatch):
    import zeroconf.asyncio

    calls = []

    class FakeZeroconf:
        def __init__(self, **kw):
            calls.append(("init", kw))

        async def async_register_service(self, info, allow_name_change):
            calls.append(("register", info.server, info.port))

        async def async_unregister_service(self, info):
            calls.append(("unregister",))

        async def async_close(self):
            calls.append(("close",))

    monkeypatch.setattr(zeroconf.asyncio, "AsyncZeroconf", FakeZeroconf)
    a = lan.Announcer(8765)
    assert await a.start("192.0.2.10") is True
    await a.stop()
    assert ("register", "resindeck.local.", 8765) in calls
    assert calls[-2:] == [("unregister",), ("close",)]
    assert a.zc is None
