# Compliance

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **86Box** (the emulator) | [FujiNetWIFI/86Box](https://github.com/FujiNetWIFI/86Box) `feat/fujinet`: upstream [86Box](https://github.com/86Box/86Box) plus the `fujinet` COM device and `EMBEDDED_HOST`; pinned in `cmake/Dependencies.cmake` | GPL-2.0-or-later | built as a static library by `cmake/Box86Core.cmake`, with this repository's platform layer (`core/86box/host/`) |
| munt (MT-32 emulation) | bundled in 86Box | LGPL-2.1-or-later | compiled into 86Box |
| **FujiNet firmware** (`libfujinet`) | [FujiNetWIFI/fujinet-firmware](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `RS232`, plus `tools/fujinet/patches/` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`ed at run time |
| **FreeDOS 1.3** kernel, FreeCOM, boot sector | [FreeDOS](https://www.freedos.org/) 1.3 floppy edition (`tools/disks/freedos/`) | GPL-2.0-or-later | on the built-in boot floppy |
| **FUJINET.SYS** and FujiNet's DOS programs | [FujiNetWIFI/fujinet-msdos](https://github.com/FujiNetWIFI/fujinet-msdos) release `v0.0.1.da8bc7` (`tools/disks/fujinet-msdos/`); the firmware's RS232 `autorun.img` | GPL-3.0 | on the built-in boot floppy |
| Zydis | [zyantific/zydis](https://github.com/zyantific/zydis) 4.1.0, amalgamated | MIT | the debugger's disassembler |
| SDL3 | libsdl-org | Zlib | audio and gamepads; system package on Linux, static in releases |
| zlib, freetype, libpng, libsndfile, zstd | their projects | Zlib, FTL/GPL-2.0, libpng, LGPL-2.1, BSD | 86Box's and the session's; system libraries on Linux, shipped beside the app on Windows and in the bundle on macOS |
| mbedTLS 3.6.x, OpenSSL 3, expat | Mbed-TLS, openssl.org, libexpat | Apache-2.0, Apache-2.0, MIT | for `libfujinet` |

The combined work is distributed under GPL-3.0.

## BIOS and other ROMs — none ship

The machines' BIOSes (IBM, AMI, Award, ...), the EGA, VGA and Trident video
BIOSes, the XT disk controllers' ROMs, the AWE32 and MT-32 ROMs are
copyrighted. **This project distributes none of them.**

- **Machine ▸ Import BIOS…** copies the user's own images into the data
  directory, matched by size and CRC-32 against a table generated from
  86Box's ROM set (`tools/roms/make-rom-table.py`, `core/src/rom_table.c`).
  A machine or card whose ROM is missing is greyed out.
- The `no_embedded_roms_*` tests check every built frontend against a
  developer's own ROM set under `tools/roms/` (gitignored).
- The tests carry one BIOS: GLaBIOS 0.4.0 (`core/tests/fixtures/`, GPL-3.0),
  so CI can boot a machine. It is not in any published build.

## Icon and name

The icon is the FujiNet Go family mark in an IBM PC's badge ink on its case
grey (`tools/icons/make-icons.py`). "IBM", "PC", "PC/XT" and "PCjr" are
trademarks of International Business Machines, "MS-DOS" of Microsoft, used to
name what is emulated; the project is not affiliated with or endorsed by
either.
