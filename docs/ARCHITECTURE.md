# Architecture

```
src/core/      platform-neutral emulation core (C99, also builds with vbcc)
  cpu.c        6507: full NMOS instruction set, 1 bus access = 1 cycle
  bus.c        address decoding (TIA / RIOT RAM / RIOT I/O / cartridge)
  tia.c        TIA: video (catch-up rendering), collisions, audio, inputs
  riot.c       6532: 128 bytes RAM, timer (computed lazily), ports
  cart.c       ROM mapping, bank switching, type detection, read map for the asm core
  cpu_asm.c    C side of the assembler core (callbacks, single steps through cpu.c)
  atari.c      frame loop, input API, PAL/NTSC detection
  palette.c    NTSC/PAL palettes (24 bit)
  unzip.c      ROMs straight from .zip files (own inflate)
src/amiga/     Amiga front end (AmigaOS headers, direct hardware access)
  cpu6507.s    6507 interpreter in 68030 assembler (vasm), falls back to cpu.c
  irq.s        VBL interrupt server (counts frames, wakes the emulator task)
  hw.c         display takeover (system-friendly or KILLOS), Agnus detection, BEAMCON0,
               VBL sync, waiting for raster lines, timing
  vidconv.c    TIA line -> colour bank + Copper moves + bitplanes (portable, host-tested)
  video.c      Copper lists, double buffer, PAL/NTSC display window, diff writes to chip RAM
  audio.c      Paula output
  input.c      joystick (JOYxDAT/CIA), keyboard through an input.device handler or CIA polling
  main_amiga.c command line, main loop, frame delay, frameskip
src/gui/       MUI launcher AmiAtari2600 (game list, libretro database, screenshots)
src/host/      test harness for Linux/macOS (PPM/WAV output)
res/           program icon (PNG, turned into a .info by tools/mkicon.py)
tests/         CPU test, test ROMs (own assembler), video conversion test,
               TIA equivalence test (optimised against reference renderer),
               amiga_tests.sh: the same tests as 68k binaries in Amiberry
tools/         asm6502.py: small 6502 assembler for the test ROMs
               refcompare.py / refcheck.sh: picture comparison with gopher2600
               gen_hmove_rom.py: HMOVE test ROMs (all cycles x all HM values)
               qemu_icount.py: count 68k instructions per frame/function under qemu-m68k
               mkicon.py: PNG -> Amiga icon (classic + OS 3.5 colour image)
```

## Timing model

`a26_cycles` is the master clock (CPU cycles). Every `bus_read`/`bus_write` counts one
cycle. The CPU adds internal cycles itself, always *before* the final write access. This
puts TIA writes (WSYNC, RESPx, HMOVE …) on the right cycle.

The TIA does not run in lockstep with the CPU. Before every TIA access the beam is
caught up to the current colour clock (`a26_cycles * 3`), with the *old* register
values. Only then is the new value taken over. This is colour-clock exact and only costs
time where something is actually drawn. Some registers have small take-over delays (PFx,
NUSIZx, REFPx, VBLANK, GRPx and ENAxx 1 colour clock each), modelled on the hardware and
on Stella/gopher2600.

More hardware details the model covers:

- **RESPx:** the main copy of a player only appears when its counter wraps. After a
  reset in the middle of a line it therefore only shows from the next line on, while the
  other copies (NUSIZ) already appear in the same line.
- **HMOVE:** the effect depends on the CPU cycle of the write. `hmove_disp[HM][cycle]`
  was measured from gopher2600 (`tools/gen_hmove_rom.py`) and follows a simple model (see
  `tia.c`): from 3·cycle+2 on, an extra pulse comes every 4 colour clocks until the
  counter reaches the HM value. Only pulses inside the (extended) HBLANK move the object;
  the HMOVE bar shifts everything 8 pixels to the right. Early in the line this gives the
  familiar movement, in the middle nothing, late in the line it only takes effect in the
  next line (the cycle 73/74 trick without the bar).
