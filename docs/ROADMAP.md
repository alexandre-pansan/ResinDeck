# ResinDeck roadmap: a native slicer

ResinDeck today is a Python server with a browser interface. It sends sliced files to printers, follows prints, and shows sliced files in 3D. This plan turns it into a **native desktop application in C++ and Qt**: a full resin slicer that covers what Chitubox does, while keeping the browser and phone interface for following and controlling prints.

This is a living document. Each phase is planned in detail before it starts, and this file is updated as decisions change.

## Goals

- **One application for the whole workflow:** open a model, orient it, hollow it, add supports, slice, send to the printer, and follow the print.
- **Cover Chitubox's features:** first those of Chitubox Basic (phases 1–3), then those of Chitubox Pro (phase 4). ResinDeck matches the *features*. It does not copy Chitubox's interface, icons or name.
- **Keep the phone and browser access:** the current web interface (follow, start, pause, stop, library, 3D preview, PIN pairing) keeps working, served by the desktop application.
- **Stay open to other brands:** printer support stays in drivers, one per brand, and file formats stay in readers and writers, one per format.

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Language and toolkit | **C++20 and Qt 6** | Direct control of the GPU for the 3D editor and for slicing, and no browser memory limits. The HALOT protocol already uses Qt's `qCompress` framing, so Qt's version matches it byte for byte. |
| Build | CMake | The standard for C++ and Qt. |
| User interface | **Qt Widgets**, with the 3D viewport in a `QRhiWidget` | A slicer is a dense tool interface (panels, settings, shortcuts), where Widgets is strongest, and it keeps everything in C++. Touch is not needed: phones use the web interface. |
| Target | Chitubox Basic, then Pro | Phases 1–3 reach Basic. Phase 4 adds the Pro features. |
| Platforms | Linux and Windows first | The same platforms as today. macOS when someone can test it. |
| License | MIT, unchanged | Dependencies must be MIT-compatible: MIT, BSD, Apache 2.0, MPL 2.0, or LGPL linked dynamically (Qt). **GPL and AGPL code cannot be used**, which rules out copying from PrusaSlicer and using CGAL. Reading their documentation and papers for ideas is fine. |
| Web interface | Kept, served by the desktop app | The `static/` files (HTML, CSS, JS) are reused unchanged. The C++ app reimplements the same HTTP and WebSocket API, so the phone interface does not need to change. |

## Architecture

Drivers have a **blocking** interface: each method waits for the printer's answer. Every printer runs in its own
thread, so a slow printer never freezes the window or the server, and a driver for another brand is plain
step-by-step code.

```
app/          desktop application: main window, 3D viewport, tools, settings
core/         no user interface, tested on its own
  printers/   driver contract and one driver per brand (HALOT first)
  formats/    readers and writers of sliced files (.cxdlpv4 first)
  mesh/       loading (STL, OBJ, 3MF), repair, transforms, hollowing
  supports/   automatic and manual supports
  slicer/     GPU slicing into layer images, anti-aliasing, estimates
  remote/     HTTP and WebSocket server for the browser and the phone, PIN pairing, mDNS
  tests/      tests of the core
web/          the browser and phone interface (today's static/)
```

The desktop application starts the `remote` server in the same process, the way the Python server works today. With the desktop app open, the phone works as it does now, through `resindeck.local`, the QR code and the PIN.

### Libraries

