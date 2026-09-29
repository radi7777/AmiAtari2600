# Atari 2600 emulator for the Amiga — project brief

> This is the original brief the project started from. Some of it is out of date: the
> emulator was written from scratch instead of porting Stella, and full speed turned out
> to need a 68060/50 rather than a 68030. The current state is in the
> [README](../README.md) and in [ARCHITECTURE.md](ARCHITECTURE.md).

## Goal
Native Atari 2600 emulator (code name "Stella port") for classic AmigaOS on 68030
hardware (e.g. an A1200 with a Blizzard/TK2 accelerator), target chipset **ECS**. No SDL,
no abstraction layers — direct access to the custom chips for maximum performance. The
target system runs a 68030 at about 25–50 MHz, optionally with a 68882 FPU. Must support
**both PAL and NTSC**.

## Starting point / reference projects

- **Stella** (https://stella-emu.github.io) is the current reference emulator, but it now
  targets C++20 + SDL2 + g++-11/clang++-10 — not sensibly buildable with 68k toolchains.
- There was already **a historic Amiga port**: Matthew Stroup ported Stella (version
  ~1.1, around 1999) to AmigaOS, released on Aminet as `misc/emu/Stella.lha`. That code
  targets 68020/OS3, without SDL2 (SDL2 only came with Stella 4.0, around 2014).
- Recommendation: check a **pre-SDL2 code base** (Stella 1.x–3.x) as the starting point
  for the CPU/TIA core instead of forking current Stella 7.0. Possibly look through the
  old Stroup Amiga port for graphics/sound routines to save boilerplate.

## Architecture decision: no SDL

SDL costs too much on a 68030 (event loop abstraction, software blitting, needless
memory copies). Instead:

- **CPU/TIA emulation core**: plain, portable C, compiled directly with vbcc. The heart
  of it is the cycle-exact emulation of the TIA graphics chip per scanline (not just the
  6507 CPU at 1.19 MHz) — that is where the real work is.
- **Graphics output**: no SDL_Renderer/SDL_Texture. Write directly into Amiga bitplane
  buffers, display through `graphics.library` or direct custom register access. Double
  buffering without OS calls where possible.
- **Sound**: directly through Paula (4 DMA-driven hardware channels), with a small sample
  buffer management of its own instead of an SDL audio queue.
- **Timing/sync**: based on the Amiga's blitter/Copper timing, not on SDL timer callbacks.

## Colour and resolution mapping (Atari TIA → Amiga ECS)

- **TIA colours**: 128 colours in total (NTSC) / 104 (PAL), but only **4 colours at once
  per scanline** (background, playfield, player 0, player 1).
- **Amiga colour mode**: standard lowres is plenty — 5 bitplanes = 32 colours at once out
  of 4096 (12-bit RGB). No EHB (64 colours) or HAM (4096 at once) needed, since the TIA
  never needs more than 4 colours per line.
- **TIA resolution**: 160 visible colour clocks per line (≈320 "real" pixels with wide
  pixels), 192 lines (NTSC) or 228 lines (PAL) visible, out of 228 colour clocks per
  line in total (68 horizontal blank + 160 visible).
- **Target screen mode PAL**: 320×256, lowres, 5 bitplanes, 50 Hz.
- **Target screen mode NTSC**: 320×200, lowres, 5 bitplanes, 60 Hz.

## PAL and NTSC support (required)

The project must **support both regions**, not just one. Technical points:

- On ECS Amigas with an **8372A "Fat Agnus"** (rev 6 board) PAL and NTSC can be switched
  at run time without a reboot, through the **BEAMCON0** custom chip register
  (timing switched by software).
- Agnus pin 41 (high = PAL, low = NTSC) only matters for the permanent boot
  configuration, not for switching at run time.
- **Minimum target hardware**: 8372A Agnus (1 MB Fat Agnus, ECS). Older Agnus variants
  (8370/8371) are hard-wired to one mode and are ruled out as a target.
- **Planned behaviour**: when a ROM is loaded, derive its region (PAL/NTSC) from the TIA
  timing (line count/sync pattern) and switch the Amiga screen mode and BEAMCON0 timing
  to match automatically — no user action, no reboot.
- Prefer direct BEAMCON0 access, no detour through an AmigaOS screen mode requester (fits
  the no-SDL/no-OS-overhead approach).

## Toolchain

- Compiler: **vbcc**, target `m68k-amigaos`
- Flags: `-cpu=68030 -fpu=68882` (FPU only if present / check at run time)
- Basic build command: `vc +aos68k -cpu=68030 -O2 file.c -o program` (vlink links in
  the background)
- NDK: AmigaOS NDK 3.1/3.2 (Hyperion) for `hardware/custom.i`, `graphics.library` headers
  etc.
- Performance: use vbcc `__reg()` to pass critical function parameters (above all in
  the fetch-decode-execute loop of the CPU emulation) in registers instead of on the
  stack.

## Open points / next steps (as planned at the start)

1. Find a suitable pre-SDL2 Stella source version and mind its licence (GPL).
2. Look through the old Stroup Amiga port (Aminet) for usable graphics/sound routines.
3. Isolate the TIA emulation core and check it is plain portable C (no platform
   dependencies).
4. Prototype the BEAMCON0 switching logic (e.g. with the well-known ntscswitch test
   program as a reference).
5. Implement ROM region detection (PAL/NTSC from the TIA sync/line data).
6. Design Paula sound output for TIA audio (2 channels, simple waveforms).
7. Set a benchmark goal: full frame rate (60 Hz NTSC / 50 Hz PAL) on a 68030 at what
   clock speed?

## Target hardware (reference)

- Amiga with ECS chipset, **8372A Fat Agnus (rev 6)** required for switching PAL/NTSC
  at run time
- 68030 accelerator (e.g. Blizzard, TK2), 25–50 MHz
- Optional 68882 FPU
- Screen resolution: 320×256 (PAL) / 320×200 (NTSC), lowres, 5 bitplanes, 32 colours
