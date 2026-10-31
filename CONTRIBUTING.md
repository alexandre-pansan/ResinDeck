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

- Python: standard library plus `websockets`, `cryptography`, `aiohttp` and `numpy`. Keep it that way
  unless there is a good reason.
- Web interface: plain JavaScript modules, no build step. Reload the page to see changes; restart
  the server after changing Python code.
- Keep the server bound to `127.0.0.1`.
- Do not commit sliced files, settings or passwords.

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
