# ResinDeck

An open source control deck for resin 3D printers: preview your sliced files in 3D, send them
to the printer over the network and follow the print, from your own computer.

Today it supports **Creality HALOT** printers, straight from Linux, with no HALOT BOX and no Wine
or Proton. The goal is to grow to more printer brands and systems with help from the community.

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

The app runs in the background with an icon in the system tray (**Open ResinDeck** / **Quit**) and
opens the interface in your browser. On GNOME the tray icon needs the AppIndicator extension, which
Ubuntu ships by default.

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

The server only listens on `127.0.0.1`, so it is not reachable from other machines. Everything,
including the 3D view, works offline.

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
| Command line printer | `~/.config/resindeck/config` (see `config.example`), or the `RESINDECK_HOST` / `RESINDECK_PASSWORD` environment variables |
| 3D preview cache | `~/.cache/resindeck/` |

On first start the web interface imports the printer from the command line config.

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

The HALOT protocol is a WebSocket on port 18188 with JSON commands and a DES-based token; uploads
go in compressed 1 MiB chunks. Everything known about it is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

| File | What it does |
|---|---|
| `resindeck.py` | command line and entry point |
| `halot.py` | Creality HALOT driver (network protocol) |
| `server.py` | local web server (aiohttp): library, previews, printer connections |
| `cxdlp.py` | `.cxdlpv4` reader: header, thumbnails, layer images, voxel grid for the 3D view |
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