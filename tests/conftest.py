import os
import sys
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))

# resindeck/server compute their config paths at import time: point them at a throwaway home
# before anything imports them, so tests never touch the real ~/.config/resindeck
_home = tempfile.mkdtemp(prefix="resindeck-test-home-")
os.environ["HOME"] = os.environ["USERPROFILE"] = _home
for var in ("RESINDECK_HOST", "RESINDECK_PASSWORD", "RESINDECK_LAN", "RESINDECK_PORT"):
    os.environ.pop(var, None)

from cxdlp_writer import write_cxdlp  # noqa: E402
from fakeprinter import FakePrinter  # noqa: E402


@pytest.fixture
def sliced(tmp_path):
    """A small synthetic .cxdlpv4 file: a 60x40 px plate with a box that grows over 12 layers."""
    return write_cxdlp(tmp_path / "part.cxdlpv4")


@pytest.fixture
async def printer(monkeypatch):
    """A fake HALOT on 127.0.0.1; halot.PORT is pointed at it."""
    import halot

    fake = FakePrinter()
    await fake.start()
    monkeypatch.setattr(halot, "PORT", fake.port)
    yield fake
    await fake.stop()
