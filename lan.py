"""Local network helpers: this machine's address, the ARP table (to find printers by MAC
address) and the resindeck.local mDNS announcement."""
import re
import socket
import subprocess  # nosec B404
import sys
import threading
from pathlib import Path

MDNS_NAME = "resindeck.local"


def local_ip():
    """The address other devices on the LAN reach this machine at (no packet is sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def resolve(name, timeout=2.0):
    """IPv4 address of a host name, or None if it does not resolve within the timeout."""
    result = []

    def lookup():
        try:
            result.append(socket.gethostbyname(name))
        except OSError:
            pass

    t = threading.Thread(target=lookup, daemon=True)
    t.start()
    t.join(timeout)
    return result[0] if result else None


MAC = re.compile(r"([0-9a-f]{1,2}(?:[:-][0-9a-f]{1,2}){5})", re.I)
IPV4 = re.compile(r"(\d{1,3}(?:\.\d{1,3}){3})")


def normalize_mac(mac):
    """'FC-EE-32-2-B5-57' -> 'fc:ee:32:02:b5:57'; None for empty or broadcast addresses."""
    if not mac:
        return None
    mac = ":".join(part.zfill(2) for part in re.split("[:-]", mac.lower()))
    if mac == "00:00:00:00:00:00" or int(mac[:2], 16) & 1:  # empty, broadcast or multicast
        return None
    return mac


def parse_neighbors(text):
    """IP -> MAC from `arp -a` (Windows, macOS, BSD) or /proc/net/arp (Linux)."""
    table = {}
    for line in text.splitlines():
        ip, mac = IPV4.search(line), MAC.search(line)
        if ip and mac and (mac_norm := normalize_mac(mac.group(1))):
            if " 0x0 " in line:  # /proc/net/arp: incomplete entry
                continue
            table[ip.group(1)] = mac_norm
    return table


def neighbors():
    """This computer's ARP table: the MAC address of every device it talked to recently.
    Only works for devices on the same network segment, which is the case for printers."""
    proc = Path("/proc/net/arp")
    try:
        if proc.exists():
            return parse_neighbors(proc.read_text())
        # only the system's own `arp`, with fixed arguments (bandit B603/B607 are fine here)
        flags = {"creationflags": 0x08000000} if sys.platform == "win32" else {}  # CREATE_NO_WINDOW
        out = subprocess.run(["arp", "-a"], capture_output=True, text=True, timeout=5,  # nosec B603 B607
                             check=False, **flags).stdout
        return parse_neighbors(out)
    except (OSError, subprocess.SubprocessError):
        return {}


def mac_of(ip):
    return neighbors().get(ip)


class Announcer:
    """Answers mDNS queries for resindeck.local and advertises an _http._tcp service."""

    def __init__(self, port):
        self.port = port
        self.zc = None
        self.info = None

    async def start(self, ip):
        try:
            from zeroconf import IPVersion, ServiceInfo
            from zeroconf.asyncio import AsyncZeroconf

            self.info = ServiceInfo(
                "_http._tcp.local.", "ResinDeck._http._tcp.local.",
                addresses=[socket.inet_aton(ip)], port=self.port,
                server=MDNS_NAME + ".", properties={"path": "/"})
            self.zc = AsyncZeroconf(ip_version=IPVersion.V4Only)
            await self.zc.async_register_service(self.info, allow_name_change=True)
            return True
        except Exception as e:  # mDNS is a convenience: the IP address and QR code still work
            print(f"mDNS unavailable ({e!r}); use the IP address instead")
            await self.stop()
            return False

    async def stop(self):
        if self.zc:
            try:
                if self.info:
                    await self.zc.async_unregister_service(self.info)
                await self.zc.async_close()
            except Exception:
                pass
            self.zc = None
