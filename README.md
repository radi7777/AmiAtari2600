# AmiAtari2600

Atari 2600 emulator for classic Amigas (ECS chipset). It runs without SDL and without OS
graphics and drives the Copper, bitplanes, Paula and BEAMCON0 directly. A MUI launcher
with a game list, game information and screenshots comes with it.
Background and the original goals are in [docs/PROJECT.md](docs/PROJECT.md), the design
in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Status

| Part | Status |
|---|---|
| 6507 CPU in C (incl. illegal opcodes) | done, passes Klaus Dormann's 6502 functional test |
| 6507 CPU in 68k assembler (`src/amiga/cpu6507.s`) | 135 opcodes in assembler, the rest through the C core, PC as a direct ROM pointer; passes the Dormann test on 68k (Amiberry), renders identically to the C core |
| TIA video (playfield, players, missiles, ball, collisions, HMOVE, VDEL) | done, colour-clock exact catch-up rendering; HMOVE cycle exact (all cycles × all HM values measured against gopher2600), incl. the Cosmic Ark stars; 15 of 21 tested games pixel-identical to gopher2600 |
| TIA audio | done (circuit model, 1 sample per scanline) |
| RIOT (RAM, timer, ports) | done, timer cycle exact like gopher2600 |
| Bank switching | 2K, 4K, F8, F6, F4, F8SC, F6SC, F4SC, FA, E0, E7, 3F, FE, with auto-detection |
| PAL/NTSC detection | done (lines per frame, with hysteresis) |
| Amiga: video (2 colour banks, Copper, C2P, double buffer, only changed longwords go to chip RAM) | runs on real hardware (PiStorm/Emu68, 68030, KS 3.2) and in Amiberry |
| Amiga: input (late joystick read, frame delay) | runs on real hardware |
| Amiga: system-friendly mode (OS interrupts keep running) / `KILLOS` | runs on real hardware |
| Amiga: BEAMCON0 switching, Paula | runs; switching PAL/NTSC in the middle of a game not yet checked on real hardware |
| Amiga: MUI launcher `AmiAtari2600` | runs on real hardware (RTG and chipset screens) and in Amiberry |
| Amiga: vbcc build | done (`make -f Makefile.amiga`) |

The core is platform-neutral C and is tested on the host and as a 68k binary in Amiberry
(`tests/amiga_tests.sh`), with the C and with the assembler CPU. Accuracy is checked
against gopher2600 with `tools/refcheck.sh`.

Performance numbers and plans: see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#performance).

## Building

### Host (macOS/Linux): core + test harness

```sh
make            # build/a26host
make test       # CPU test, test ROMs, TIA equivalence, video conversion
./build/a26host roms/game.bin -frames 300 -ppm shot.ppm -wav sound.wav
```

`a26host` runs a ROM without a display and prints its type, region, line count and
visible area. It can write the last picture as PPM (320 pixels wide, as on the Amiga)
and the sound as WAV. More options: `-type F8`, `-region pal`, `-reset N` (press Game
Reset from frame N on), `-fire N`, `-bench`, `-clean` (empty RAM, INTIM = 0, like
gopher2600), `-raw prefix -rawfrom N` (frames as TIA colour indices).
`make build/a26trace` builds a variant that, run with `-trace N`, prints every TIA write
of frame N with line, cycle and pixel.

**68k tests in Amiberry** (no qemu needed): `make -f Makefile.amiga tests` builds the test
harness and the CPU test as Amiga programs (C and asm core); `sh tests/amiga_tests.sh [roms …]`
starts Amiberry with the test drawer as boot drive, waits for the results and requires
identical pictures and sound from the C core, the asm core and the host build.

**Accuracy against gopher2600:** `REFDUMP=/path/to/refdump sh tools/refcheck.sh 300 10 roms/*.bin`
compares frames 300–309 pixel by pixel (refdump is a small Go program based on
gopher2600 that writes frames as TIA colour indices). `tools/refcompare.py … --detail`
shows the lines that differ.

With an m68k cross GCC, `qemu-m68k` and `vasmm68k_mot` (Linux, e.g. in a Docker container
on the Mac) there is also:

```sh
make m68k-test                         # all tests as a 68030 binary, C and asm CPU
make m68k-profile PROFILE_ROM=roms/x.bin   # 68k instructions per frame, by function
```

ROMs go into the `roms/` drawer. It is ignored by git, and `make test` additionally runs
every `roms/*.bin` for 300 frames.

### Amiga (vbcc)

Requirements: vbcc with the `m68k-amigaos` target (`$VBCC` set, `vc` and
`vasmm68k_mot` in the PATH) and the AmigaOS NDK 3.1 or 3.2.

```sh
make -f Makefile.amiga NDK_INC=/path/to/NDK3.2/Include_H
# -> build/amiga/A26 (emulator, with the assembler CPU; ASM_CPU=0 for the C CPU)
#    build/amiga/AmiAtari2600 + AmiAtari2600.info (launcher with icon)
```

