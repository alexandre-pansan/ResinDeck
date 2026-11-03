# Security

## Reporting a vulnerability

Please report security problems privately, through
[GitHub's private vulnerability reporting](https://github.com/alexandre-pansan/ResinDeck/security/advisories/new),
not in a public issue. Include what you found, how to reproduce it and which version you used.
You should get an answer within a week.

## What ResinDeck protects

ResinDeck runs a small web server on your computer and, by default, on your local network, so a
phone can follow a print. It talks to printers on the same network.

| Who | What they can do |
|---|---|
| This computer (any of its addresses) | Everything |
| A device that entered the PIN | Follow prints, preview files, send, start, pause and stop prints |
| Any other device on the network | Nothing beyond the PIN screen |
| A web page open in your browser | Nothing |

How:

- **Pairing:** other devices type a 6-digit PIN shown on the computer. Five wrong attempts from an
  address block it for a minute. Pairing gives the device a random session cookie (HttpOnly,
  SameSite=Strict); only its SHA-256 hash is stored. The computer lists paired devices and can
  disconnect any of them; a new PIN signs out all of them.
- **Computer-only actions:** pairing management, printer and library folder settings, network scan
  and Quit are refused for paired devices.
- **Browser attacks:** requests whose `Origin` is another site are refused, WebSocket handshakes
  included. Requests from this computer must use a known host name, which blocks DNS rebinding.
  Pages are sent with a Content-Security-Policy, `X-Frame-Options: DENY` (no clickjacking) and
  `nosniff`.
- **Files:** only files inside the configured library folders can be read, after resolving `..` and
  symbolic links. Uploads keep only the file name and must be `.cxdlpv4`/`.cxdlp`.
- **Settings** (`~/.config/resindeck/settings.json`), which hold the printer passwords, are written
  with permissions `600`.

To keep ResinDeck reachable from this computer only, start it with `RESINDECK_LAN=0`.

## Known limits

- The interface uses plain HTTP on the local network. Someone who can watch your Wi-Fi traffic can
  see the session cookie. Use a network you trust.
- The HALOT network protocol itself has no encryption: the printer token is derived from its
  password with a fixed key (see [docs/PROTOCOL.md](docs/PROTOCOL.md)). That is how the printer
  works, not something ResinDeck can change.

## Automated checks

Every pull request runs the tests, [bandit](https://bandit.readthedocs.io/) (static analysis) and
[pip-audit](https://pypi.org/project/pip-audit/) (known vulnerabilities in dependencies).
Dependabot opens weekly updates for dependencies and GitHub Actions.
