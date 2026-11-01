import socket
import sys
import types

import pytest

import desktop


@pytest.mark.parametrize("loc,expected", [
    (("pt_BR", "UTF-8"), "Abrir ResinDeck"), (("en_US", "UTF-8"), "Open ResinDeck"),
    (("de_DE", "UTF-8"), "Open ResinDeck"), ((None, None), "Open ResinDeck"),
])
def test_tray_labels_follow_the_locale(monkeypatch, loc, expected):
    monkeypatch.setattr(desktop.locale, "getlocale", lambda: loc)
    assert desktop.labels()["open"] == expected


def test_already_running(monkeypatch):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        s.listen()
        monkeypatch.setattr(desktop, "PORT", s.getsockname()[1])
        assert desktop.already_running()
    assert not desktop.already_running()  # the port is closed now


def test_windowed_builds_log_to_a_file(tmp_path, monkeypatch):
    monkeypatch.setattr(desktop, "LOG", tmp_path / "logs" / "resindeck.log")
    monkeypatch.setattr(sys, "stdout", None)
    monkeypatch.setattr(sys, "stderr", None)
    desktop.redirect_output()
    print("hello")
    sys.stdout.flush()
    assert "hello" in desktop.LOG.read_text()


def test_second_launch_opens_the_browser(monkeypatch):
    opened = []
    monkeypatch.setattr(desktop, "already_running", lambda: True)
    monkeypatch.setattr(desktop.webbrowser, "open", opened.append)

    async def url(port):
        return f"http://resindeck.local:{port}/"

    monkeypatch.setattr(desktop.server, "browser_url", url)
    desktop.main()
    assert opened == [f"http://resindeck.local:{desktop.PORT}/"]


def test_tray_quit_stops_the_server(monkeypatch):
    """main() with a fake pystray: the server runs until Quit is picked in the tray menu."""
    events = []

    async def fake_run(port, open_browser):
        events.append("served")
        await __import__("asyncio").Event().wait()  # until cancelled by Quit

    class Item:
        def __init__(self, text, action, default=False):
            self.text, self.action = text, action

    class Icon:
        def __init__(self, name, image, title, menu):
            self.menu = menu

        def run(self):
            import time

            while "served" not in events:
                time.sleep(0.01)
            quit_item = self.menu[-1]
            quit_item.action(self, quit_item)

        def stop(self):
            events.append("tray stopped")

    fake = types.SimpleNamespace(Icon=Icon, MenuItem=Item, Menu=lambda *items: list(items))
    monkeypatch.setitem(sys.modules, "pystray", fake)
    pil = types.ModuleType("PIL")
    pil.Image = types.SimpleNamespace(open=lambda path: path)
    monkeypatch.setitem(sys.modules, "PIL", pil)
    monkeypatch.setattr(desktop, "already_running", lambda: False)
    monkeypatch.setattr(desktop.server, "run", fake_run)
    desktop.main()
    assert events[0] == "served" and "tray stopped" in events


def test_without_a_tray_it_keeps_serving(monkeypatch, capsys):
    async def fake_run(port, open_browser):
        return None  # as if Quit was clicked in the web interface

    monkeypatch.setitem(sys.modules, "pystray", None)  # import fails
    monkeypatch.setattr(desktop, "already_running", lambda: False)
    monkeypatch.setattr(desktop.server, "run", fake_run)
    desktop.main()
    assert "Tray icon unavailable" in capsys.readouterr().err
