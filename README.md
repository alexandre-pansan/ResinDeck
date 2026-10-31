# halot-send

Send sliced files to Creality HALOT resin printers over the network, straight from Linux.
No HALOT BOX, no Wine or Proton.

[Leia em português](README.pt-BR.md)

- **Web interface** (runs locally, opens in your browser):
  - 3D preview rebuilt from the layers of the sliced file
  - layer-by-layer viewer and the slicer thumbnail
  - printer manager: several printers, network search, live status
  - send, send and print, pause/resume, stop, with progress per layer and time left
- **Command line** for scripts and quick jobs.
- Reads `.cxdlpv4` files, the format HALOT BOX exports for HALOT printers.

Tested with a **HALOT-R6**. Other HALOT models use the same protocol as far as we know
(the protocol base comes from a HALOT Mage Pro project), but they have not been tested.
Reports are welcome.

> [!WARNING]
> This is an unofficial tool, not affiliated with or endorsed by Creality. It talks to your
> printer with a protocol worked out by observation. Check the resin vat and the build plate
> before starting a print, as you would with the official software.

## Install

You need Python 3.10+ and the printer on the same network as your computer.

```sh
git clone https://github.com/alexandre-pansan/halot-send.git
cd halot-send
./start.sh
```

`start.sh` creates a virtualenv in `.venv` on the first run (with [uv](https://docs.astral.sh/uv/)
if you have it, `python3 -m venv` otherwise), installs the dependencies and opens the interface
at <http://127.0.0.1:8765/>.

To do it by hand:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python halot_send.py ui
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

The server only listens on `127.0.0.1`, so it is not reachable from other machines. The 3D view
loads [three.js](https://threejs.org/) from a CDN; without internet, the Layers and Thumbnail tabs
still work.

## Command line

```sh
halot-send version                       # firmware version, checks the connection
halot-send status                        # print status
halot-send send part.cxdlpv4             # upload only
halot-send send part.cxdlpv4 --print     # upload and print (asks for confirmation)
halot-send print part.cxdlpv4            # print a file that is already on the printer
halot-send pause                         # pause / resume
halot-send stop                          # cancel the print
halot-send ui                            # web interface (also the default with no command)
```

Run it as `.venv/bin/python halot_send.py ...`, or create a small wrapper script named
`halot-send` on your `PATH`.

## Configuration

| What | Where |
|---|---|
| Web interface settings (printers, passwords, folders) | `~/.config/halot-send/settings.json`, mode 600, edited from the interface |
| Command line printer | `~/.config/halot-send/config` (see `config.example`), or the `HALOT_HOST` / `HALOT_PASSWORD` environment variables |
| 3D preview cache | `~/.cache/halot-send/` |

On first start the web interface imports the printer from the command line config.

The printer password must be 1 to 8 characters. Longer passwords can be set on the printer but
give a token error, also with HALOT BOX.

## Troubleshooting

- **"Wrong password (TOKEN_ERROR)"**: the password does not match the one on the printer. Check it
  on the printer screen, or set a new one there.
- **"The printer is still receiving another file"**: an earlier upload was left half-way. Close
  any other program connected to the printer (including another halot-send) or restart the
  printer.
- **Printer shows as offline**: check that the IP address is right and that port 18188 is
  reachable from your computer.

## How it works

The protocol is a WebSocket on port 18188 with JSON commands and a DES-based token; uploads go in
compressed 1 MiB chunks. Everything known about it is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

| File | What it does |
|---|---|
| `halot_send.py` | printer protocol and command line |
| `server.py` | local web server (aiohttp): library, previews, printer connections |
| `cxdlp.py` | `.cxdlpv4` reader: header, thumbnails, layer images, voxel grid for the 3D view |
| `static/` | web interface (plain JavaScript, three.js for 3D, `i18n.js` for translations) |

## Contributing

Bug reports, test results with other HALOT models and translations are very welcome. See
[CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

- [HalotMageProSharp](https://github.com/mrtska/HalotMageProSharp) by mrtska (MIT): the base of
  the network protocol and the token algorithm.
- [UVtools](https://github.com/sn4k3/UVtools) by Tiago Conceição: documentation of the
  `.cxdlpv4` file layout. halot-send has its own Python implementation and contains no UVtools code.
- [three.js](https://threejs.org/) (MIT) for the 3D view.

## License

[MIT](LICENSE). "Creality" and "HALOT" are trademarks of their respective owners and are used here
only to describe compatibility.