- **HMxx writes during the wave** change the target. If the new stop value is already
  behind the counter, the object stays "locked" and keeps getting extra pulses: 17 pixels
  per line to the left, and a width that changes with the position mod 4. That is the
  starfield in Cosmic Ark. The lock ends with the next HMOVE or with HM = $80.

The objects use mask tables: each object keeps a pointer into a 320-entry table, and
`mask[x]` gives the graphics bit for pixel `x` directly. Lines without players, missiles
or ball take a fast path for playfield and background only.

The RIOT timer is not clocked. It is computed on reading from the cycle of the last
write: after writing N in cycle w, a read in cycle r returns N − 1 − (r − w − 1) / interval.
After passing zero it keeps counting from $FF at the clock rate and sets the timer flag;
reading INTIM clears it again. This matches gopher2600 cycle for cycle (being one cycle
off was enough to shift INTIM wait loops, and with them VBLANK, in single frames).

## Accuracy: reference comparison

`tools/refcheck.sh` runs ROMs in our own core and in gopher2600 side by side (through a
small Go program that writes frames as TIA colour indices) and compares the pictures
pixel by pixel, searching for frame and line offsets automatically. With `-clean` the
core starts with empty RAM and INTIM = 0 like gopher2600; otherwise games that take their
randomness from the power-up state would run differently from the start.

Status (frames 300–309 in attract mode, 21 games): 15 pixel-exact, among them Cosmic Ark,
Pitfall, River Raid, Solaris, Ms. Pac-Man, H.E.R.O., Space Invaders, Yars' Revenge,
Missile Command, Kaboom and Combat. In Asteroids, Battlezone, Demon Attack, Enduro,
Jr. Pac-Man and Moon Patrol the game states drift apart after a while (Asteroids e.g.
from frame 257); positions and timing of single lines match. The cause is still open
(power-up state of CPU/TIA or a rare timing case).

## Picture output on the Amiga

The TIA delivers 160 pixels per line, each with one of 128 colour values. But a line
hardly ever needs more than a handful of them. The Amiga therefore runs in lowres with 5
bitplanes, and every TIA pixel becomes two Amiga pixels wide (160 → 320).

The 32 colour registers are split into two banks (`vidconv.c`): even lines use
COLOR00–15, odd lines COLOR16–31. Bitplane 5 selects the bank and is constant per line
(written once per mode change). The Copper loads a line's colours during the previous
line, which shows the other bank. That leaves a whole raster line of Copper time, i.e.
up to 15 colours per line (register 0 of each bank is black, COLOR00 is also the border).
Beyond 16 colours the nearest one is used.

This way a converted line only depends on its own 160 TIA pixels:

1. Each buffer keeps a copy of the TIA lines in fast RAM. Unchanged lines are skipped
   without computing or writing anything.
2. Changed lines are converted (4 planes, pair table + 4×4 byte transpose) and compared
   with fast RAM copies of the bitplane and Copper longwords. Only longwords that really
   changed go to chip RAM. A moving sprite usually changes only one or two longwords per
   line.

Chip RAM accesses are the most expensive thing on accelerated Amigas (PiStorm, turbo
cards); on the test machine (Emu68, 68040) this brought the conversion down from 89 to
9–25 raster lines per frame.

The picture is switched through `COP1LC`: the Copper starts with the new list at the next
vertical blank, so there is no tearing. The game's visible area is centred vertically
(NTSC: 224 lines from raster line $1C, PAL: 256 from $2C). The emulator only takes over
changes once they have been stable for 30 frames.

## Input and latency

Goal: as little time as possible between joystick and picture.

- **Late read:** the joystick is not read at the start of the frame but at the moment
  the game reads SWCHA or INPT4/5 (`a26_input_hook`, at most once per emulated line). The
  keyboard is taken over once per frame.
- **Frame delay:** on fast machines a frame is done in a fraction of the frame time.
  Instead of starting right after the vertical blank and then waiting, emulation starts
  so late that it finishes just before the next vertical blank. The start line follows
  the slowest of the last 64 frames plus 25 % plus 12 lines; after a missed vertical
  blank there is more headroom for a while. `DELAY=n` fixes the start line, `DELAY=0`
  turns it off.