| Need | Library | License |
|---|---|---|
| Window, widgets, network, WebSocket, HTTP server | Qt 6: Widgets, Network, WebSockets, HttpServer | LGPL 3 |
| 3D rendering | Qt RHI through `QRhiWidget` (Vulkan, Direct3D, Metal or OpenGL) | LGPL 3 |
| Mesh booleans, offsets and repair | [manifold](https://github.com/elalish/manifold) | Apache 2.0 |
| Hollowing (signed distance fields) | [OpenVDB](https://www.openvdb.org/) | MPL 2.0 / Apache 2.0 |
| Geometry helpers | [libigl](https://libigl.github.io/) and [Eigen](https://eigen.tuxfamily.org/) | MPL 2.0 |
| 3MF files | [lib3mf](https://github.com/3MFConsortium/lib3mf) | BSD 2-Clause |
| QR code | [qrcodegen](https://github.com/nayuki/QR-Code-generator) | MIT |
| mDNS (`resindeck.local`) | [mdns](https://github.com/mjansson/mdns) (single header) | Public domain |
| DES for the HALOT token | A small built-in implementation (single 8-byte block) | MIT (ours) |
| Tests | Qt Test or [Catch2](https://github.com/catchorg/Catch2) | LGPL 3 / BSL 1.0 |

Each library is confirmed when its phase starts. If a library does not fit, it is replaced with another MIT-compatible one.

## Moving from Python to C++

The Python version is the working product and stays the release line (0.3.x) until the C++ version does everything it does.

1. The C++ app is developed under `app/` and `core/` next to the Python code, with its own CI jobs.
2. When the C++ app reaches **feature parity with the Python version** (end of phase 0), it becomes ResinDeck 1.0. The Python code moves to a `legacy` branch, and `main` keeps only the C++ app.
3. The Python tests are the specification for the port: the fake HALOT, the synthetic `.cxdlpv4` files, and the server security tests (Origin check, PIN, rate limit, local-only endpoints) are ported first.

Settings and paired devices carry over: the C++ app reads the same `settings.json`.

## Phases

Each phase ends with a release, and with a real print on a HALOT-R6 once slicing exists.

### Phase 0: native base, parity with today

Everything the Python version does, in C++.

- [x] CMake project with Qt 6; CI that builds on Linux and Windows and runs the tests
- [x] `core/printers`: driver contract, normalized status, HALOT driver (token, WebSocket, compressed upload, start, pause, resume, stop, probe)
  - [x] normalized status and printer errors
  - [x] HALOT token (DES) and status parsing, checked against the Python version
  - [x] driver contract and registry; WebSocket connection, upload and commands, probe; tested against a fake HALOT
  - [x] checked on a real HALOT-R6: version, status and probe (read only)
  - [x] upload, print, pause, resume and stop on a real HALOT-R6
- [x] Printer manager: one thread per printer, polling, reconnection, commands and uploads (`core/printers/printer`)
- [x] Printer tracking by MAC address after DHCP changes; network scan (`core/net/lan`)
- [x] Print start and expected end times
- [x] `core/formats`: `.cxdlpv4` reader (header, previews, RLE layers, checksum), 3D grid from layers; identical to the Python reader on 27 real files, about 3× faster
- [x] Main window (Qt Widgets): printers, library, file info, 3D preview on the GPU, layers, send and print, phone pairing; strings shared with the web interface
- [x] `core/remote`: the current HTTP and WebSocket API, serving the web interface; PIN pairing, sessions, rate limit, security headers, Origin and Host checks, local-only endpoints. Same answers as the Python server, endpoint by endpoint
- [x] `resindeck.local` over mDNS; QR code
- [x] Command line, like today's `resindeck.py` (`resindeck-cli`)
- [ ] Packages: AppImage (`linuxdeploy` with the Qt plugin) and a Windows installer (`windeployqt`, Inno Setup)
  - [x] install rules, packaging scripts and a CI job that builds and starts both
  - [x] first successful CI run
  - [ ] tested on a real Windows machine
- [x] Port the tests: fake HALOT, synthetic `.cxdlpv4` files, server security tests

**Result:** ResinDeck 1.0, native. It does everything 0.3 does. **Released.**

The Python code moved to the `legacy` branch, and the release workflow publishes the native packages.

### Phase 1: slice and print

- [ ] Open STL, OBJ and 3MF, several models at a time
- [ ] Workspace with the printer's build plate and volume
- [ ] Move, rotate, scale, mirror, duplicate, lay flat, auto-arrange
- [ ] Printer profiles: resolution, pixel size, build volume, file format
- [ ] Resin profiles: layer height, exposure, bottom layers, lift and retract speeds, wait times
- [ ] GPU slicing into layer images; anti-aliasing; image blur
- [ ] `.cxdlpv4` writer
  - [x] the same layout as HALOT BOX and Chitubox files: both previews, print settings, per-layer lift
    settings and area; 27 real files rewritten byte for byte (`RESINDECK_SAMPLE_FILES`)
  - [ ] a file written by ResinDeck printed on the HALOT-R6
- [ ] Layer viewer: slider, layer image, model cross-section
- [ ] Estimates: print time, resin volume, weight and cost
- [ ] Send to the printer and start, as today

**Result:** prints models that come pre-supported. **Checkpoint: a real print on the HALOT-R6 from a file ResinDeck sliced.** Phase 2 does not start before this works.

### Phase 2: prepare the model

- [ ] Mesh analysis and repair: holes, flipped normals, non-manifold edges, intersecting shells
- [ ] Island detection (parts that start in mid-air), shown in the layer viewer
- [ ] Hollowing with a set wall thickness, with or without inner infill
- [ ] Drain holes: place, size, preview
- [ ] Measure tool
- [ ] Model information: size, volume, triangles

**Result:** hollow models and less resin.

### Phase 3: supports

The longest phase. Support quality decides whether a print succeeds, so each step is tested with real prints.

- [ ] Overhang and island analysis: where the model needs support
- [ ] Automatic supports with light, medium and heavy presets
- [ ] Support settings: contact tip shape and depth, body diameter, base, angles
- [ ] Raft (base) settings
- [ ] Manual editing: add, move and remove supports, select by area
- [ ] Support paths that do not pass through the model
- [ ] Tree (branching) supports
- [ ] Lifting the model off the plate, with a set height

**Result:** prints any model from scratch. This matches Chitubox Basic.

### Phase 4: Pro features

The features mostly found in Chitubox Pro. Before the phase starts, this list is checked against the current Chitubox versions.

- [ ] Cut models along a plane, with connectors
- [ ] Boolean operations: union, subtract, intersect
- [ ] Automatic orientation
- [ ] Suction cup (cupping) detection and fixes
- [ ] Inner infill patterns
- [ ] Text and logos embossed or engraved on models
- [ ] Multiple exposure settings in one file (test strips)

**Result:** matches Chitubox Pro.

## Testing

- `core/` has no user interface, so it is tested on its own, with no printer and no GPU where possible.
- The slicer is tested by slicing simple shapes (cube, cylinder, overhang) and comparing the layer images with expected ones.
- File writers are tested by writing, then reading back with the reader.
- The fake HALOT tests the printer protocol, as it does today.
- Real prints are the final test for slicing (phase 1), hollowing (phase 2) and supports (phase 3). Each phase lists the prints that must succeed before release.
- CI builds on Linux and Windows, runs the tests, measures coverage, and runs static analysis (clang-tidy) and sanitizers (AddressSanitizer, UBSan).

## Risks

| Risk | Impact | Mitigation |
|---|---|---|
| The printer rejects files ResinDeck writes | Phase 1 is blocked | Compare our files byte by byte with Chitubox's for the same model; the reader already knows the format. Test on the real printer before anything else in phase 1. |
| Supports that fail in real prints | Wasted resin, users lose trust | Ship presets only after test prints; let users edit every support by hand; mark automatic supports as experimental until proven. |
| Rewriting in C++ takes a long time before anything new appears | Nothing new for months | Phase 0 is a straight port with the Python tests as the specification, and the Python version stays maintained meanwhile. |
| Larger packages | Downloads grow from tens to hundreds of MB | Ship only the Qt modules in use; compress the AppImage. |
| GPU drivers on users' machines | Rendering or slicing fails on some computers | Use Qt RHI, which picks Vulkan, OpenGL or Direct3D; keep a CPU fallback for slicing. |
| Encrypted file formats on newer printers | Some printers cannot be supported | Support formats whose layout is documented or readable. |

## Open questions

- How many printers to support in phase 1 besides the HALOT family. This depends on contributors and on access to printers for testing.
- Whether to read other slicers' project files (for example Chitubox `.chitubox` projects). This depends on whether the format is documented.