If the vbcc installation has no suitable config (e.g. the one from the m68k-amigaos-gcc
toolchain, whose `aos68k` points to other paths), pass an adjusted copy with
`CONFIG=+/path/to/aos68k`.

`make -f Makefile.amiga dist` builds the release archive `build/aminet/AmiAtari2600.lha`
and its Aminet readme (from `res/AmiAtari2600.readme`): one drawer with drawer icon, both
programs, `res/ReadMe`, the licence and the committed source; needs `lha`. The file
names carry no version, so a new Aminet upload replaces the old one.

VS Code has ready-made tasks (`Terminal > Run Task`): host build, tests and Amiga build.
The Amiga build needs the environment variable `NDK_INC`.

## Using it on the Amiga

`AmiAtari2600` is the launcher (double-click its icon). It runs the emulator `A26` for
each game; `A26` must be in the same drawer. `A26` can also be started from the Shell:

```
A26 <rom> [PAL|NTSC] [COLORS=PAL|NTSC] [TYPE=F8|F6|...] [SKIP=n] [DELAY=n] [PORT1]
    [NOSOUND] [KILLOS] [PROFILE] [BENCH=n] [FRAMES=n]
```

- Region and bank switching are detected automatically. `PAL`/`NTSC` and `TYPE=`
  force them.
- `COLORS=PAL`: PAL palette with NTSC timing (for PAL60 games).
- `SKIP=n`: show only every (n+1)th frame. Without it the emulator decides by itself
  (at most 2 frames in a row).
- `DELAY=n`: start emulating each frame n raster lines after the vertical blank.
  Without it the emulator picks the delay so that the frame is ready just before the
  next vertical blank: this shortens the time between joystick and picture.
  `DELAY=0` turns it off.
- `PORT1`: the joystick in the mouse port controls player 2.
- `PRI=n`: task priority while playing. The default is 19: above network and Wi-Fi
  tasks, which otherwise take 100–150 ms now and then (a visible hiccup), and below
  input.device (20), so the keyboard always works.
- `KILLOS`: switches the OS interrupts off while playing (a little faster on slow
  machines). The default is the system-friendly mode, in which timers, keyboard and
  network keep running.
- `PROFILE`: prints a timing table on exit. Per frame it shows the wait before emulation
  (frame delay), emulation, sound, picture conversion and the vsync wait, in raster
  lines. The budget is 312 (PAL) or 262 (NTSC) lines per frame.
- `FRAMES=n`: quit after n frames (for measurements with `PROFILE`).
- `BENCH=n`: runs n frames as fast as possible, without vsync and without frameskip,
  then prints the table and the frames per second reached.

| Key | Function |
|---|---|
| Joystick port 2 / cursor keys + Space/Alt | Player 1 |
| F1 / F2 | Game Reset / Game Select (hold) |
| F3 | Colour / black and white |
| F4 / F5 | Left / right difficulty |
| F6 | Region: Auto → NTSC → PAL → Auto |
| P | Pause |
| HELP | Hard reset |
| ESC | Quit |

## Requirements

**Emulator (`A26`):**
- For full speed at least a **68060 at 50 MHz** (or comparable, e.g. Emu68/PiStorm).
  Slower CPUs run with frameskip.
- For switching PAL/NTSC at run time an **8372A Fat Agnus (ECS)**. With an OCS Agnus the
  emulator runs in the machine's native mode, without switching.
- No other libraries or tools. ROMs can be loaded straight from `.zip` files; no external
  unzip is needed.

**Launcher (`AmiAtari2600`):**
- **MUI 3.8+**, OS 3.x. Screenshots need **picture.datatype V43** (OS 3.2, 3.5/3.9) and
  a **PNG datatype** (not part of every OS; otherwise from Aminet).
- Optional: **curl** and **AmiSSL** (both on Aminet). With them the launcher downloads the
  game database (libretro-database) and the screenshots (libretro-thumbnails) itself over
  https. Everything else works without curl/AmiSSL. The database and pictures can then be
  copied by hand: the `.dat` files into `db/` (`nointro.dat`, `publisher.dat`,
  `developer.dat`, `releaseyear.dat`, `genre.dat`), the pictures as `snaps/<CRC32>.png`
  (8 upper-case hex digits, e.g. `snaps/1E86DE5A.png`), both in the program's drawer.
- cybergraphics.library is used if present (RTG screen), but it is not required.

## Licence

MIT, see [LICENSE](LICENSE). The code is written from scratch and contains no Stella
code. The TIA audio model follows the same hardware logic that Stella models.

The game database and the screenshots are not part of the project. The launcher
downloads them from libretro on the user's machine. No ROMs are included.

`tools/refdump` is built against gopher2600 (GPL). It is only a developer tool; no
binary of it is distributed.

The Atari logo in the program icon (`res/AmiAtari2600.png`) is a trademark of Atari and
is not covered by the MIT licence.