- **Display:** the finished picture is activated through `COP1LC` at the next vertical
  blank. Between the joystick read and the display there is only the rest of the current
  frame.
- **Frameskip** only kicks in when a vertical blank was missed (slow Amigas).

## System takeover

The default is a system-friendly mode: the OS keeps its interrupts; timers, keyboard and
network keep running. The emulator takes over the display with `LoadView(NULL)` and its
own Copper list and runs at task priority 19 (`PRI=n`).

- Vertical blank: an interrupt server of its own (`irq.s`). It counts frames and wakes the
  emulator task with a signal. It is written in assembler because an interrupt server has
  to return with the Z flag set, otherwise Exec skips all servers behind it. The C
  compiler does not guarantee that, and it crashed the test machine.
- Waiting for raster lines: sleeping through `timer.device` (UNIT_MICROHZ); the last
  lines are polled on the beam. Other tasks get CPU time this way.
- Keyboard: an `input.device` handler at priority 100 that swallows RAWKEY and RAWMOUSE
  events so the Workbench behind it gets nothing.
- Timing: VBL counter plus beam position. The OS reads the CIA-B TOD itself, and that
  destroys its latch.

`KILLOS` is the old mode: Forbid, all interrupts and DMA off, keyboard through CIA
polling (this loses the OS's CIA interrupt flags; the network often takes a long time to
recover afterwards).

## PAL/NTSC

- Detection: moving average of the scanlines per frame. From 287 lines on it is PAL,
  below that NTSC. In the first 60 frames 4 matching frames are enough to switch, after
  that 16.
- Switching: `BEAMCON0` bit 5 (ECS Agnus 8372A or AGA only). The Agnus ID comes from
  `VPOSR` (bit 5 of the ID = ECS). Then the emulator sets the display window (256 or 224
  lines), palette and Paula period anew.
- On exit it restores BEAMCON0 from `GfxBase->DisplayFlags`.

## Sound

The TIA is clocked twice per scanline (31.4 kHz). One sample is stored per scanline.
Paula plays one buffer per Amiga frame at about one raster line per sample (period ≈ 227).
Each TIA channel sits on two Paula channels (left + right) so it sounds centred. The
buffers are switched without interrupts: the new `AUDxLC` is written, and Paula takes it
over at the end of the current buffer. The samples are made in fast RAM and go to chip
RAM as longwords.

## Assembler CPU

`src/amiga/cpu6507.s` is a hybrid core. The common opcodes (loads/stores in all
addressing modes, ALU, compares, branches, JSR/RTS, stack, INC/DEC, shifts) run in
assembler. Everything else jumps to `fallback`: the state is handed to `cpu.c`, exactly
one instruction is executed in C, then it continues in assembler. So the core was
complete from the start and can be extended opcode by opcode. Change the entry in the
table at the end of `cpu6507.s` from `fallback` to the new `op_XX`, and the Dormann test
plus `make m68k-test` show at once whether it is right.

Registers: d2 = A, d3 = X, d4 = Y, d5 = cycle counter, d6/d7 = N/Z source, a3 = PC,
a5 = read map, a6 = RAM. C, V, D and I are bytes in the context (`cpu_asm.h`).

The PC is a direct pointer into the ROM: as long as the code runs in a fast page, an
opcode fetch is just `cmp.l PCEND,a3` / `move.b (a3)+,d0`. Only at the end of the page,
after jumps or after a bank switch is the page looked up in the table. Code in slow
pages (hotspot page, RAM) runs in a "slow mode" through the C callbacks. Branches within
the page only add the offset; exactly then the page-crossing penalty cycle is dropped
too. `tests/roms/cpu_paths.asm` checks these cases cycle for cycle.

Memory accesses go through a page table with 256 entries. Cartridge pages point directly
into the ROM. Hotspot pages, TIA and RIOT are slow pages and go through a C callback.
Zero page from $80 and the stack access the RAM directly. The table is only rebuilt after
bank switches.

## Performance

