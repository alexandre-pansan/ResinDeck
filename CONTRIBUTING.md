# Contributing

Thanks for helping! Issues and pull requests are welcome, in English or Portuguese.

## Reporting a problem

Please include:

- your system (Linux distribution or Windows version) and ResinDeck version (`resindeck --version`),
- printer model and firmware (`resindeck-cli version` prints the firmware version),
- what you did and what happened,
- the error shown in the window, or printed in the terminal if you started ResinDeck from one,
- for status or protocol issues, the output of `resindeck-cli raw-status`.

Do not post your printer password. The token in captured traffic is derived from it and can be
reversed, so remove `"token"` values too.

## Testing other printers

ResinDeck has only been tested on a HALOT-R6. If you have another HALOT model (Mage, Mage Pro,
One, Sky, Lite, X1…), a report saying whether status, upload, print, pause and stop work is very
useful, even if everything works. [docs/PROTOCOL.md](docs/PROTOCOL.md) describes what we expect
the printer to answer.

## Building

ResinDeck is C++20 and Qt 6, built with CMake. On Linux, `build.sh` does all of it:

```bash
./build.sh setup        # first time: CMake, Ninja and Qt, without admin rights (Qt goes to ~/Qt)
./build.sh run          # build (optimized) and open the window; ./build.sh run FILE.cxdlpv4 opens a file
./build.sh test         # build with warnings as errors and sanitizers, and run the tests
./build.sh              # just build, into build/release
```

