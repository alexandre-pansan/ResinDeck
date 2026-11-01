# Contributing

Thanks for helping! Issues and pull requests are welcome, in English or Portuguese.

## Reporting a problem

Please include:

- printer model and firmware (`resindeck version` prints the firmware version),
- what you did and what happened,
- the error shown in the interface or terminal,
- for status or protocol issues, the output of `resindeck raw-status`.

Do not post your printer password. The token in captured traffic is derived from it and can be
reversed, so remove `"token"` values too.

## Testing other printers

ResinDeck has only been tested on a HALOT-R6. If you have another HALOT model (Mage, Mage Pro,
One, Sky, Lite, X1…), a report saying whether status, upload, print, pause and stop work is very
useful, even if everything works. [docs/PROTOCOL.md](docs/PROTOCOL.md) describes what we expect
the printer to answer.

## Adding a printer brand

Each printer family is a **driver**: one Python module in `drivers/`. The web server, the
interface and the command line only talk to the driver contract in
[`drivers/base.py`](drivers/base.py), so a new brand does not touch any other file.
[`drivers/halot.py`](drivers/halot.py) is a complete example.

### 1. Write the driver

Create `drivers/<brand>.py` with a subclass of `Driver`:

```python
from drivers import base
from drivers.base import Driver, PrinterError, Status, tcp_open


class MyBrandDriver(Driver):
    id = "mybrand"                  # stored in settings.json, never change it later
    name = "MyBrand"                # shown in the interface
    models = "Model X (tested)"
    extensions = (".goo",)          # files the printer accepts
    password = False                # True if status and commands need the printer password
    password_max = 0                # longest password the printer accepts (0: no limit)

    async def connect(self):        # open the connection; set self.version if you can
    async def close(self):
    async def ping(self):           # cheap "still there?" check, without a password
    async def status(self):         # -> Status(state=base.PRINTING, file=..., layer=..., layers=..., remaining=...)
    async def upload(self, path, on_progress=None):   # call on_progress(percent); return a dict
    async def start(self, filename):
    async def pause(self):
    async def resume(self):
    async def stop(self):

    @classmethod
    async def probe(cls, host):     # "is one of my printers at this address?" for Search the network
```

Rules of the contract:

- **Status:** translate the printer's own state into one of the states in `drivers/base.py`
  (`idle`, `printing`, `paused`, `finished`…). Put the printer's own name for it in `code` and its
  full reply in `raw`. Anything you cannot map becomes `UNKNOWN`, and the interface shows `code`.
- **Errors:** raise `PrinterError(message, code, **params)` for problems the user can fix (wrong
  password, file refused). Let network errors (`OSError`, `asyncio.TimeoutError`, …) propagate:
  ResinDeck shows the printer as offline and reconnects. Add every new `code` to `static/i18n.js`
  as `errors.<code>`, in English and Portuguese.
- **probe()** runs for every address on the network at once. Keep it under about a second, start
  with `tcp_open(host, port)` and never send anything that changes the printer.
- `self.host` is the address and `self.secret` the password the user typed.

### 2. Register it

Add the class to `DRIVERS` in [`drivers/__init__.py`](drivers/__init__.py). It then shows up in
**+ add**, in **Search the network** and in `resindeck --driver mybrand`.

### 3. Test it without the printer

Write a fake printer that speaks your protocol, like [`tests/fakeprinter.py`](tests/fakeprinter.py)
does for the HALOT, and test the driver against it, like `tests/test_halot.py`. That way the tests
run in CI and nobody needs your printer to change the code safely.
`tests/test_drivers.py::test_every_driver_fills_in_the_contract` checks that every registered
driver implements every method. Then try it on the real printer and say in the pull request which
model and firmware you used.

Please also document the protocol, as [docs/PROTOCOL.md](docs/PROTOCOL.md) does for the HALOT.

### Another file format

If the printer uses another sliced format, add a reader in `formats/` with the interface described
in [`formats/__init__.py`](formats/__init__.py) (header info, thumbnail, layer images, voxels for the
3D view) and list its extensions in `READERS`. [`formats/cxdlp.py`](formats/cxdlp.py) is the
reference, and [`tests/cxdlp_writer.py`](tests/cxdlp_writer.py) shows how to build test files.

## Development

```sh
./start.sh                 # creates .venv and starts the interface
.venv/bin/python resindeck.py status
```

- Python: standard library plus `websockets`, `cryptography`, `aiohttp`, `numpy`, `zeroconf`
  (mDNS) and `segno` (QR code). Keep it that way unless there is a good reason.
- Web interface: plain JavaScript modules, no build step. Reload the page to see changes; restart
  the server after changing Python code.
- The server listens on the network: every `/api` route and `/ws` need the PIN session unless
  the request comes from this computer (see `guard` in `server.py`). Keep new routes under `/api`.
- Changes that only paired devices should not make (settings, files on disk) must call
  `local_only(request)`. See [SECURITY.md](SECURITY.md).
- Do not commit sliced files, settings or passwords.

## Tests and checks

```sh
uv pip install --python .venv/bin/python -r requirements-dev.txt   # or: .venv/bin/pip install -r ...
.venv/bin/python -m pytest --cov          # tests and coverage
.venv/bin/ruff check .                    # lint
.venv/bin/bandit -c pyproject.toml -r . -ll
.venv/bin/pip-audit -r requirements-desktop.txt
```

The tests need no printer and no sliced files: `tests/fakeprinter.py` is a fake HALOT that speaks
the network protocol (including the error cases), and `tests/cxdlp_writer.py` writes small
synthetic `.cxdlpv4` files. Add a test with every change; CI fails below 90% coverage.

Every pull request runs `.github/workflows/ci.yml`:

- lint (ruff, JavaScript and shell syntax);
- tests with coverage on Linux and Windows, Python 3.10 and 3.12;
- bandit and pip-audit;
- a smoke test that builds the AppImage and the `.exe`, starts them and checks that the interface
  answers.

The release workflow runs the same checks before publishing.

## Desktop packages and releases

The AppImage and the Windows `.exe` are built with PyInstaller from `desktop.py`
(`packaging/resindeck.spec`).

```sh
uv pip install --python .venv/bin/python -r requirements-desktop.txt
packaging/build_appimage.sh        # Linux: dist/ResinDeck-<version>-x86_64.AppImage
```

The `.exe` has to be built on Windows: `python -m PyInstaller packaging/resindeck.spec`.

To publish a release, bump `__version__` in `resindeck.py`, commit, then push a matching tag:

```sh
git tag v0.2.0 && git push origin v0.2.0
```

The `Release` workflow builds both packages and attaches them to a GitHub release with generated
notes. Run `packaging/make_icon.py` after changing the icon.

## Translations

UI strings live in `static/i18n.js`. To add a language, copy the `en` block, give it a language
tag (for example `"es"`), translate the values and keep the keys. Missing keys fall back to
English. Add a short label for it in `initLanguage()` in `static/app.js`.

Messages from the server reach the interface with an error `code`, so they are translated through
the `errors.*` keys too. Command line output stays in English.