Measured with `make m68k-profile`: 68k instructions executed per emulated frame
(gcc -O2 -m68030, qemu-m68k; without the Amiga picture conversion and chip RAM):

| Test ROM | Start (C CPU) | asm CPU | + new TIA renderer | + PC as pointer | + direct TIA writes |
|---|---|---|---|---|---|
| `busy_ntsc` (CPU-heavy, ~23 6502 instructions/line, 2 TIA writes/line) | 961,000 | 648,000 | 537,000 | 467,000 | **443,000** |
| `bars_ntsc` (player sprite on every line, little CPU) | 1,035,000 | 1,019,000 | 273,000 | 264,000 | **249,000** |

Budget for 60 fps: about 130,000–200,000 instructions per frame on a 68030 at 50 MHz
(at ~4–6 clocks per instruction), half that at 25 MHz.

### Real hardware

PiStorm (Emu68, reports a 68040), raster lines per frame (NTSC budget: 262), measured
with `PROFILE`/`BENCH=300` before the assembler optimisations:

| Game | Emulation | Sound | Picture (before → now) | Speed without vsync |
|---|---|---|---|---|
| Demon Attack | 46 | 9 → 5 | 89 → 23 | 108 → 209 fps |
| River Raid | 81 | 5 | 30 → 9–12 | 159 fps |
| Enduro | 72 | 5 | 73 → 25 | 153 fps |

In real time: a steady 60 fps; on this machine chip RAM is the bottleneck.

68030/50 (TF530, A500): River Raid at first 5.3 fps (emulation 2419 lines per frame,
~6 MIPS). Full speed is out of reach there; the minimum for full speed is a 68060/50.

### 68k instructions per frame (tools/m68kprof)

`make build/m68kprof` and `make -f Makefile.amiga bench` build an instruction counter
based on the Musashi emulator and an OS-free benchmark program from exactly the code
that runs on the Amiga (vbcc, asm CPU, asm TIA, asm picture conversion). The result is
exact and reproducible, per function with call counts:

```sh
./build/m68kprof build/m68kbench/bench.exe build/m68kbench/bench.map roms/riverraid.bin -frames 30 -video
```

On real hardware there is also a sampling profiler (`A26 rom PROFPC`, only with the OS
switched off; evaluate with `tools/pcprof.py` and `make -f Makefile.amiga map`).

| Step | River Raid | Cosmic Ark |
|---|---|---|
| Start (C renderer, C picture conversion) | 1,113,000 | – |
| needless mask updates removed | 1,000,000 | – |
| picture conversion in assembler | 856,000 | – |
| TIA renderer in assembler | 757,000 | – |
| TIA write path in assembler | 653,000 | 915,000 |
| timer wait loops skipped | 584,000 | 915,000 |
| lock masks precomputed | 584,000 | 619,000 |
| HMOVE in assembler, faster dispatch loop | 538,000 | 613,000 |
| segment memo (see below) | 521,000 | 610,000 |
| audio shortcut, compact memo signature | 510,000 | 588,000 |
| drawn lines flagged (picture output) | 513,000 | 580,000 |

Current (instructions per frame): Klax 323k, Demon Attack 367k, Ms. Pac-Man 380k,
Pitfall 393k, River Raid 516k, Cosmic Ark 580k, Enduro 582k, Solaris 639k. Expensive
instructions like `mulu.l` are replaced by additions/shifts in the write path (more
instructions, fewer clocks).

**Segment memo:** a line is drawn in segments between two TIA writes. For each segment
(line, sequence number) the renderer remembers the complete render-relevant state
(44 bytes, laid out contiguously in the Tia struct), the pixel range and the collisions
produced. If the same segment comes again with the same state, the pixels are still in
the frame buffer (segments of a line never overlap), and only the collisions are taken
over. Entries behind a line's last segment are dropped at the end of the line; segments
that keep changing are only stored every 16th time. The TIA also flags every line in
which it actually draws; the picture output skips unflagged lines without comparing.