By hand, or on other systems: a C++20 compiler (GCC 11+, Clang 15+ or MSVC 2022), CMake, and Qt 6.8 or
newer with the WebSockets, HttpServer and ShaderTools modules. CI uses Qt 6.10.3: aqtinstall cannot read
the Windows packages of Qt 6.11 yet. On Linux, Qt also needs the OpenGL development files
(`sudo apt install libgl-dev libegl-dev` on Debian and Ubuntu). Qt installs without admin rights with
[aqtinstall](https://github.com/miurahr/aqtinstall):

```bash
pip install aqtinstall cmake ninja
aqt install-qt linux desktop 6.10.3 linux_gcc_64 -O ~/Qt -m qtwebsockets qthttpserver qtshadertools
cmake -B build/cpp -G Ninja -DCMAKE_PREFIX_PATH=~/Qt/6.10.3/gcc_64 -DRESINDECK_WERROR=ON
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

Add `-DRESINDECK_SANITIZE=ON` to build with AddressSanitizer and UBSan, as CI does on Linux.

The build gives `app/desktop/resindeck` (the window, which also serves the web interface to phones and
browsers) and `app/cli/resindeck-cli` (the command line). Both use `~/.config/resindeck/`.

## The code

| Folder | What |
|---|---|
| `core/printers/` | the driver contract (`driver.h`), the registry (`drivers.cpp`), the HALOT driver, and `Printer`, which keeps each printer connected in its own thread |
| `core/formats/` | sliced file readers (`sliced.h` is the interface, `cxdlp.cpp` the `.cxdlpv4` reader and writer) |
| `core/mesh/` | meshes for the 3D view: the voxel grid and the smooth surface through the layers |
| `core/deck.*` | the app state the window and the web server share: settings, printers, library, paired devices |
| `core/remote/` | the web server for phones and browsers, and the QR code |
| `core/net/` | this computer's address, the ARP table (printers found by MAC address), mDNS for `resindeck.local` |
| `core/tests/` | Qt Test suites, with a fake HALOT and synthetic `.cxdlpv4` files |
| `app/desktop/` | the window (Qt Widgets) and the 3D view (QRhi) |
| `app/cli/` | `resindeck-cli` |
| `static/` | the web interface: plain JavaScript modules, three.js, `i18n.js` |
| `third_party/` | vendored libraries, each with a README saying where it comes from |
| `packaging/` | icon, menu entry, AppImage and Windows scripts, Windows installer |

Guidelines:
- `core/` has no user interface and is tested on its own. Keep printer and file logic there.
- The web server listens on the network: every `/api` route and `/ws` need a PIN session unless the request
  comes from this computer (see `Server::handle`). Settings, files on disk and pairing must call
  `localOnly()`. See [SECURITY.md](SECURITY.md).
- Do not commit sliced files, settings or passwords.

## Adding a printer brand

Each printer family is a **driver**: a subclass of `Driver` ([`core/printers/driver.h`](core/printers/driver.h))
listed in [`core/printers/drivers.cpp`](core/printers/drivers.cpp). The window, the web server and the
command line only talk to that contract, so a new brand touches no other file.
[`core/printers/halot.cpp`](core/printers/halot.cpp) is a complete example.

### 1. Write the driver

```cpp
class MyBrandDriver : public Driver {
public:
    MyBrandDriver(const QString &host, QString password) : Driver(host, std::move(password)) {}

    static const DriverInfo &driverInfo() {
        static const DriverInfo info{
            .id = "mybrand",              // stored in settings.json, never change it later
            .name = "MyBrand",            // shown in the interface
            .models = "Model X (tested)",
            .extensions = {".goo"},       // files the printer accepts, lower case
            .password = false,            // true if status and commands need the printer password
            .passwordMax = 0,             // longest password the printer accepts (0: no limit)
        };
        return info;
    }
    const DriverInfo &info() const override { return driverInfo(); }

    void connect() override;              // open the connection; set version_ if you can
    void close() override;
    void ping() override;                 // cheap "still there?" check, without a password
    Status status() override;             // state, file, layer, layers, remaining, code, raw
    QJsonObject upload(const QString &path, const Progress &progress) override;  // call progress(percent)
    QJsonObject start(const QString &filename) override;
    QJsonObject pause() override;
    QJsonObject resume() override;
    QJsonObject stop() override;
    void abort() override;                // optional: make a waiting call fail now (app closing)

    static bool probe(const QString &host, int timeoutMs);  // "is one of my printers here?"
};
```

Rules of the contract:
- **Blocking calls:** each method waits for the printer's answer. Every printer runs in its own thread, so
  plain step-by-step code is fine. To wait for network replies, run a `QEventLoop` with a timeout, as
  `Halot::receive` does.
- **Status:** translate the printer's state into one of the states in `status.h` (`Idle`, `Printing`,
  `Paused`, `Finished`…). Put the printer's own name for it in `code` and its full reply in `raw`. Anything
  you cannot map becomes `Unknown`, and the interface shows `code`.
- **Errors:** throw `PrinterError(message, code, params)` for problems the user can fix (wrong password,
  file refused), and `NetworkError` when the connection fails: ResinDeck shows the printer as offline and
  reconnects. Add every new `code` to `static/i18n.js` as `errors.<code>`, in English and Portuguese.
- **probe()** runs for every address on the network, many at once. Keep it around a second and never send
  anything that changes the printer.
- `host_` is the address the user typed (it may end in `:port`) and `password_` the password.

### 2. Register it

Add an entry to `driverTypes()` in [`core/printers/drivers.cpp`](core/printers/drivers.cpp). It then shows
up in **+ add**, in the network search and in `resindeck-cli --driver mybrand`.

### 3. Test it without the printer

Write a fake printer that speaks your protocol, like [`core/tests/fakeprinter.h`](core/tests/fakeprinter.h)
does for the HALOT, and test the driver against it, like `core/tests/test_halot_driver.cpp`. The tests then
run in CI and nobody needs your printer to change the code safely. `everyDriverFillsInItsInfo` checks every
registered driver. Then try it on the real printer and say in the pull request which model and firmware you
used.

Please also document the protocol, as [docs/PROTOCOL.md](docs/PROTOCOL.md) does for the HALOT.

### Another file format

If the printer uses another sliced format, add a subclass of `SlicedFile`
([`core/formats/sliced.h`](core/formats/sliced.h)): header info, thumbnails and layer images. The 3D view
and the web interface work from those. Register its extensions in `loadSliced()` and `slicedExtensions()`.
[`core/formats/cxdlp.cpp`](core/formats/cxdlp.cpp) is the reference, and its `Writer` shows how to build
test files.

## Tests

The tests need no printer and no sliced files: `core/tests/fakeprinter.h` is a fake HALOT that speaks the
network protocol (including the error cases), and `cxdlp::Writer` writes small synthetic `.cxdlpv4` files.
Add a test with every change.

If you have real `.cxdlpv4` files from HALOT BOX or Chitubox, point `RESINDECK_SAMPLE_FILES` at their folder:
`test_cxdlp` then reads each one, writes it again with `cxdlp::Writer` and checks the result is the same file.
It is slow in the sanitizer build, so run it from a release build:

```sh
RESINDECK_SAMPLE_FILES=~/prints QTEST_FUNCTION_TIMEOUT=1800000 build/cpp-release/core/tests/test_cxdlp
```

Every pull request runs `.github/workflows/ci.yml`:
- lint (JavaScript and shell syntax);
- build with warnings as errors and the tests on Linux (with AddressSanitizer and UBSan) and Windows;
- the packages: it builds the AppImage and the Windows installer, starts them and checks that the web
  interface answers.

## Packages and releases

```sh
packaging/build_appimage.sh   # Linux: dist/ResinDeck-<version>-x86_64.AppImage (downloads linuxdeploy)
packaging/build_windows.sh    # Windows, in Git Bash: a portable .zip, and an installer with Inno Setup
```

To publish a release, set the version in `project(ResinDeck VERSION …)` in `CMakeLists.txt`, merge, then
push a matching tag:

```sh
git tag v1.0.1 && git push origin v1.0.1
```

The `Release` workflow runs CI, which builds and checks the packages, and attaches them to a GitHub release.
Run `packaging/make_icon.py` after changing the icon.

The Python version (0.x) is kept in the `legacy` branch and no longer maintained.

## Translations

The strings of the window and of the web interface both live in `static/i18n.js`. To add a language, copy
the `en` block, give it a language tag (for example `"es"`), translate the values and keep the keys.
Missing keys fall back to English. The window picks the new language up on its own; for the web interface,
add a short label for it in `initLanguage()` in `static/app.js`.

Errors reach both interfaces with a `code`, so they are translated through the `errors.*` keys too. The
command line stays in English.
