# ResinDeck

[![CI](https://github.com/alexandre-pansan/ResinDeck/actions/workflows/ci.yml/badge.svg)](https://github.com/alexandre-pansan/ResinDeck/actions/workflows/ci.yml)

An open source control deck for resin 3D printers: preview your sliced files in 3D, send them
to the printer over the network and follow the print, from your own computer.

Today it supports **Creality HALOT** printers, straight from Linux, with no HALOT BOX and no Wine
or Proton. The goal is to bring network communication with every resin 3D printer together in one
place, growing to more brands and systems with help from the community.

[Leia em português](README.pt-BR.md)

- **Web interface** (runs locally, opens in your browser):
  - 3D preview rebuilt from the layers of the sliced file
  - layer-by-layer viewer and the slicer thumbnail
  - printer manager: several printers, network search, live status
  - send, send and print, pause/resume, stop, with progress per layer and time left
- **Command line** for scripts and quick jobs.
- Reads `.cxdlpv4` files, the format HALOT BOX exports for HALOT printers.

## Supported printers

| Printer | Status |
|---|---|
| Creality HALOT-R6 | tested: status, upload, print (pause and stop not confirmed yet) |
| Other Creality HALOT models | same protocol as far as we know, untested; reports welcome |
| Other brands | not yet; see [CONTRIBUTING.md](CONTRIBUTING.md) to help add one |

> [!WARNING]
> This is an unofficial tool, not affiliated with or endorsed by any printer maker. It talks to your
> printer with a protocol worked out by observation. Check the resin vat and the build plate
> before starting a print, as you would with the official software.

## Download

Get the latest version from the [Releases page](https://github.com/alexandre-pansan/ResinDeck/releases):

- **Linux:** `ResinDeck-<version>-x86_64.AppImage`. Make it executable (`chmod +x`, or in your
  file manager's properties) and open it.
- **Windows:** `ResinDeck-<version>-windows-x64.exe`. The file is not signed yet, so Windows
  SmartScreen may warn about it: click **More info** → **Run anyway**.

The app runs in the background with an icon in the system tray and opens the interface in your
browser. Clicking the icon opens the interface again; **Quit**, at the top of the interface, stops
ResinDeck. On Linux the tray icon has no menu, and on GNOME it needs the AppIndicator extension,
which Ubuntu ships by default.

## Install from source

You need Python 3.10+ and the printer on the same network as your computer.

```sh
git clone https://github.com/alexandre-pansan/ResinDeck.git
cd ResinDeck
./start.sh
```

`start.sh` creates a virtualenv in `.venv` on the first run (with [uv](https://docs.astral.sh/uv/)
if you have it, `python3 -m venv` otherwise), installs the dependencies and opens the interface
at <http://127.0.0.1:8765/>.

To do it by hand:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python resindeck.py ui
```

## Using the web interface

1. **Add the printer**: click **+ add**, type its IP address (or use **Search the network**) and
   the network password shown on the printer screen.
2. **Set the library folder**: click **folders** and enter the folder where your `.cxdlpv4`
   files are. You can also drop files on the window.
3. **Pick a file** to preview it in 3D, browse its layers and see the print time, resin and
   exposure settings.
4. **Send** or **Send and print**. While it prints, the printer card shows the layer, the
   progress and the time left; **Follow** shows in the 3D view how far the print has got.

The interface is in English and Portuguese, picked from your browser language (switch at the top
right).

Everything, including the 3D view, works offline.

### Follow the print from your phone

With the phone on the same Wi-Fi, click **Phone** at the top and scan the QR code (or open
<http://resindeck.local:8765/>), then type the 6-digit PIN shown there. The phone gets the full
interface: progress, 3D view, pause and stop.

- Only the computer running ResinDeck has access without the PIN, through any of its addresses.
  The same window lists the connected devices: disconnect one, or click **new PIN** to sign out
  all of them.
- `resindeck.local` needs mDNS: it works on iPhone, Windows, macOS and Linux, but not on some
  Android phones: there, open the IP address shown under the QR code.
- On Windows, allow ResinDeck when the firewall asks on the first run.
- Printer settings and library folders can only be changed on the computer.
- To keep ResinDeck reachable only from this computer, start it with `RESINDECK_LAN=0`. More in
  [SECURITY.md](SECURITY.md).

## Command line

```sh
resindeck version                       # firmware version, checks the connection
resindeck status                        # print status
resindeck send part.cxdlpv4             # upload only
resindeck send part.cxdlpv4 --print     # upload and print (asks for confirmation)
resindeck print part.cxdlpv4            # print a file that is already on the printer
resindeck pause                         # pause / resume
resindeck stop                          # cancel the print
resindeck ui                            # web interface (also the default with no command)
```

Run it as `.venv/bin/python resindeck.py ...`, or create a small wrapper script named
`resindeck` on your `PATH`.

## Configuration

| What | Where |
|---|---|
| Web interface settings (printers, passwords, folders) | `~/.config/resindeck/settings.json`, mode 600, edited from the interface |
| Command line printer | `~/.config/resindeck/config` (see `config.example`), or the `RESINDECK_HOST` / `RESINDECK_PASSWORD` / `RESINDECK_DRIVER` environment variables |
| 3D preview cache | `~/.cache/resindeck/` |

On first start the web interface imports the printer from the command line config.

You do not need to give the printer a fixed IP. ResinDeck remembers its MAC address, and when the
printer stops answering it looks for that MAC on the network and updates the IP by itself. This
works when the printer and the computer are on the same network, which is the usual case at home.

The printer password must be 1 to 8 characters. Longer passwords can be set on the printer but
give a token error, also with HALOT BOX.

## Troubleshooting

- **"Wrong password (TOKEN_ERROR)"**: the password does not match the one on the printer. Check it
  on the printer screen, or set a new one there.
- **"The printer is still receiving another file"**: an earlier upload was left half-way. Close
  any other program connected to the printer (including another ResinDeck) or restart the
  printer.
- **Printer shows as offline**: check that the IP address is right and that port 18188 is
  reachable from your computer.

## How it works

Each printer family is a driver in `drivers/`, so adding a brand means writing one module. See
[Adding a printer brand](CONTRIBUTING.md#adding-a-printer-brand).

The HALOT protocol is a WebSocket on port 18188 with JSON commands and a DES-based token; uploads
go in compressed 1 MiB chunks. Everything known about it is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

| File | What it does |
|---|---|
| `resindeck.py` | command line and entry point |
| `drivers/` | one module per printer family. `base.py` is the contract, `halot.py` the Creality HALOT driver |
| `formats/` | sliced file readers. `cxdlp.py` reads `.cxdlpv4`: header, thumbnails, layers, voxels for 3D |
| `server.py` | local web server (aiohttp): library, previews, printer connections |
| `desktop.py` | desktop app entry point: background server and tray icon |
| `static/` | web interface (plain JavaScript, three.js for 3D, `i18n.js` for translations) |
| `packaging/` | icon, PyInstaller spec and AppImage build script |

## Contributing

Bug reports, test results with other HALOT models and translations are very welcome. See
[CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

- [HalotMageProSharp](https://github.com/mrtska/HalotMageProSharp) by mrtska (MIT): the base of
  the network protocol and the token algorithm.
- [UVtools](https://github.com/sn4k3/UVtools) by Tiago Conceição: documentation of the
  `.cxdlpv4` file layout. ResinDeck has its own Python implementation and contains no UVtools code.
- [three.js](https://threejs.org/) (MIT) for the 3D view, bundled in `static/vendor/three/`.

## License

[MIT](LICENSE). "Creality" and "HALOT" are trademarks of their respective owners and are used here
only to describe compatibility.

> And yeah, this was entirely built using Claude (or Claudinho, to those close to him). The only human-made part is this one, which goes like: dabadee dabadaa. 
> I'm all for using AI to create new builds, apps, dreams, models, logos, images, especially when some deep research is required or when we don't fully understand how something works. Feel free to use it—just make sure to test everything and be nice to AI. After all, we never know when Skynet might come for us :D. 
>I don't fully understand how that communication works or how I was able to do it. The best I could do was point out some UI enhancements, so don't judge me! I'm just here to make Unix users' lives easier when it comes to 3D printing. (Running it natively is better than WINEing it, after all! )