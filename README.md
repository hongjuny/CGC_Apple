# CGC Apple

> 1980년대 한국 애플 II 클론의 발상지, 청계천.
> 애플 키드라면 한 번쯤 순례하던 그 성지를, 이제 당신의 Mac 위에.

In 1980s Korea, the road to the Apple II ran through **Cheonggyecheon**.
Seoul's electronics market was the birthplace of Korean Apple II clones and
a pilgrimage for a generation of Apple kids. **CGC Apple** brings that spirit
to the Mac: an Apple II emulator written from scratch, with a portable C core
and a thin Swift/AppKit front end. **CGC** stands for **C**heon**g**gye**c**heon.

## Features

- **Machines**: Apple ][+, Apple //e, and enhanced Apple //e.
- **CPU**: NMOS 6502 and 65C02. Both pass Klaus Dormann's functional test suites.
- **Memory**: 48K plus a 16K language card on the ][+, and 128K with auxiliary memory and the full MMU and IOU soft switches on the //e.
- **AE memory expansion**: optional RamWorks III (IIe AUX slot) and RamFactor (slot 1), each with 8 MB, following MAME's capacity model. Both can be installed together on a IIe.
- **Video**:
  - 40- and 80-column text with the //e character set (lowercase, alternate set, MouseText).
  - Lo-res, hi-res, double lo-res and double hi-res.
  - NTSC artifact colour from a 4-dot phase window.
  - Colour, green, amber and white monitors, plus optional scanlines.
- **Disk II** (slot 6): emulated at the bit level.
  - Reads and writes `.dsk`/`.do`, `.po`, `.nib`, WOZ 1/2 and 140K 2IMG (`.2mg`) images. A 2IMG file keeps its header and chunks when saved.
  - Sector images are converted to GCR bit streams on insert and decoded back on save.
  - Optional "fast disk" mode runs the whole machine at full speed while a drive spins. Emulated timing is unchanged, so loaders and copy protection still work.
- **Sound**:
  - Speaker.
  - Mockingboard in slot 4 (two 6522 VIAs and two AY-3-8913 PSGs).
  - Band-limited step synthesis keeps the square waves clean, and a jitter buffer with rate control prevents dropouts.
- **Mouse** (slot 2): a card compatible with the AppleMouse II firmware interface (its own firmware, so no ROM is needed), with VBL, movement and button interrupts. The Mac pointer over the window is the Apple II mouse.
- **FujiNet** (slot 5): a SmartPort card that relays to [FujiNet-PC](https://github.com/FujiNetWIFI/fujinet-firmware) over TCP ("SP over SLIP"), for FujiNet's network disks, network device, printer and clock (see below).
- **Hard disk** (slot 7): a ProDOS block device with built-in firmware, so no ROM is needed. It mounts headerless ProDOS block images up to 32 MB (`.hdv`, `.2meg`, large `.po`) and ProDOS-order 2IMG images (`.2mg`, `.2img`) (see below).
- **Joystick**: from the arrow keys, the mouse, or a game controller (GameController framework). The Option keys work as buttons 0 and 1 (Open/Closed Apple on the //e), and while a joystick mode is on the `` ` `` key (left of 1) is also button 1, so both buttons are under the left hand.
- **Hangul**: runs CALL3327, the 1986 Korean Hangul I/O program. Its disk is inserted by default when it is present (see below).

## Building

Requirements: macOS 13 or later, and Xcode or the Swift toolchain.

```sh
swift build -c release            # builds .build/release/CGCApple
scripts/build_app.sh              # builds build/CGCApple.app
scripts/build_app.sh --with-roms  # also copies your local roms/ (and disks/call3327.dsk) into the app
```

`--with-roms` is only for personal use. Never distribute a bundle built with it.

## ROMs

ROM images are copyrighted by Apple and are **not** included in this repository.
Provide your own dumps in a `roms/` folder next to the app or the project, in
`~/Library/Application Support/CGCApple/roms`, or in a folder named by
`CGCAPPLE_ROMS`. If none of these has the ROMs, the app asks for a folder on
first launch.

AppleWin's file names and MAME's split ROM names are both accepted.

| Machine | File (AppleWin name) | MAME parts |
|---|---|---|
| ][+ system, 12 KB | `Apple2_Plus.rom` | `341-0011.d0` … `341-0015.f0`, `341-0020-00.f8` |
| ][+ character, 2 KB | `Apple2_Video.rom` | `341-0036.chr` |
| //e system, 16 KB | `Apple2e.rom` | `342-0135-b.64`, `342-0134-a.64` |
| Enhanced //e system, 16 KB | `Apple2e_Enhanced.rom` | `342-0304-a.e10`, `342-0303-a.e8` |
| //e video, 4 KB | `Apple2e_Enhanced_Video.rom` | `342-0265-a.chr` |
| Disk II boot PROM, 256 B | `DISK2.rom` | `341-0027-a.p5` |

MAME's ROM definitions (`apple2p`, `apple2e`, `apple2ee`, `diskiing`) list the
SHA-1 of each part. Use them to check your dumps.

## Memory cards

The **Memory** menu enables or disables either card and remembers your settings.
Changing a card saves disk images and power cycles the machine, clearing working
RAM. Both cards are off by default; slot 7 is reserved for a future hard disk controller.

- **RamWorks III**: IIe and enhanced IIe only. Expands auxiliary memory to 8 MB
  in 128 banks of 64 KB (plus the machine's 64 KB main RAM). Bank 0 supplies video;
  CPU bank selection applies to RAMRD/RAMWRT, ALTZP, language-card RAM and 80STORE.
  No extra ROM is needed. Its setting is retained while using a II+ and takes
  effect again when switching back to a IIe.
- **RamFactor**: II+ and both IIe models, in **slot 1**. Provides 8 MB accessed
  through address/data ports, plus firmware for RAM-disk use. Supply your own
  **8,192-byte** ROM revision as `RamFactor.rom` in the same folder as the system
  ROMs. MAME names `ae ramfactor rom v1.4.bin`, `v1.3.bin`, `v1.1.bin` and `v1.0.bin`
  (each with the full `ae ramfactor rom ` prefix) are also accepted. A combined
  32 KB MAME ROM region is not accepted. If the ROM is missing or invalid,
  enabling the card reports an error and leaves it off.

These cards require software that supports their memory interfaces; they do not
increase Applesoft BASIC's ordinary program space. **Ctrl-RESET preserves card
RAM**, but power cycling, switching models, removing cards or quitting discards it.
RamFactor battery backup and persistence are not emulated.

## Disks

Disk images are not included either. When the app is launched without a disk
argument, it looks for **`call3327.dsk`**, the CALL3327 Hangul disk from
[iolo/call3327](https://github.com/iolo/call3327). It searches these places:

- `CGCAPPLE_DISKS`
- the app bundle
- a `disks/` folder next to the ROM folder
- `./disks`

The first copy found is copied once to `~/Library/Application Support/CGCApple/disks/` and used from there, so the original file is never modified.

Other images can be inserted from the Disk menu (⌘O / ⇧⌘O) or by dropping
them on the window; the right half of the window is drive 2.

**Note:** when a program writes to a disk, the image file you opened is updated
when the disk is ejected or the app quits. Keep backups of images you care
about, especially WOZ images. If saving fails, the app reports the error and
keeps the disk inserted; ejecting, replacing the disk, switching models and
quitting are stopped until saving succeeds.

### Typing Hangul with CALL3327 (3327 한글)

CALL3327, or "3327 한글", was one of the first Hangul input/output programs for
the Apple II. It draws Hangul on the hi-res screen in software, and its name
comes from its entry point, `CALL 3327` ($0CFF).

1. Boot the disk and type `BRUN CALL3327`. The screen switches to the hi-res Hangul display.
2. **Ctrl-K** switches to Hangul input (한글 모드). Type with the 2-set layout (두벌식); for example, `gksrmf` gives 한글 and `dkssud` gives 안녕.
3. **Ctrl-A** or **Return** switches back to English (영문 모드). The original README says Ctrl-E, but the source uses Ctrl-A.
4. On the //e, keep **Caps Lock** on (Model menu, ⇧⌘K). The program expects uppercase key codes and drops into the monitor on lowercase ones.
5. Ctrl-RESET returns to the normal text screen, and `CALL 3327` re-enters Hangul mode.

The macOS input source does not matter (한글 입력기를 켜 두어도 됩니다). Keys are taken by their US-layout position.

### Hard disk (slot 7)

Mount a ProDOS hard disk image from the Disk menu (⌥⌘O), or drop it on the
window. Where an image goes is decided by its content, not its name:

- A 2IMG file (`.2mg`, `.2img`) holding a 140K floppy goes into a floppy drive; a bigger one goes to the hard disk.
- `.hdv` and `.2meg` files are hard disk images.
- Any other headerless image larger than a 140K floppy (a multiple of 512 bytes, such as an 800K or 32 MB `.po`) is mounted as a hard disk.
- `.dsk`, `.do`, `.nib` and `.woz` files always go into a floppy drive.

The machine restarts when a hard disk is mounted or unmounted, and the mounted
image is remembered across launches.

Slot 7 is scanned first at power-on, so the machine boots from the hard disk
even with a floppy in drive 1. To boot the floppy instead, type `PR#6` or
unmount the hard disk (⌥⌘E). Writes are saved to the image file, like floppy
writes.

This card is not a copy of any real hardware. It is an emulator-specific card
like AppleWin's hard disk: its firmware passes each ProDOS block request to the
emulator through one I/O register.

### Mouse (slot 2)

Turn on **Model → AppleMouse (Slot 2)**. Mouse programs find the card by its
ID bytes and call the standard routines (SetMouse, ReadMouse, ServeMouse,
ClampMouse, HomeMouse and so on), so it works in any slot-scanning program.
The Mac pointer is mapped onto the area the program clamps the mouse to, so
the Apple II cursor follows the pointer without capturing it. The left button
is the mouse button. While the card is in, the Mac mouse no longer drives the
joystick's mouse mode.

### FujiNet (slot 5)

[FujiNet](https://fujinet.online) on the Apple II is a SmartPort device. Here,
the slot 5 SmartPort card relays each SmartPort call to FujiNet-PC, the desktop
build of the FujiNet firmware, using its "SP over SLIP" protocol.

1. Turn on **Model → FujiNet**. The card listens on `127.0.0.1:1985`, FujiNet's default port for this protocol.
2. Build and start FujiNet-PC for the Apple II (`./build.sh -p APPLE` in [fujinet-firmware](https://github.com/FujiNetWIFI/fujinet-firmware), then `./run-fujinet`). It connects to the emulator, and the window subtitle shows "FujiNet: n devices".
3. Boot it with `PR#5`. The Disk II in slot 6 is scanned before slot 5, so a floppy in drive 1 boots first. With a hard disk in slot 7, the hard disk boots first, and ProDOS still sees the FujiNet devices.

The card has its own firmware with the standard ProDOS and SmartPort entry
points, so no ROM is needed. The emulator waits for each reply, as a real
SmartPort call waits for the bus (up to 30 seconds for long network
transfers).

## Keys

| Key | Action |
|---|---|
| ⌘R | Ctrl-RESET |
| ⇧⌘R | Power cycle |
| ⌘P | Pause |
| ⇧⌘F | Fast disk on/off |
| ⌥⌘O / ⌥⌘E | Mount / unmount hard disk |
| ⇧⌘K | Caps Lock (//e) |
| ⌘1 … ⌘4 | Colour / green / amber / white monitor |
| ⌘L | Scanlines |
| ⌘V | Type the clipboard |
| Left / right Option | Joystick buttons 0 / 1 (Open / Closed Apple) |
| `` ` `` (left of 1) | Joystick button 1 while a joystick mode is on |

## Tests

`swift test` checks disk save failures, model-switch rollback, memory-card configuration and optional ROM loading in the Swift wrapper.

The headless tests in `tests/` build against the C core with clang:

- `cputest.c`: Klaus Dormann's 6502 and 65C02 functional tests. Download the binaries from [Klaus2m5/6502_65C02_functional_tests](https://github.com/Klaus2m5/6502_65C02_functional_tests); they are not included.
- `boottest.c`: boots ROMs and disks, types keys, dumps the text screen and screenshots. `RAMWORKS=1` enables RamWorks (with `A2MODEL=iie` or `iiee`); `RAMFACTOR=1` enables RamFactor using `RamFactor.rom`.
- `memorytest.c`: RamWorks bank isolation, MMU and fixed video bank; RamFactor full-capacity access, pointer carry/wrap, slot ROM banking/arbitration, reset and slot conflicts. Uses synthetic firmware; no external ROMs required.
- `disktest.c`: round trips between `.dsk`, `.po`, `.nib` and WOZ 2, plus malformed-image rejection and preservation of inserted disks.
- `mbtest.c`: Mockingboard tone frequency, channel separation and VIA timer IRQ rate.
- `mousetest.c`: the mouse card from 6502 code on the II+ and enhanced IIe: ID bytes, InitMouse, ClampMouse, SetMouse, ReadMouse position and button, HomeMouse and 60 Hz VBL interrupts served by ServeMouse.
- `sptest.c`: SmartPort against a fake FujiNet-PC on TCP: slot 5 boot, ProDOS and SmartPort calls (STATUS, READBLOCK, WRITEBLOCK, READ, WRITE, CONTROL), SLIP escaping, INIT numbering, stale replies and the reset CONTROL.
- `hdtest.c`: boots a hard disk image from slot 7 on all three machines, and checks ProDOS STATUS, READ and WRITE, the error codes, 2IMG and write protection.

```sh
clang -O2 -ISources/A2Core/include -ISources/A2Core -o /tmp/mbtest tests/mbtest.c Sources/A2Core/*.c
/tmp/mbtest roms
clang -O2 -ISources/A2Core/include -ISources/A2Core -o /tmp/memorytest tests/memorytest.c Sources/A2Core/*.c
/tmp/memorytest
```

## Sources and acknowledgements

This project does **not** contain code copied from MAME or from any other
emulator. The core was written independently. MAME's source code was studied as
a reference for how the hardware behaves: memory maps, soft switches, card
wiring and chip register semantics. Where a file follows a behaviour first
understood from MAME, its header comment names the MAME source file.

- **[MAME](https://github.com/mamedev/mame)**: the files used as references are all BSD-3-Clause, by R. Belmont and other MAME contributors.
  - `src/mame/apple/apple2.cpp` and `apple2e.cpp`: memory map and soft switches.
  - `apple2video.cpp`: text rendering rules and the NTSC artifact colour phase.
  - `src/devices/bus/a2bus/a2diskiing.cpp` and `machine/wozfdc.cpp`: Disk II.
  - `a2mockingboard.cpp`, `sound/ay8910.cpp` and `machine/6522via.cpp`: Mockingboard.
  - `a2eramworks3.cpp` and `a2memexp.cpp`: RamWorks III and RamFactor memory banking and register behaviour.
- **[Klaus Dormann's 6502/65C02 functional tests](https://github.com/Klaus2m5/6502_65C02_functional_tests)** (GPL-3.0): used to verify the CPU core. They are not redistributed here.
- **[WOZ disk image reference](https://applesaucefdc.com/woz/reference2/)** by John K. Morris (Applesauce): the WOZ 1/2 file format, including the "fake bit" behaviour on long runs of zero bits.
- **Blip_Buffer** by Shay Green (blargg): the idea behind the band-limited step synthesis in `audio.c`.
- **[AppleWin](https://github.com/AppleWin/AppleWin)**:
  - ROM file naming conventions, which this app accepts.
  - Running at full speed while the disk is active.
- **[FujiNet](https://github.com/FujiNetWIFI/fujinet-firmware)** (GPL-3.0): `lib/devrelay` and `lib/bus/iwm` were read as the reference for the "SP over SLIP" packet layout, the default port and SmartPort INIT numbering. No FujiNet code is included.
- **[iolo/call3327](https://github.com/iolo/call3327)**: a restoration of CALL3327, the early Korean Hangul input/output program for the Apple II. It was published as source in the June 1986 issue of the Korean magazine *Micro Software*.

Apple, Apple II, Mockingboard, Phasor and other names are trademarks
of their respective owners.
