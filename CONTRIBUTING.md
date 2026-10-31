# Contributing

Thanks for helping! Issues and pull requests are welcome, in English or Portuguese.

## Reporting a problem

Please include:

- printer model and firmware (`halot-send version` prints the firmware version),
- what you did and what happened,
- the error shown in the interface or terminal,
- for status or protocol issues, the output of `halot-send raw-status`.

Do not post your printer password. The token in captured traffic is derived from it and can be
reversed, so remove `"token"` values too.

## Testing other printers

halot-send has only been tested on a HALOT-R6. If you have another HALOT model (Mage, Mage Pro,
One, Sky, Lite, X1…), a report saying whether status, upload, print, pause and stop work is very
useful, even if everything works. [docs/PROTOCOL.md](docs/PROTOCOL.md) describes what we expect
the printer to answer.

## Development

```sh
./start.sh                 # creates .venv and starts the interface
.venv/bin/python halot_send.py status
```

- Python: standard library plus `websockets`, `cryptography`, `aiohttp` and `numpy`. Keep it that way
  unless there is a good reason.
- Web interface: plain JavaScript modules, no build step. Reload the page to see changes; restart
  the server after changing Python code.
- Keep the server bound to `127.0.0.1`.
- Do not commit sliced files, settings or passwords.

## Translations

UI strings live in `static/i18n.js`. To add a language, copy the `en` block, give it a language
tag (for example `"es"`), translate the values and keep the keys. Missing keys fall back to
English. Add a short label for it in `initLanguage()` in `static/app.js`.

Messages from the server reach the interface with an error `code`, so they are translated through
the `errors.*` keys too. Command line output stays in English.
