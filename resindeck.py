"""ResinDeck: send sliced files to resin printers over the network and control them.

Usage:
  resindeck version
  resindeck status
  resindeck send FILE.cxdlpv4 [--print]
  resindeck print NAME_ON_PRINTER.cxdlpv4
  resindeck pause | stop
  resindeck [ui]           (web interface in the browser)
Config: ~/.config/resindeck/config  (HOST=..., PASSWORD=...)
"""
import argparse
import asyncio
import json
import os
import shutil
import sys
from pathlib import Path

import websockets

__version__ = "0.2.0"

sys.path.insert(0, str(Path(__file__).resolve().parent))  # start.sh runs Python with -I
from halot import Halot, PrinterError, fmt_status  # noqa: E402

CONFIG_DIR = Path.home() / ".config" / "resindeck"
CONFIG = CONFIG_DIR / "config"
OLD_CONFIG_DIR = Path.home() / ".config" / "halot-send"  # name before the rename


def migrate_config():
    if OLD_CONFIG_DIR.is_dir() and not CONFIG_DIR.exists():
        shutil.copytree(OLD_CONFIG_DIR, CONFIG_DIR)


def load_config():
    cfg = {}
    if CONFIG.exists():
        for line in CONFIG.read_text().splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    cfg["HOST"] = os.environ.get("RESINDECK_HOST", cfg.get("HOST", ""))
    cfg["PASSWORD"] = os.environ.get("RESINDECK_PASSWORD", cfg.get("PASSWORD", ""))
    return cfg


async def main():
    ap = argparse.ArgumentParser(
        prog="resindeck", description="Send files to resin printers over the network and control them.")
    ap.add_argument("--host", help="printer IP (default: from config)")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("version", help="firmware version (no password needed)")
    sub.add_parser("status", help="print status")
    s = sub.add_parser("send", help="upload a .cxdlpv4 to the printer")
    s.add_argument("file", type=Path)
    s.add_argument("--print", action="store_true", help="start printing after the upload")
    s.add_argument("-y", "--yes", action="store_true", help="do not ask for confirmation before printing")
    p = sub.add_parser("print", help="print a file that is already on the printer")
    p.add_argument("filename")
    p.add_argument("-y", "--yes", action="store_true")
    sub.add_parser("pause", help="pause / resume")
    sub.add_parser("stop", help="cancel the print")
    sub.add_parser("raw-status", help="raw status reply (debug)")
    u = sub.add_parser("ui", help="open the web interface in the browser (default)")
    u.add_argument("--port", type=int, default=8765)
    u.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    migrate_config()

    if a.cmd in (None, "ui"):
        import server
        await server.run(getattr(a, "port", 8765), open_browser=not getattr(a, "no_browser", False))
        return

    cfg = load_config()
    host = a.host or cfg["HOST"]
    if not host:
        sys.exit(f"No printer IP. Put HOST=... in {CONFIG} or use --host")

    def confirm(name):
        if getattr(a, "yes", False):
            return True
        ans = input(f"Start printing '{name}' now? Check the resin and the build plate. [y/N] ")
        return ans.strip().lower() in ("y", "yes", "s", "sim")

    try:
        async with Halot(host, cfg["PASSWORD"]) as h:
            if a.cmd == "version":
                print(await h.request({"cmd": "VERSION_CHECK"}))
            elif a.cmd == "status":
                print(fmt_status(await h.status()))
            elif a.cmd == "raw-status":
                print(json.dumps(await h.status(), indent=2, ensure_ascii=False))
            elif a.cmd == "send":
                if not a.file.is_file():
                    sys.exit(f"File not found: {a.file}")
                if a.file.suffix.lower() not in (".cxdlpv4", ".cxdlp"):
                    print("Warning: the HALOT expects a .cxdlpv4 file.", file=sys.stderr)
                st = await h.status()  # validates password before uploading
                if st.get("printStatus") not in (None, "", "PRINT_GENERAL", "PRINT_COMPLETE", "PRINT_STOP"):
                    print(f"Warning: the printer is in '{st.get('printStatus')}'.", file=sys.stderr)
                print(f"Uploading {a.file.name} ({a.file.stat().st_size / 1e6:.1f} MB) to {host}...")
                def progress(pct):
                    print(f"\r  uploading... {pct:3d}%", end="", flush=True)

                r = await h.upload(a.file, on_progress=progress)
                print()
                print(f"Upload finished (checkstate={r.get('checkstate')}).")
                if a.print and confirm(a.file.name):
                    print(await h.request({"cmd": "START_PRINT", "filename": a.file.name, "token": h.tok()}))
            elif a.cmd == "print":
                if confirm(a.filename):
                    print(await h.request({"cmd": "START_PRINT", "filename": a.filename, "token": h.tok()}))
            elif a.cmd == "pause":
                print(await h.request({"cmd": "PRINT_PAUSE", "token": h.tok()}))
            elif a.cmd == "stop":
                print(await h.request({"cmd": "PRINT_STOP", "token": h.tok()}))
    except PrinterError as e:
        sys.exit(str(e))
    except (OSError, asyncio.TimeoutError, websockets.WebSocketException) as e:
        sys.exit(f"Could not talk to {host}: {e!r}")


if __name__ == "__main__":
    asyncio.run(main())
