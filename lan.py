"""Local network helpers: this machine's address and the resindeck.local mDNS announcement."""
import socket
import threading

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
