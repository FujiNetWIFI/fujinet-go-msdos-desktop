# FujiNet Go — MS-DOS

IBM PCs from the 5150 to the Pentium with a built-in
[FujiNet](https://fujinet.online/) on a serial port: boot FreeDOS with
FujiNet's driver, browse network hosts and the SD card in CONFIG, mount disk
images as DOS drives, and run network programs, all in one desktop app. A
member of the FujiNet Go desktop family.

| | |
|---|---|
| **Emulator** | [86Box](https://86box.net/), the [FujiNetWIFI fork](https://github.com/FujiNetWIFI/86Box/tree/feat/fujinet) (upstream plus its `fujinet` COM device and `EMBEDDED_HOST`), built in as a static library. |
| **Machines** | IBM 5150 PC, IBM 5160 PC/XT, IBM 4860 PCjr, a typical 286 (AMI), 386 (ECS, Am386DX-40), 486 (DTK, 486DX2-66) and Pentium (ASUS P/I-P54TP4XE, 133 MHz). CPU, RAM and FPU per machine. |
| **Video** | MDA, CGA, EGA, VGA, Trident TVGA 8900 SVGA; the PCjr's own. |
| **Sound** | AdLib, Sound Blaster 1.0, 2.0, Pro 2, 16, AWE32, Gravis UltraSound; MT-32 / CM-32L MIDI. PC speaker always; the PCjr's SN76489. |
| **Drives** | Floppies (360K–2.88M) and hard disk images (raw or VHD). The 5150/5160 choose their controller: IBM (Xebec), WD1002A-WX1, DTC 5150X, Seagate ST-11M, XT-IDE; the rest use their built-in IDE. |
| **BIOS** | Not included. **Machine ▸ Import BIOS…** takes files, folders or zips (86Box's ROM set works), matched by CRC-32. |
| **FujiNet** | The firmware's `RS232` PC target, in-process as `libfujinet`, on COM1–COM4 (default **COM2**; the PCjr uses its own serial port): FujiBus over TCP **11516**, web UI **11517** (loopback). |
| **Boot floppy** | Built in: FreeDOS 1.3, `FUJINET.SYS` and FujiNet's DOS tools; CONFIG.SYS follows the COM port and speed. |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32): display, Ctrl+Alt+Del and Reset buttons, Controllers panel, Preferences, FujiNet log, debugger, Help. |
| **Packaging** | DEB/RPM/TGZ per Linux frontend, two Flatpaks, Windows zip and NSIS installer, macOS arm64 and x86_64 bundles. |

## Using it

**Help ▸ FujiNet Go MS-DOS Help** (Ctrl+Shift+H, ⌘? on a Mac) covers it
all: the machines, importing a BIOS, the drives, FujiNet, every setting in
Preferences, the keys, the Controllers panel and the debugger. The text is
[data/help/help.md](data/help/help.md), built into every frontend; the
`help` test fails when a Preferences setting or choice is missing from it.

## Building

    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build

Needs CMake 3.20+, a C11/C++17 compiler, SDL3, zlib, freetype, libpng,
libsndfile, zstd and a frontend toolkit (GTK 4.12 + libadwaita 1.5, or Qt 6).
The FujiNet runtime build also needs Python 3 with PyYAML and Jinja2,
OpenSSL and expat; `-DWITH_FUJINET=OFF` skips it. 86Box, fujinet-firmware
and Zydis are fetched at their pins (`cmake/Dependencies.cmake`);
`-DBOX86_SRC=<checkout>` builds a local 86Box instead.

The tests boot GLaBIOS (GPL, `core/tests/fixtures`) in place of IBM's BIOS,
so they run anywhere, FujiNet netboot on every COM port included.

- **Flatpak:** `flatpak-builder --user --install build-flatpak
  build-aux/flatpak/online.fujinet.go.msdos.gnome.yml` (or `.kde.yml`).
- **Windows:** natively in MSYS2 UCRT64.
- **macOS:** built on CI (`.github/workflows/macos.yml`).

**FujiNet updates itself.** A local build checks fujinet-firmware's `master`
(at most every ten minutes). When it is newer and every patch still applies,
the build moves all three pins (`cmake/Dependencies.cmake` and both flatpak
manifests) for you to commit; when a patch no longer applies it keeps the old
pin and says which. CI, flatpak and release builds use the committed pin.
`tools/fujinet/update-fujinet-firmware.sh` does it by hand.

## Licences

GPL-3.0-or-later (`LICENSE`). 86Box is GPL-2.0-or-later, FreeDOS GPL-2.0,
Zydis MIT. No BIOS or other copyrighted ROM is in any published build
(`COMPLIANCE.md`).
