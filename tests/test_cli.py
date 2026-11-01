import sys

import pytest

import halot
import resindeck


@pytest.fixture
def config(tmp_path, monkeypatch):
    cfg = tmp_path / "resindeck" / "config"
    monkeypatch.setattr(resindeck, "CONFIG_DIR", cfg.parent)
    monkeypatch.setattr(resindeck, "CONFIG", cfg)
    monkeypatch.setattr(resindeck, "OLD_CONFIG_DIR", tmp_path / "halot-send")
    return cfg


async def run(monkeypatch, *args, stdin="n"):
    monkeypatch.setattr(sys, "argv", ["resindeck", *args])
    monkeypatch.setattr("builtins.input", lambda prompt="": stdin)
    try:
        await resindeck.main()
        return 0
    except SystemExit as e:
        return e.code


def test_load_config(config, monkeypatch):
    config.parent.mkdir(parents=True)
    config.write_text("# comment\nHOST = 10.0.0.7\nPASSWORD=abc\nnot a setting\n")
    assert resindeck.load_config() == {"HOST": "10.0.0.7", "PASSWORD": "abc"}
    monkeypatch.setenv("RESINDECK_HOST", "10.0.0.8")
    monkeypatch.setenv("RESINDECK_PASSWORD", "env")
    assert resindeck.load_config() == {"HOST": "10.0.0.8", "PASSWORD": "env"}


def test_load_config_without_file(config):
    assert resindeck.load_config() == {"HOST": "", "PASSWORD": ""}


def test_migrate_config(config):
    resindeck.OLD_CONFIG_DIR.mkdir()
    (resindeck.OLD_CONFIG_DIR / "config").write_text("HOST=1.2.3.4\n")
    resindeck.migrate_config()
    assert config.read_text() == "HOST=1.2.3.4\n"
    (resindeck.OLD_CONFIG_DIR / "config").write_text("HOST=changed\n")
    resindeck.migrate_config()  # never overwrites an existing config
    assert config.read_text() == "HOST=1.2.3.4\n"


async def test_no_host(config, monkeypatch):
    code = await run(monkeypatch, "status")
    assert "No printer IP" in code


async def test_version(config, printer, monkeypatch, capsys):
    assert await run(monkeypatch, "--host", "127.0.0.1", "version") == 0
    assert "'version': '1'" in capsys.readouterr().out


async def test_status(config, printer, monkeypatch, capsys):
    monkeypatch.setenv("RESINDECK_PASSWORD", "secret1")
    assert await run(monkeypatch, "--host", "127.0.0.1", "status") == 0
    assert "PRINT_GENERAL" in capsys.readouterr().out
    assert await run(monkeypatch, "--host", "127.0.0.1", "raw-status") == 0
    assert '"printStatus": "PRINT_GENERAL"' in capsys.readouterr().out


async def test_wrong_password(config, printer, monkeypatch):
    monkeypatch.setenv("RESINDECK_PASSWORD", "wrong12")
    assert "TOKEN_ERROR" in await run(monkeypatch, "--host", "127.0.0.1", "status")


async def test_unreachable_printer(config, monkeypatch):
    monkeypatch.setattr(halot, "PORT", 1)
    assert "Could not talk to 127.0.0.1" in await run(monkeypatch, "--host", "127.0.0.1", "version")


async def test_send_and_print(config, printer, sliced, monkeypatch, capsys):
    monkeypatch.setenv("RESINDECK_PASSWORD", "secret1")
    monkeypatch.setattr(halot, "CHUNK", 2048)
    assert await run(monkeypatch, "--host", "127.0.0.1", "send", str(sliced), "--print", stdin="y") == 0
    out = capsys.readouterr().out
    assert "Upload finished (checkstate=1)" in out and "100%" in out
    assert printer.files["part.cxdlpv4"] == sliced.read_bytes()
    assert printer.status["printStatus"] == "PRINT_PROCESSING"


async def test_send_without_confirmation_does_not_print(config, printer, sliced, monkeypatch):
    monkeypatch.setenv("RESINDECK_PASSWORD", "secret1")
    assert await run(monkeypatch, "--host", "127.0.0.1", "send", str(sliced), "--print", stdin="n") == 0
    assert printer.status["printStatus"] == "PRINT_GENERAL"


async def test_send_checks_the_file(config, printer, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("RESINDECK_PASSWORD", "secret1")
    assert "File not found" in await run(monkeypatch, "--host", "127.0.0.1", "send", str(tmp_path / "nope"))
    other = tmp_path / "model.stl"
    other.write_bytes(b"solid x" * 10)
    printer.status = {"printStatus": "PRINT_PROCESSING"}
    await run(monkeypatch, "--host", "127.0.0.1", "send", str(other))
    err = capsys.readouterr().err
    assert "expects a .cxdlpv4" in err and "PRINT_PROCESSING" in err


async def test_print_pause_stop(config, printer, monkeypatch):
    monkeypatch.setenv("RESINDECK_PASSWORD", "secret1")
    assert await run(monkeypatch, "--host", "127.0.0.1", "print", "part.cxdlpv4", "-y") == 0
    assert printer.status["printStatus"] == "PRINT_PROCESSING"
    await run(monkeypatch, "--host", "127.0.0.1", "pause")
    assert printer.status["printStatus"] == "PRINT_PAUSE"
    await run(monkeypatch, "--host", "127.0.0.1", "stop")
    assert printer.status["printStatus"] == "PRINT_STOP"


async def test_ui_is_the_default(config, monkeypatch):
    import server

    calls = []

    async def fake_run(port, open_browser):
        calls.append((port, open_browser))

    monkeypatch.setattr(server, "run", fake_run)
    await run(monkeypatch)
    await run(monkeypatch, "ui", "--port", "9000", "--no-browser")
    assert calls == [(8765, True), (9000, False)]
