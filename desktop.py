"""ResinDeck desktop launcher: runs the local server in the background with a tray icon.

This is the entry point of the AppImage and the Windows .exe. It opens the interface in the
browser; the tray icon reopens it or quits. Without a usable tray it just keeps serving.
"""
import asyncio
import locale
import os
import socket
import sys
import threading
import webbrowser
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import server  # noqa: E402

PORT = int(os.environ.get("RESINDECK_PORT", "8765"))
URL = f"http://127.0.0.1:{PORT}/"
LOG = Path.home() / ".cache" / "resindeck" / "resindeck.log"

LABELS = {
    "en": {"open": "Open ResinDeck", "quit": "Quit"},
    "pt": {"open": "Abrir ResinDeck", "quit": "Sair"},
}


def labels():
    lang = (locale.getlocale()[0] or "en").split("_")[0].lower()
    return LABELS.get(lang, LABELS["en"])


def already_running():
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", PORT)) == 0


def redirect_output():
    """Windowed builds have no console: send output to a log file instead."""
    if sys.stdout is None or sys.stderr is None:
        LOG.parent.mkdir(parents=True, exist_ok=True)
        log = open(LOG, "a", buffering=1, encoding="utf-8")
        sys.stdout = sys.stdout or log
        sys.stderr = sys.stderr or log


def main():
    redirect_output()
    if already_running():
        webbrowser.open(asyncio.run(server.browser_url(PORT)))
        return

    loop = asyncio.new_event_loop()
    task = loop.create_task(server.run(PORT, open_browser=True))
    tray = []

    def serve():
        try:
            loop.run_until_complete(task)
        except asyncio.CancelledError:
            pass
        for icon in tray:  # quit from the web interface: take the tray icon down too
            icon.stop()

    thread = threading.Thread(target=serve, name="server")
    thread.start()

    try:
        import pystray
        from PIL import Image

        def quit_app(icon, _item):
            loop.call_soon_threadsafe(task.cancel)
            icon.stop()

        def open_ui():
            webbrowser.open(asyncio.run_coroutine_threadsafe(server.browser_url(PORT), loop).result())

        text = labels()
        icon = pystray.Icon(
            "ResinDeck",
            Image.open(HERE / "packaging" / "icon.png"),
            "ResinDeck",
            menu=pystray.Menu(
                pystray.MenuItem(text["open"], open_ui, default=True),
                pystray.MenuItem(text["quit"], quit_app),
            ),
        )
        tray.append(icon)
        icon.run()
    except Exception as e:  # no tray available: keep serving until the process is stopped
        print(f"Tray icon unavailable ({e!r}); ResinDeck keeps running at {URL}", file=sys.stderr)
    finally:
        thread.join()


if __name__ == "__main__":
    main()
