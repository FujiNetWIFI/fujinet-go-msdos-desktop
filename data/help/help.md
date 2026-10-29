This is the text of FujiNet Go MS-DOS's built-in Help (Help ▸ FujiNet Go
MS-DOS Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to
the frontend it is about; here you see them all. Every topic below is one
page in the app.

# Getting started

FujiNet Go MS-DOS is an IBM PC with a FujiNet on one of its serial ports.
It runs the machines from the 1981 IBM PC to a Pentium on
[86Box](https://86box.net/), boots FreeDOS with FujiNet's driver, and lets
DOS use disk images and programs from the network.

1. **Import a BIOS.** No BIOS comes with the app. Use **Import BIOS…** on the
   first page, or in the Machine menu, and pick the BIOS files you have, a
   zip of them, or a folder such as an 86Box ROM set. See
   [BIOS and ROMs](#bios-and-roms).
2. **The PC starts.** It boots the FujiNet boot floppy in A: and runs
   FujiNet's CONFIG. On a 4.77 MHz PC or XT that takes about half a minute;
   **Warp Speed** (<kbd>Ctrl+Shift+W</kbd>) gets there sooner.
3. **Pick something in CONFIG.** Choose a host (the SD card or a server on
   the network), a disk image on it and a slot to mount it in. DOS sees each
   mounted slot as a drive. See [FujiNet](#fujinet).
4. **Or use your own disks.** **Open…** (<kbd>Ctrl+Shift+O</kbd>) puts a
   floppy image in A: or attaches a hard disk image. See
   [Drives and disks](#drives-and-disks).
5. **Change the PC.** [Preferences](#preferences) picks the machine and its
   processor, memory, cards, drives and ports.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers panel, the FujiNet
console log and the first page opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-msdos-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The PC's screen fills the window. Above it, or beside the menus:

- **Ctrl+Alt+Del** sends the three keys: DOS restarts (a warm boot).
- **Reset** presses the reset button: the PC starts again from cold
  (<kbd>Ctrl+Shift+R</kbd>). A new hard disk, and Preferences, take effect
  with a reset.
- **The FujiNet light** is lit while the PC's COM port is connected to
  FujiNet.
- **The status line** says the machine, then FujiNet's state: *FujiNet on
  COM2* (connected), *FujiNet waiting* (running, not connected yet),
  *FujiNet stopped* or *FujiNet off* (*External FujiNet* for another
  instance). After that comes *Paused*, *Warp*,
  *Stopped* (the Debugger has stopped it), *Off*, or the speed when the
  host cannot keep up (*72%*).
- **The drive lights** A: to D: show the PC's drives and light up while one
  is in use.

<!-- only: gnome -->

On GNOME, the buttons, the light, the status line and the drive lights are
in the header bar.

<!-- end -->

<!-- only: kde macos -->

The buttons, the light, the status line and the drive lights are in the
toolbar.

<!-- end -->

<!-- only: windows -->

On Windows, the buttons are under the menu bar; the FujiNet light, the
status line and the drive lights are in the status bar at the bottom.

<!-- end -->

Messages (a disk inserted, a gamepad plugged in, FujiNet restarted) show for
a few seconds over the picture or in the status line.

## The menus

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar):

- **Machine:** Open…, Ctrl+Alt+Del, Reset, Pause, Warp Speed, Import BIOS…,
  Import BIOS from a Folder…
- **Drives:** a submenu for each of the PC's drives, and FujiNet's SD card.
- Copy Screen Text and Paste.
- Controllers, Debugger, 4:3 Picture, Whole-Number Scaling, Smooth Scaling
  and Fullscreen.
- **FujiNet:** FujiNet Web UI, Console Log, Restart FujiNet.
- Preferences, Help, About FujiNet Go MS-DOS and Quit.

<!-- end -->

<!-- only: kde windows -->

The menu bar has:

- **Machine:** Open…, Ctrl+Alt+Del, Reset, Pause, Warp Speed, Import BIOS…,
  Import BIOS from a Folder…, Preferences…, and Quit (Exit on Windows).
- **Drives:** a submenu for each of the PC's drives, and FujiNet's SD card.
- **Edit:** Copy Screen Text, Paste.
- **View:** Controllers, Debugger, 4:3 Picture, Whole-Number Scaling,
  Smooth Scaling, Fullscreen.
- **FujiNet:** FujiNet Web UI, Console Log, Restart FujiNet.
- **Help:** FujiNet Go MS-DOS Help, About FujiNet Go MS-DOS.

The <kbd>Alt</kbd> key is the PC's, so it does not open the menus: use the
mouse, or the shortcuts in [Keyboard and mouse](#keyboard-and-mouse).

<!-- end -->

<!-- only: windows -->

On Windows, <kbd>F10</kbd> is the PC's too, and in fullscreen the menu bar
is hidden: <kbd>Ctrl+Alt+PgDn</kbd> or <kbd>Ctrl+Shift+F</kbd> brings it
back.

<!-- end -->

<!-- only: macos -->

On a Mac, the menu bar has:

- **FujiNet Go MS-DOS:** About, Preferences…, Hide, Quit.
- **Machine:** Open…, Ctrl+Alt+Del, Reset, Pause, Warp Speed, Import BIOS…,
  Import BIOS from a Folder…
- **Drives:** a submenu for each of the PC's drives, and FujiNet's SD card.
- **Edit:** Copy Screen Text, Paste to the PC, and the usual Cut, Copy,
  Paste and Select All for text fields.
- **View:** Controllers, Debugger, 4:3 Picture, Whole-Number Scaling,
  Smooth Scaling, Full Screen.
- **FujiNet:** FujiNet Web UI, Console Log, Restart FujiNet.
- **Window** and **Help** (FujiNet Go MS-DOS Help, About).

<!-- end -->

## Opening files

**Open…** (<kbd>Ctrl+Shift+O</kbd>) takes any file and puts it where it
belongs. Dropping a file on the window does the same, and so does naming
one when starting the app.

- **A floppy image** goes in A:.
- **A hard disk image** asks which drive, C: or D:, and is attached from the
  next reset.
- **A BIOS or ROM image**, or a zip of them, is imported.
- **Anything else** is copied to FujiNet's SD card (a zip is unpacked
  there), to mount in CONFIG.

[Drives and disks](#drives-and-disks) lists which files are which.

## Running the PC

- **Pause** stops the PC until you choose it again.
- **Warp Speed** (<kbd>Ctrl+Shift+W</kbd>) runs the PC as fast as the
  computer can, with no sound. It is handy for the memory test and boot of
  the slower machines; FujiNet's link stays at its own speed.
- **Reset** and **Ctrl+Alt+Del** are above.
- There are no saved states, as on a real PC. Quitting turns the PC off;
  what DOS wrote to a disk image is on it.

## The picture

These are in the View menu and in Preferences ▸ Display, and apply at once:

- **4:3 Picture** shapes the picture like a PC monitor's; off, every pixel
  is square.
- **Whole-Number Scaling** draws the picture at 1×, 2×, 3× … its size only,
  with a border, so every pixel is the same size.
- **Smooth Scaling** blends pixels when the picture is scaled; off, they
  stay sharp.
- **Fullscreen** (<kbd>Ctrl+Alt+PgDn</kbd>, or <kbd>Ctrl+Shift+F</kbd>)
  fills the screen; the same key comes back.

# Keyboard and mouse

Every key goes to the PC, <kbd>F1</kbd> to <kbd>F12</kbd>,
<kbd>Ctrl</kbd> and <kbd>Alt</kbd> included, while the PC's window is in
front and no dialog is open. Keys go by where they are on the keyboard,
like a US PC keyboard, whatever your keyboard's layout.

The app keeps these for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+Shift+O</kbd> | Open… |
| <kbd>Ctrl+Shift+R</kbd> | Reset |
| <kbd>Ctrl+Shift+W</kbd> | Warp Speed |
| <kbd>Ctrl+Shift+C</kbd> | Copy Screen Text |
| <kbd>Ctrl+Shift+V</kbd> | Paste |
| <kbd>Ctrl+Shift+K</kbd> | Controllers |
| <kbd>Ctrl+Shift+D</kbd> | Debugger |
| <kbd>Ctrl+Shift+F</kbd> | Fullscreen |
| <kbd>Ctrl+Alt+PgDn</kbd> | Fullscreen |
| <kbd>Ctrl+End</kbd> | Lets go of a captured mouse |
| <kbd>Ctrl+Shift+P</kbd> | Preferences |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>Ctrl+Shift+Q</kbd> | Quit |

<!-- only: gnome kde windows -->

The PC never gets <kbd>Ctrl</kbd>+<kbd>Shift</kbd> with a letter.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

<!-- only: windows -->

On Windows, combinations with the Windows key, <kbd>Alt</kbd>+<kbd>F4</kbd>
and <kbd>Alt</kbd>+<kbd>Space</kbd> are Windows's.

<!-- end -->

<!-- only: macos -->

On a Mac, the PC's <kbd>Ctrl</kbd> and <kbd>Alt</kbd> are Control and
Option, and ⌘ is the Mac's: pressing it lets go of every PC key held, and
⌘C and ⌘V in the PC's window copy the screen text and paste to the PC. A
Mac keyboard's Help key, or Insert on a PC keyboard, is the PC's Insert;
F13, F14 and F15 are Print Screen, Scroll Lock and Pause.

<!-- end -->

## Keys a PC has

- <kbd>Alt</kbd>+<kbd>PrtSc</kbd> is SysRq, and <kbd>Ctrl</kbd>+<kbd>Pause</kbd>
  is Break.
- The Controllers panel has the machine's own keyboard, SysRq, Pause and
  Scroll Lock included. See [Controllers](#controllers).

## Copy and paste

- **Copy Screen Text** (<kbd>Ctrl+Shift+C</kbd>) puts the text on the
  PC's screen on the clipboard. It works in the text modes only; in a
  graphics mode there is no text to copy.
- **Paste** (<kbd>Ctrl+Shift+V</kbd>) types the clipboard's text on the
  PC's keyboard, as fast as the PC takes keys in. A character that is not
  on a US keyboard is typed as <kbd>Alt</kbd> and its code page 437 number
  on the keypad.

## The mouse

The PC has a mouse only when one is fitted in Preferences ▸ Ports (see
[Ports settings](#ports-settings)).

- Click the picture to capture the mouse: the pointer goes away and moves
  the PC's mouse.
- <kbd>Ctrl+End</kbd>, or the middle button, lets it go. So does switching
  to another window.
- The wheel goes to the PC while the mouse is captured.

# Machines

There are seven machines. Each one is a profile: it keeps its own
processor, memory, cards, drives, ports and disks, so switching to another
machine and back brings everything back as it was. The machine is chosen in
Preferences ▸ Machine; the picture, the volume and FujiNet on or off are the
same for every machine.

- **IBM 5150 PC** (1981): an Intel 8088 at 4.77 MHz (up to 16), 64 to 640
  KB, CGA, the PC speaker and a 5.25" 360K drive. Its hard disk controllers
  are the XT's.
- **IBM 5160 PC/XT**: the same, with 256 to 640 KB.
- **IBM 4860 PCjr**: an 8088 at 4.77 MHz, 128 to 640 KB, its own video and
  sound (an SN76489), one 360K drive and its own serial port, which is
  FujiNet's. It has no hard disk, and its cards and drives can't be
  changed.
- **Typical 286**: an AMI BIOS board with a 286 (6 to 25 MHz), 512 KB to 8
  MB, VGA, a Sound Blaster Pro 2, a 1.2M and a 1.44M drive and IDE.
- **Typical 386**: an ECS Panda 386V with an AMD Am386DX at 40 MHz (or an
  Intel i386DX), 1 to 64 MB, a Trident SVGA, a Sound Blaster 16, a 1.44M
  and a 1.2M drive and IDE.
- **Typical 486**: a DTK board with an i486DX2 at 66 MHz (486SX, DX and DX2
  from Intel, Am486DX4 from AMD), 1 to 128 MB; cards and drives as the 386.
- **Typical Pentium**: an ASUS P/I-P55TP4XE with a Pentium at 133 MHz (75 to
  200), 8 to 128 MB; cards and drives as the 386.

A machine can only start once its BIOS is imported. If the chosen one has
none, the app starts the first one that has and says so.

<!-- context: nobios -->
# BIOS and ROMs

The machines' BIOS chips and the cards' ROMs are copyrighted, so none come
with the app. Import the ones you have:

- **Import BIOS…** (in the Machine menu, on the first page, and on the ROMs
  page of Preferences) takes single images or zips.
- **Import BIOS from a Folder…** takes a whole folder and everything in
  it, such as an 86Box ROM set.
- Opening or dropping a BIOS image or a zip of ROMs imports it too.
- The 86Box project's ROM set is a free download from
  [86Box ROM releases](https://github.com/86Box/roms/releases); the
  first page and the ROMs page of Preferences link to it.

Files are recognised by their size and checksum (86Box's ROM set), whatever
they are called; anything else in a folder or zip is left alone. The
message says what is now available. Imported files are copied into the
app's `roms` folder (see [Files and folders](#files-and-folders)), so the
originals can go.

A card or drive whose ROM is missing shows as *(not available)* in
Preferences until its ROM is imported.

<!-- context: prefs/ROMs -->
## The ROMs page

Preferences ▸ ROMs lists every ROM the app can use, *Imported* or *Not
imported*, with **Import…** and **Import a Folder…** buttons, and says
where they are kept.

- **Machines:**
  - IBM 5150 PC BIOS (1982): the IBM 5150 PC.
  - IBM Cassette BASIC 1.10: the 5150's BASIC, for when there is no disk
    to boot (optional).
  - IBM 5160 PC/XT BIOS (1986): the IBM 5160 PC/XT.
  - IBM 4860 PCjr BIOS: the PCjr.
  - AMI 286 BIOS (DataExpert NEAT): the Typical 286.
  - ECS Panda 386V BIOS: the Typical 386.
  - DTK PKM-0038S E-2 BIOS: the Typical 486.
  - ASUS P/I-P55TP4XE BIOS: the Typical Pentium.
- **Video:**
  - IBM MDA/CGA character set: MDA, CGA and the PCjr's video.
  - IBM EGA BIOS: EGA.
  - IBM VGA BIOS: VGA.
  - Trident TVGA 8900 BIOS: the SVGA card.
- **Disk controllers** (for the 5150 and 5160):
  - IBM Fixed Disk Adapter (Xebec) BIOS
  - Western Digital WD1002A-WX1 BIOS
  - DTC 5150X BIOS
  - Seagate ST-11M BIOS
  - XTIDE Universal BIOS (XT): the XT-IDE controller.
- **Sound:**
  - Sound Blaster AWE32 wavetable: the Sound Blaster AWE32.
  - Roland MT-32 and Roland CM-32L: those MIDI synthesizers.

# Drives and disks

The PC's drives are in the Drives menu, which also has FujiNet's SD card.
Which drives a machine has is set in Preferences ▸ Storage (see
[Storage settings](#storage-settings)).

## Floppy disks

Each floppy drive, A: and B:, has:

- **Insert…**: a floppy image. It is in the drive at once.
- **Eject**.
- **Write-Protect** (or **Allow Writing**): the disk can't be written,
  like a write-protect tab.
- **FujiNet Boot Floppy**: the built-in boot floppy, sized for the drive.
  See [The FujiNet boot floppy](#the-fujinet-boot-floppy).
- **New Blank Disk…**: makes a formatted, empty floppy image of the drive's
  size, and inserts it.

Floppy images are `.img`, `.ima`, `.vfd`, `.flp` or `.dsk` (raw, of a
floppy's size), and `.86f`, `.imd`, `.td0`, `.fdi` and `.mfm`. Each drive
takes the sizes up to its own.

## Hard disks

A machine with a hard disk controller has C: and D:, each with:

- **Attach…**: a hard disk image. It is the drive from the next reset.
- **Detach**: from the next reset.
- **New Blank Disk…**: makes an empty image of the type set in Preferences
  ▸ Storage, raw if its name ends in `.img`, a fixed VHD if `.vhd`, and
  attaches it.

Hard disk images are `.vhd`, `.hdi` and `.hdx`, and `.img` or `.dsk` larger
than a floppy. An image keeps its own size; a blank one has to be
partitioned with FDISK and formatted with FORMAT, which are on your DOS's
disks, not on the FujiNet boot floppy.

With no hard disk attached, the 5150's and 5160's controller is left out,
so the BIOS does not stop at error 1701.

## FujiNet's SD card

FujiNet's SD card is a folder in the app's data (see
[Files and folders](#files-and-folders)). Its files show under the SD host
in CONFIG.

- **Import to SD Card…** copies a file there; a zip is unpacked.
- **Open SD Folder** opens it in your file manager.
- Opening or dropping a file the app doesn't use itself copies it there too.

# The FujiNet boot floppy

The built-in floppy boots FreeDOS with FujiNet's driver and runs CONFIG. It
is in A: on every machine until you put another disk in, and **FujiNet Boot
Floppy** in the drive's menu puts it back. It comes in the drive's size:
360K for 5.25" drives, 720K, or 1.44M (also for a 2.88M drive).

Its `CONFIG.SYS` loads `FUJINET.SYS` on FujiNet's COM port at its speed
(Preferences ▸ Ports), and `FUJIPRN.SYS`, FujiNet's printer:

```
DEVICE=FUJINET.SYS FUJI_PORT=2 FUJI_BPS=115200
DEVICE=FUJIPRN.SYS
LASTDRIVE=Z
FILES=20
BUFFERS=20
```

`AUTOEXEC.BAT` runs `CONFIG.EXE`. The rest are FujiNet's DOS programs:

| Program | Does |
|---|---|
| `FMOUNT` | mounts and unmounts FujiNet disk images as DOS drives |
| `FMALL` | mounts every FujiNet slot |
| `NCOPY` | copies files over the network |
| `NGET` | downloads a file from a URL |
| `NPUT` | sends a file to a URL |
| `FNSHARE` | mounts a network share FujiNet can reach as a drive |
| `FCONFIG` | shows FujiNet's network settings |
| `FRESET` | resets FujiNet |
| `CFGTSR` | stays resident: <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>F5</kbd> mounts the boot disk or runs CONFIG |

The floppy is a file in the app's `disks` folder, and what DOS writes to it
stays. A newer app replaces it only if it is still as the app left it.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, on one of
the PC's serial ports: COM2 unless set otherwise in Preferences ▸ Ports
(the PCjr's is its built-in port). `FUJINET.SYS` on the boot floppy follows
that port and its speed.

## CONFIG

CONFIG is FujiNet's program for choosing disks:

- **Host slots** are where disks come from: the SD card, and servers such
  as fujinet.online, apps.irata.online, fujinet.diller.org,
  FujiNet.Atari8bit.net, atarionline.eu, fujinet.pl and tnfs.abbuc.social.
- Pick a host, a disk image on it, and a **device slot** to mount it in.
- DOS sees the mounted slots as drives after the PC's own.

## The FujiNet menu

- **FujiNet Web UI** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11517/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).
- **Restart FujiNet** is like unplugging the adapter and plugging it back
  in.

FujiNet restarts by itself when it reboots (CONFIG's reset, or the web
UI's). If it restarts three times within half a minute it is left stopped:
*FujiNet keeps restarting; it is stopped (FujiNet > Restart)*.

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder (up to the last
megabyte).

## Turning it off

Preferences ▸ FujiNet ▸ **FujiNet** turns the FujiNet off; the status line
says *FujiNet off*. `FUJINET.SYS` on the boot floppy then waits for it
forever, so boot a floppy of your own.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet
and connects the PC's COM port to another FujiNet instance at the address
you give, for example `192.168.1.20:11516`. Set that instance's `[BOIP]`
section to listen there (`enabled=1`, `host`, `port`). The PC connects out,
so the other FujiNet can start before or after it; the status line says
*External FujiNet waiting* until it does, then *External FujiNet on COM2*.
FujiNet Web UI, Console Log and Restart FujiNet belong to the built-in
FujiNet, so they say the FujiNet is external instead of opening; use the
other instance's own. The Debugger's FujiNet packets still show the traffic.

## Behind the scenes

- The link runs at the speed set in Preferences ▸ Ports, counted in the
  PC's time: 115200 bps moves about 10 KB a second.
- FujiNet and the PC's COM port talk over TCP port 11516 on 127.0.0.1;
  nothing from outside can reach either port.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you; only its `[BOIP]` section
  is set by the app on every start.
- A serial mouse goes on the first COM port FujiNet is not using.

# Preferences

Preferences (<kbd>Ctrl+Shift+P</kbd>) set up the PC. The machine's settings
apply together when Preferences closes: the PC resets once. The picture
settings and the volume apply at once, and FujiNet on or off restarts
FujiNet.

Everything but the machine, the picture, the volume and FujiNet on or off
belongs to the machine chosen: each machine keeps its own (see
[Machines](#machines)).

A choice shown as *(not available)* needs a ROM that isn't imported (see
[BIOS and ROMs](#bios-and-roms)), or isn't on this machine. Settings a
machine doesn't have are greyed.

<!-- only: gnome -->

On GNOME, the search button in Preferences finds a setting by name.

<!-- end -->

<!-- only: windows -->

On Windows, <kbd>Ctrl+Tab</kbd> and <kbd>Ctrl+Shift+Tab</kbd> switch pages
and <kbd>Esc</kbd> closes Preferences.

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

**Machine**

| Setting | What it does |
|---|---|
| Machine | IBM 5150 PC, IBM 5160 PC/XT, IBM 4860 PCjr, Typical 286, Typical 386, Typical 486 or Typical Pentium. Each machine keeps its own processor, cards, ports and disks. |
| Processor | The maker, processor and speed: Intel 8088 (the 5150, 5160 and PCjr), Intel 80286, Intel i386DX or AMD Am386DX, Intel i486SX, Intel i486DX, Intel i486DX2 or AMD Am486DX4, Intel Pentium (P54C). Only the speeds the board takes are offered. |
| Memory (KB) | How much memory, within what the machine takes. |
| Math coprocessor | Adds an 8087, 287 or 387. A 486DX or Pentium has its own. |

**Speed**

| Setting | What it does |
|---|---|
| Recompiler | Runs 386 and later processors much faster. Off, every instruction is interpreted, as the Debugger does. |

<!-- context: prefs/Display -->
## Display settings

**Video card**

| Setting | What it does |
|---|---|
| Video card | MDA (monochrome), CGA, EGA, VGA or SVGA (Trident TVGA 8900). The PCjr has PCjr built-in. |

**Picture**

| Setting | What it does |
|---|---|
| 4:3 picture | Shape the picture like a PC monitor's. |
| Whole-number scaling | Only 1×, 2×, 3× … the PC's size, with a border. |
| Smooth scaling | Blends pixels when the picture is scaled. |

<!-- context: prefs/Sound -->
## Sound settings

**Sound card**

| Setting | What it does |
|---|---|
| Sound card | None (PC speaker only), AdLib, Sound Blaster 1.0, Sound Blaster 2.0, Sound Blaster Pro 2, Sound Blaster 16, Sound Blaster AWE32 or Gravis UltraSound. The PCjr has PCjr built-in (SN76489). The PC speaker is always there. |
| MIDI synthesizer | None, Roland MT-32 or Roland CM-32L, on an MPU-401 (the Sound Blaster 16's own, or a separate one). |

**Output**

| Setting | What it does |
|---|---|
| Volume | 0 to 100, FujiNet's speech included. |

<!-- context: prefs/Storage -->
## Storage settings

**Floppy drives**

| Setting | What it does |
|---|---|
| Drive A: | 5.25" 360K, 5.25" 1.2M, 3.5" 720K, 3.5" 1.44M or 3.5" 2.88M. |
| Drive B: | The same, or None. |

The 5150's and 5160's floppy controller is double density only: 360K and
720K. The PCjr's drive is fixed.

**Hard disks**

| Setting | What it does |
|---|---|
| Controller | On the 5150 and 5160: None, IBM Fixed Disk Adapter (Xebec), Western Digital WD1002A-WX1, DTC 5150X, Seagate ST-11M or XT-IDE. The 286 and later have Built-in IDE; the PCjr has None. |
| Drive C: type | Used for a new blank disk; an image keeps its own. |
| Drive D: type | The same, for D:. |

The types are From the image (20 MB, or 504 MB on IDE, for a blank disk),
and on the XT's MFM controllers 10 MB (Seagate ST-412), 20 MB (Seagate
ST-225), 30 MB (Seagate ST-4038) and 40 MB (Seagate ST-251); on XT-IDE and
IDE 40 MB, 120 MB, 250 MB, 504 MB and 2 GB.

<!-- context: prefs/Ports -->
## Ports settings

**FujiNet**

| Setting | What it does |
|---|---|
| FujiNet port | COM1 (3F8h, IRQ 4), COM2 (2F8h, IRQ 3), COM3 (3E8h, IRQ 4) or COM4 (2E8h, IRQ 3); on the PCjr, its Built-in serial port (2F8h). FUJINET.SYS on the boot floppy follows it. |
| FujiNet speed | 9600, 19200, 38400, 57600 or 115200 bits a second. |

**Mouse and joystick**

| Setting | What it does |
|---|---|
| Mouse | None, Microsoft serial mouse, PS/2 mouse (on boards with a PS/2 port) or Microsoft InPort bus mouse. A serial mouse goes on the first COM port FujiNet is not using. |
| Game port | None, Standard (2 buttons) or Standard (4 buttons): two joysticks, worked from the Controllers panel or gamepads. |

<!-- context: prefs/FujiNet -->
## FujiNet settings

**FujiNet**

| Setting | What it does |
|---|---|
| FujiNet | The FujiNet runtime inside the app. Off, there is no FujiNet; see [FujiNet](#turning-it-off). Greyed while an external FujiNet is in use. |

**External FujiNet**

| Setting | What it does |
|---|---|
| Use external FujiNet | Connect the PC's COM port to another FujiNet instead of the built-in one; see [An external FujiNet](#an-external-fujinet). Takes effect with a reset. |
| Address:port | Where that FujiNet listens for FujiBus: a host name or IPv4 address and a port, or just a port for this computer. Empty means `127.0.0.1:11516`. Not saved while it is not an address:port. |

<!-- context: controllers -->
# Controllers

The Controllers panel (<kbd>Ctrl+Shift+K</kbd>) has what a PC user would
have on the desk. Keys typed while it is in front go to the PC.

- **The keyboard**: the machine's own, the 83-key one on the 5150, 5160 and
  PCjr, and the 101-key one on the 286 and later. A key is held while you
  hold the mouse button on it. Under it, the Num Lock, Caps Lock and
  Scroll Lock lights.
- **Joystick A and Joystick B**: drag in the square to move the stick (it
  springs back to the middle) and click buttons 1 to 4. They need a game
  port (Preferences ▸ Ports).
- **Gamepads**: each one plugged in, and which joystick it drives. A new
  gamepad drives Joystick A, the next one B; choose Joystick A or Joystick
  B to fix it, or Automatic. Gamepads can be plugged in and out at any
  time, and one plugged back in gets its joystick back.

On a gamepad, the left stick or the D-pad is the stick, and the South,
East, West and North buttons are buttons 1 to 4; the left and right
triggers are buttons 3 and 4 too. Other joysticks use their first stick or
hat and their first four buttons.

# Debugger

The Debugger (<kbd>Ctrl+Shift+D</kbd>) stops the PC and shows what it is
doing. While it is open the PC runs on 86Box's interpreter, which is slower
than the recompiler on a 386 or later; closing it lets the PC go on as it
was.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a CALL, INT, LOOP or REP, to the instruction after it |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns (RET, RETF or IRET) |
| Run to Cursor | On to the line selected in the Disassembly |

Beside them, the Debugger says why the PC stopped: a step, a breakpoint
and its address, port or interrupt.

## Symbols

**Symbols** loads the labels of a `.map` file from MS LINK or Watcom WLINK.
Give the segment the program was loaded at first: a `.COM`'s PSP, or an
`.EXE`'s PSP + 10h. The Disassembly then shows the labels, addresses show
as a label and an offset, and a label can be typed wherever an address
goes. **Clear Symbols** forgets them.

<!-- only: windows -->

On Windows, **Load segment**, **Load Symbols…** and **Clear Symbols** are
on the Debugger's toolbar.

<!-- end -->

Addresses are written as a segment and offset (`1234:0010`), a register
pair (`cs:ip`, `ds:si`), a linear address in hex (`B8000`), or a label
(`_main`).

<!-- context: debugger/console -->
## Console

The Console takes DEBUG.COM's commands, and a few more for breakpoints.
<kbd>Up</kbd> and <kbd>Down</kbd> bring back earlier commands; `h` lists
them all.

| Command | Does |
|---|---|
| `r [reg value]` | Shows the registers, or sets one |
| `d [addr] [l len]` | Dumps memory |
| `e addr bytes...` | Enters bytes |
| `u [addr] [l count]` | Unassembles |
| `g [addr]` | Goes (to addr) |
| `t [n]` | Traces n instructions |
| `p` | Proceeds over a call, INT or loop |
| `bp addr` | A breakpoint on code |
| `bpr/bpw/bpa addr [len]` | On reading, writing or either, of memory |
| `bpi/bpo/bpio port [last]` | On IN, OUT or either, of I/O ports |
| `bpint n [ah]` | On INT n, with that AH only if given |
| `bl` | Lists the breakpoints |
| `bc n` | Clears breakpoint n; `bc *` clears them all |
| `i port` | Reads an I/O port |
| `o port byte` | Writes one |
| `h` | Lists the commands (or `?` or `help`) |

<!-- context: debugger/cpu -->
## CPU and memory

- **Registers**: type a new value in hex and press <kbd>Enter</kbd> to set
  it; the flags are check boxes. The segment registers show their bases,
  and the mode (real, protected or V86).
- **Memory**: a dump at a linear address. **Go to** takes any address,
  and the page buttons move 4 KB back or on. **Write** puts a byte at an
  address. Video memory at A0000 to BFFFF reads as the CPU would see it.

<!-- context: debugger/disassembly -->
## Disassembly

The code at CS:IP, which **Follow CS:IP** keeps in view; **Jump to** shows
the code at any address. Click a line to select it (for Run to Cursor);
click its left edge, or double-click it, for a breakpoint (`*` on, `o` off).
Scroll, or use <kbd>PgUp</kbd> and <kbd>PgDn</kbd>, to browse.

<!-- context: debugger/breakpoints -->
## Breakpoints

Breakpoints stop the PC on:

- **Execute**: an instruction at an address, before it runs.
- **Memory read**, **Memory write** or **Memory read/write**: a range of
  memory.
- **Port IN**, **Port OUT** or **Port IN/OUT**: a range of I/O ports.
- **INT**: a software interrupt, and an AH if given.

All but Execute stop after the instruction that touched them. Add one with
its type, its first and last address or port, and AH for an INT. Each one
in the list can be turned off and on, shows how often it was hit, and can
be removed; **Clear All** removes them all.

<!-- context: debugger/interrupts -->
## Interrupts

- **INT calls**: with **Trace** on, every software interrupt the PC runs
  (the last 512), with its registers and who called it. DOS's INT 21h
  functions are named, and so are the BIOS's video, disk, serial, keyboard,
  printer and clock calls, the multiplex and mouse calls, and FujiNet's INT
  F5h. Tracing costs speed. **Clear** empties it.
- **Vector table**: where every interrupt goes.

<!-- context: debugger/fujinet -->
## FujiNet packets

- **FujiBus packets**: the last 512 messages between `FUJINET.SYS` and
  FujiNet, each with its direction, device, command, length and first
  bytes. **Clear** empties it.
- **FujiNet's UART**: the COM port FujiNet is on, its speed, FIFO and
  registers, and whether FujiNet is connected.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-msdos/settings.ini` and the
data is in `~/.local/share/fujinet-go-msdos` (or under `$XDG_CONFIG_HOME`
and `$XDG_DATA_HOME`). The GNOME and KDE apps share them. As a Flatpak, they
are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-msdos\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-msdos`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-msdos/settings.ini` and the
data is in `~/.local/share/fujinet-go-msdos`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported BIOS and ROM images |
| `disks` | the FujiNet boot floppies |
| `media` | new blank disks, unless you save them elsewhere |
| `86box` | 86Box's configuration and each machine's CMOS settings |
| `fujinet` | `fnconfig.ini`, the SD card (`SD`), and `fujinet-console.log` |

# Troubleshooting

- **No PC BIOS yet: use Machine > Import BIOS**: no machine has its BIOS;
  see [BIOS and ROMs](#bios-and-roms).
- **The machine has no BIOS imported; starting another**: the chosen
  machine needs its BIOS; the app started one that has it.
- **86Box has already run in this process; restart the app**: 86Box starts
  once per run of the app. Quit and start it again.
- **86Box stopped the last session**: 86Box hit an error it could not go on
  from and closed the app; the message says what it was.
- **The screen stays blank for a while**: the 5150 and 5160 count their
  memory and seek their drives slowly, and CONFIG takes about half a
  minute to come up. **Warp Speed** helps.
- **FUJINET.SYS stops after Port: 2F8h**: it is waiting for a FujiNet that
  is off or stopped. Turn FujiNet on, or choose **Restart FujiNet**.
- **FujiNet keeps restarting; it is stopped**: it rebooted three times in
  half a minute. **Restart FujiNet** starts it again; the console log says
  what went wrong.
- **Keys don't reach the PC**: a dialog or another window is in front, or
  the key is one the app keeps (see
  [Keyboard and mouse](#keyboard-and-mouse)).
- **The mouse does nothing**: no mouse is fitted; choose one in
  Preferences ▸ Ports.
- **A card or drive is not available**: import its ROM, or pick a machine
  that takes it.
- **A new hard disk doesn't show up**: it takes effect with Reset
  (Ctrl+Alt+Del is not enough), and a blank one needs FDISK and FORMAT.

# About and licences

FujiNet Go MS-DOS is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **86Box** (GPL-2.0-or-later, the 86Box team), the
  [FujiNetWIFI fork](https://github.com/FujiNetWIFI/86Box), is the
  emulator.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and
  **FUJINET.SYS** and FujiNet's DOS programs (GPL-3.0) are on the boot
  floppy.
- **FreeDOS** (GPL-2.0): the kernel and FreeCOM on the boot floppy.
- **Zydis** (MIT) disassembles for the Debugger.
- No BIOS or other copyrighted ROM is part of the app; you import your own.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-msdos-desktop/issues)
