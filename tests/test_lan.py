import ipaddress
import socket
import time
import types

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


# --- ARP table -----------------------------------------------------------------------

LINUX = """IP address       HW type     Flags       HW address            Mask     Device
192.168.15.8     0x1         0x2         fc:ee:32:02:b5:57     *        enp6s0
192.168.15.9     0x1         0x0         00:00:00:00:00:00     *        enp6s0
192.168.15.1     0x1         0x2         44:89:6D:41:81:10     *        enp6s0
"""
WINDOWS = """
Interface: 192.168.15.13 --- 0x9
  Internet Address      Physical Address      Type
  192.168.15.8          fc-ee-32-02-b5-57     dynamic
  192.168.15.255        ff-ff-ff-ff-ff-ff     static
  224.0.0.22            01-00-5e-00-00-16     static
"""
MACOS = """? (192.168.15.8) at fc:ee:32:2:b5:57 on en0 ifscope [ethernet]
? (192.168.15.20) at (incomplete) on en0 ifscope [ethernet]
"""


def test_parse_linux_proc_arp():
    assert lan.parse_neighbors(LINUX) == {"192.168.15.8": "fc:ee:32:02:b5:57", "192.168.15.1": "44:89:6d:41:81:10"}


def test_parse_windows_arp():
    table = lan.parse_neighbors(WINDOWS)
    assert table["192.168.15.8"] == "fc:ee:32:02:b5:57"
    assert "192.168.15.255" not in table  # broadcast
    assert "224.0.0.22" not in table      # multicast
    assert "192.168.15.13" not in table   # the interface line has no MAC


def test_parse_macos_arp():
    assert lan.parse_neighbors(MACOS) == {"192.168.15.8": "fc:ee:32:02:b5:57"}  # pads "2" to "02"


def test_normalize_mac():
    assert lan.normalize_mac("FC-EE-32-2-B5-57") == "fc:ee:32:02:b5:57"
    assert lan.normalize_mac("") is None and lan.normalize_mac("00:00:00:00:00:00") is None


def test_neighbors_reads_proc_on_linux(tmp_path, monkeypatch):
    arp = tmp_path / "arp"
    arp.write_text(LINUX)
    monkeypatch.setattr(lan, "Path", lambda p: arp)
    assert lan.mac_of("192.168.15.8") == "fc:ee:32:02:b5:57"
    assert lan.mac_of("10.0.0.1") is None


def test_neighbors_runs_arp_elsewhere(tmp_path, monkeypatch):
    monkeypatch.setattr(lan, "Path", lambda p: tmp_path / "missing")
    calls = []

    def fake_run(cmd, **kw):
        calls.append(cmd)
        return types.SimpleNamespace(stdout=WINDOWS)

    monkeypatch.setattr(lan.subprocess, "run", fake_run)
    assert lan.neighbors()["192.168.15.8"] == "fc:ee:32:02:b5:57"
    assert calls == [["arp", "-a"]]


def test_neighbors_without_arp(tmp_path, monkeypatch):
    monkeypatch.setattr(lan, "Path", lambda p: tmp_path / "missing")

    def missing(cmd, **kw):
        raise FileNotFoundError("arp")

    monkeypatch.setattr(lan.subprocess, "run", missing)
    assert lan.neighbors() == {}
