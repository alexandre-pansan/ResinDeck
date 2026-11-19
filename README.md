# ResinDeck

[![CI](https://github.com/alexandre-pansan/ResinDeck/actions/workflows/ci.yml/badge.svg)](https://github.com/alexandre-pansan/ResinDeck/actions/workflows/ci.yml)

An open source control deck for resin 3D printers: preview your sliced files in 3D, send them
to the printer over the network and follow the print, from your own computer.

Today it supports **Creality HALOT** printers, straight from Linux, with no HALOT BOX and no Wine
or Proton. The goal is to bring network communication with every resin 3D printer together in one
place, growing to more brands and systems with help from the community.

[Leia em português](README.pt-BR.md)

- **Desktop application** for Linux and Windows:
  - 3D view rebuilt from the layers of the sliced file, drawn by the graphics card, smooth and detailed down
    to the pixel, cut at any layer
  - layer-by-layer viewer and the slicer thumbnail, print time, resin and exposure settings
  - printer manager: several printers, network search, live status with layer, time left, start time and
    expected end
  - send, send and print, pause/resume, stop, with upload progress
- **Phone and browser access** on your network, with a QR code and a PIN, to follow and control prints.
- **Command line** (`resindeck-cli`) for scripts and quick jobs.
- Reads `.cxdlpv4` files, the format HALOT BOX and Chitubox export for HALOT printers.

## Supported printers

| Printer | Status |
|---|---|
| Creality HALOT-R6 | tested: status, upload, print, pause, resume, stop |
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
- **Windows:** `ResinDeck-<version>-windows-x64-setup.exe` installs it with a Start menu entry, or
  `ResinDeck-<version>-windows-x64.zip` runs without installing (unzip and open `resindeck.exe`). The files
  are not signed yet, so Windows SmartScreen may warn about them: click **More info** → **Run anyway**.
  Allow ResinDeck when the firewall asks, so phones can connect.

## Build from source

On Linux:

```sh
git clone https://github.com/alexandre-pansan/ResinDeck.git
cd ResinDeck
./build.sh setup   # first time: CMake, Ninja and Qt, without admin rights
./build.sh run     # build and open ResinDeck
```

Qt needs the OpenGL development files: `sudo apt install libgl-dev libegl-dev` on Debian and Ubuntu.
Other systems and details: [CONTRIBUTING.md](CONTRIBUTING.md#building).

## Using ResinDeck

1. **Add the printer**: click **+ add**, type its IP address (or use **Search the network**) and
   the network password shown on the printer screen.
2. **Set the library folder**: click **folders** and pick the folder where your `.cxdlpv4`
   files are. You can also drop files on the window, or open one with ResinDeck from your file manager.
3. **Pick a file** to see it in 3D, browse its layers and see the print time, resin and
   exposure settings. Drag to turn the model, right-drag to move it, scroll to zoom, double-click to reset.
4. **Send** or **Send and print**. While it prints, the printer card shows the layer, the time left,
   when it started and when it should end.

The window is in English and Portuguese, picked from your system language (switch at the top).
Everything, including the 3D view, works offline.

### Follow the print from your phone

While ResinDeck is open, phones and other computers on the same Wi-Fi can follow the print in a browser.
Click **Phone** at the top and scan the QR code (or open <http://resindeck.local:8765/>), then type the
6-digit PIN shown there. The phone gets progress, the 3D view, pause and stop.

- Only the computer running ResinDeck has access without the PIN, through any of its addresses.
  The same window lists the connected devices: disconnect one, or click **new PIN** to sign out
  all of them.
- `resindeck.local` needs mDNS: it works on iPhone, Windows, macOS and Linux, but not on some
  Android phones: there, open the IP address shown under the QR code.
- Printer settings and library folders can only be changed on the computer.
- To keep ResinDeck reachable only from this computer, start it with `RESINDECK_LAN=0`. More in
  [SECURITY.md](SECURITY.md).

## Command line

```sh
resindeck-cli version                       # firmware version, checks the connection
resindeck-cli status                        # print status
resindeck-cli send part.cxdlpv4             # upload only
resindeck-cli send part.cxdlpv4 --print     # upload and print (asks for confirmation)
resindeck-cli print part.cxdlpv4            # print a file that is already on the printer
resindeck-cli pause                         # pause / resume
resindeck-cli stop                          # cancel the print
resindeck-cli serve                         # the phone and browser interface, without the window
```

It is next to `resindeck` in the AppImage (`--appimage-extract` to get it) and in the Windows folder, and in
`build/release/app/cli/` when built from source.

## Configuration

| What | Where |
|---|---|
| Printers, passwords, library folders, paired phones | `~/.config/resindeck/settings.json`, mode 600, edited from the window |
| Command line printer | `~/.config/resindeck/config` (see `config.example`), or the `RESINDECK_HOST` / `RESINDECK_PASSWORD` / `RESINDECK_DRIVER` environment variables |
| 3D preview cache | `~/.cache/resindeck/` |

On first start ResinDeck imports the printer from the command line config.

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
- **"Port 8765 is in use"**: another ResinDeck is already open. The window still works, but phones
  connect to the other one.
- **Printer shows as offline**: check that the IP address is right and that port 18188 is
  reachable from your computer.

## How it works

ResinDeck is written in C++ with Qt. Each printer family is a driver, so adding a brand means writing one
class; see [Adding a printer brand](CONTRIBUTING.md#adding-a-printer-brand). The 3D view extracts a surface
from the layer images themselves, using their anti-aliasing to place the edges between pixels.

The HALOT protocol is a WebSocket on port 18188 with JSON commands and a DES-based token; uploads
go in compressed 1 MiB chunks. Everything known about it is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

| Folder | What it does |
|---|---|
| `core/` | printers and drivers, file formats, 3D meshes, the phone web server, network helpers, with tests |
| `app/desktop/` | the window (Qt Widgets) and the GPU 3D view |
| `app/cli/` | the command line |
| `static/` | the phone and browser interface (plain JavaScript, three.js, `i18n.js` for translations) |
| `packaging/` | icon, AppImage and Windows packaging |

Version 0.x was written in Python; it is kept in the `legacy` branch.

## Contributing

Bug reports, test results with other HALOT models and translations are very welcome. See
[CONTRIBUTING.md](CONTRIBUTING.md).

## Roadmap

ResinDeck is growing into a full resin slicer: opening models, hollowing, supports, slicing and printing in
one place. The phases are in [docs/ROADMAP.md](docs/ROADMAP.md).

## Credits

- [HalotMageProSharp](https://github.com/mrtska/HalotMageProSharp) by mrtska (MIT): the base of
  the network protocol and the token algorithm.
- [UVtools](https://github.com/sn4k3/UVtools) by Tiago Conceição: documentation of the
  `.cxdlpv4` file layout. ResinDeck has its own implementation and contains no UVtools code.
- [Qt](https://www.qt.io/) (LGPL 3) for the window, the network and the 3D view.
- [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki (MIT),
  in `third_party/qrcodegen/`.
- [three.js](https://threejs.org/) (MIT) for the 3D view of the phone and browser interface, bundled in
  `static/vendor/three/`.

## License

[MIT](LICENSE). "Creality" and "HALOT" are trademarks of their respective owners and are used here
only to describe compatibility.

> And yeah, this was entirely built using Claude (or Claudinho, to those close to him). The only human-made part is this one, which goes like: dabadee dabadaa. 
> I'm all for using AI to create new builds, apps, dreams, models, logos, images, especially when some deep research is required or when we don't fully understand how something works. Feel free to use it—just make sure to test everything and be nice to AI. After all, we never know when Skynet might come for us :D. 
>I don't fully understand how that communication works or how I was able to do it. The best I could do was point out some UI enhancements, so don't judge me! I'm just here to make Unix users' lives easier when it comes to 3D printing. (Running it natively is better than WINEing it, after all! )