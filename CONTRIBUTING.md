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

Printer support lives in driver modules; `halot.py` is the first one. A new brand needs:

- a driver with the same operations: connect, status, upload, start, pause, stop;
- if it uses another file format, a reader like `cxdlp.py` for the thumbnail, layers and 3D
  preview.

The server and the interface still assume the HALOT driver in a few places, so open an issue
first and we can work out the shape together.

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
