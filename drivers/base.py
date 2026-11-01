"""The contract every printer driver follows.

A driver is a subclass of `Driver` in its own module under drivers/, listed in
drivers/__init__.py. The rest of ResinDeck (web server, interface, command line) only talks to
this interface, so supporting a new printer family means writing one module; nothing else
changes. CONTRIBUTING.md has a step-by-step guide and drivers/halot.py is a complete example.
"""
import asyncio
from dataclasses import asdict, dataclass, field

# Printer states. Drivers translate whatever their printer reports into one of these.
IDLE = "idle"
PRINTING = "printing"
PAUSING = "pausing"
PAUSED = "paused"
STOPPING = "stopping"
STOPPED = "stopped"
FINISHING = "finishing"
FINISHED = "finished"
FAILED = "failed"
UNKNOWN = "unknown"

STATES = (IDLE, PRINTING, PAUSING, PAUSED, STOPPING, STOPPED, FINISHING, FINISHED, FAILED, UNKNOWN)
READY_STATES = (IDLE, STOPPED, FINISHED, FAILED)  # a new file can be sent and printed


class PrinterError(Exception):
    """An error meant for the user. `code` and `params` let the web UI translate it
    (keys "errors.<code>" in static/i18n.js)."""

    def __init__(self, message, code=None, **params):
        super().__init__(message)
        self.code = code
        self.params = params


@dataclass
class Status:
    """What the interface shows for a printer, the same for every driver."""
    state: str = IDLE
    file: str = ""        # name of the file being printed, as stored on the printer
    layer: int = 0        # current layer, 1-based; 0 when unknown
    layers: int = 0       # total layers; 0 when unknown
    remaining: int = 0    # seconds left; 0 when unknown
    code: str = ""        # the printer's own state name, shown when `state` is UNKNOWN
    raw: dict = field(default_factory=dict)  # the printer's full reply, for `resindeck raw-status`

    @property
    def ready(self):
        return self.state in READY_STATES

    def public(self):
        """For the web interface (without the raw reply)."""
        d = asdict(self)
        d.pop("raw")
        return d


class Driver:
    """One connection to one printer. Used as `async with Driver(host, password) as d: ...`.

    Class attributes describe the printer family; methods talk to one printer. Methods raise
    PrinterError for problems the user can fix (wrong password, printer busy) and let network
    errors (OSError, asyncio.TimeoutError, ...) propagate: the caller reconnects.
    """
    id = ""                      # short identifier stored in settings.json, e.g. "halot"
    name = ""                    # shown in the interface, e.g. "Creality HALOT"
    models = ""                  # models known to work, for the interface and docs
    extensions = ()              # file types the printer accepts, e.g. (".cxdlpv4",)
    password = True              # whether the printer needs a password for status and commands
    password_max = 0             # longest accepted password (0: no limit)

    def __init__(self, host, password=""):
        self.host = host
        self.secret = password
        self.version = None      # firmware version, when connect() can tell

    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, *exc):
        await self.close()

    # --- to implement ----------------------------------------------------------------

    async def connect(self):
        """Open the connection; set self.version if the printer reports it."""
        raise NotImplementedError

    async def close(self):
        raise NotImplementedError

    async def ping(self):
        """Check the printer still answers, without a password (used while none is set)."""
        raise NotImplementedError

    async def status(self) -> Status:
        raise NotImplementedError

    async def upload(self, path, on_progress=None):
        """Send a file to the printer. Call on_progress(percent) as it goes. Returns a dict
        with whatever the printer answered (shown to the user as is)."""
        raise NotImplementedError

    async def start(self, filename):
        """Start printing a file already on the printer."""
        raise NotImplementedError

    async def pause(self):
        raise NotImplementedError

    async def resume(self):
        raise NotImplementedError

    async def stop(self):
        raise NotImplementedError

    @classmethod
    async def probe(cls, host):
        """True if a printer of this family answers at `host`. Used by "Search the network",
        so it must be quick (about a second) and must not need the password."""
        raise NotImplementedError

    # --- helpers ---------------------------------------------------------------------

    @classmethod
    def check_password(cls, password):
        """Raise PrinterError if the printer cannot accept this password."""
        if cls.password_max and len(password) > cls.password_max:
            raise PrinterError(f"The password has at most {cls.password_max} characters.",
                               "password_length", max=cls.password_max)

    def need_password(self):
        if self.password and not self.secret:
            raise PrinterError("The printer password is not set.", "no_password")

    @classmethod
    def describe(cls):
        """For the web interface's printer dialog."""
        return {"id": cls.id, "name": cls.name, "models": cls.models, "extensions": list(cls.extensions),
                "password": cls.password, "passwordMax": cls.password_max}


async def tcp_open(host, port, timeout=0.6):
    """True if something accepts TCP connections at host:port. Cheap first step for probe()."""
    try:
        _, w = await asyncio.wait_for(asyncio.open_connection(host, port), timeout)
        w.close()
        return True
    except (OSError, asyncio.TimeoutError):
        return False