**Sound:** the Paula buffer length is set per frame through an accumulator so that a
buffer is exactly one frame long on average (with a fixed length Paula repeated a buffer
about every 10 s on NTSC and every 5 s on PAL: an audible click). In clocks in which the
divider does not fire, the TIA sound channels are only counted on (exact).

**Timer wait loops:** almost every game waits at the end of the frame in
`LDA INTIM / BNE *-3` (or `BPL`, `LDX`/`LDY`). When the core reads INTIM inside such a
loop, time jumps ahead by whole loop rounds up to the read that ends the loop. This is
identical cycle for cycle (checked: all ROMs bit-identical with and without `-noskip`)
but saves emulating every round.

### TIA renderer

`render()` draws a segment in two steps:

1. **Playfield/background:** the playfield only changes every 4 pixels. Whole blocks are
   therefore written as one longword, with a running bit mask per screen half.
2. **Objects as stamps:** only inside the precomputed pixel ranges of the active objects
   (copies × width, derived directly from the mask tables) does the full priority and
   collision logic run. Everywhere else the result is by definition playfield or
   background without collision.

In addition, writes without a visible effect (HMxx, HMCLR, unchanged colours/CTRLPF/
ENAxx/VDEL/RESMP) do not trigger catching up the beam. Registers with a take-over delay
(PFx, NUSIZx, REFPx, VBLANK) are excluded from this: catching up for them draws a few
pixels ahead and thereby shifts the effect of the following writes.

The old pixel-by-pixel renderer stays as a reference (`-DA26_TIA_REFERENCE`).
`tests/tia_equiv.c` feeds both with random, 2600-typical write sequences (40 seeds ×
60 frames) and requires identical pixels and identical collision values. Every further
optimisation has to pass this test.

With the pointer PC the CPU costs about 23 68k instructions per emulated 6502
instruction; at first it was ~100 with the C core and ~40 with the first asm core.

### Where the time goes now (asm CPU)

`busy_ntsc`, per frame (~1,050 TIA writes, 3 render segments per line):
- filling the playfield ~155,000 (≈ 600 per line). The block loop itself takes ~6
  instructions per 4-pixel block; the rest is setup and edges per segment.
- dispatch loop ~59,000
- TIA write path ~100,000: `tia_write`, `update_to` and `render` ~33,000 each;
  `asm_tia_write`, `write_class` and `tia_write_direct` together ~30,000
- opcodes the rest

TIA writes go from the asm core directly to `tia_write`, without `bus_write` and without
the bank switch check. The exception are 3F carts, where TIA writes switch the bank.

### Next steps, ordered by leverage

1. **Hotspot page $1Fxx:** treat only $1FE0–$1FFF as slow. Many games have code in the
   last page, which currently runs entirely in slow mode.
2. **Frameskip without pixel work:** in skipped frames only compute the collisions. This
   saves filling the playfield completely and helps most when frameskip is needed anyway.
3. **Detect unchanged lines:** do not redraw lines with the same register history as in
   the previous frame; the Amiga-side comparison for C2P already exists. The collisions
   must stay right.
4. **Measure on real hardware** (`BENCH=500`, `PROFILE`): qemu counts instructions, not
   clocks. Chip RAM accesses and the 256-byte caches of the 68030 only show on real
   hardware or in WinUAE's cycle-exact mode.

## Open points / next steps

1. **68040/68060 speed:** a measurement on a real 68040/25 and 68060/50 (or WinUAE
   cycle-exact) is still missing. Amiberry on macOS does not emulate at real-machine
   speed and is only good for function tests. After that the points from "Next steps"
   above (hotspot page, frameskip without pixel work).
2. **Accuracy:** find out why some attract modes drift apart from gopher2600 (see
   [reference comparison](#accuracy-reference-comparison)); HMOVE artefacts while an
   object is being drawn (missile disappears, ball gets wider) are still missing; locking
   across the end of the frame; paddles in the front end.
3. **PAL palette:** the row for hue D is interpolated and has to be checked against
   Stella.
4. **Sound when emulation is too slow:** Paula then repeats the old buffer. A ring buffer
   with an audio interrupt would be better.
