# AmiAtari2600

Atari-2600-Emulator für klassische Amigas (68030, ECS-Chipsatz). Er läuft ohne SDL und
ohne OS-Grafik und greift direkt auf Copper, Bitplanes, Paula und BEAMCON0 zu.
Projektziele und Hintergrund stehen in [docs/PROJEKT.md](docs/PROJEKT.md), der Aufbau in
[docs/ARCHITEKTUR.md](docs/ARCHITEKTUR.md).

## Stand

| Teil | Status |
|---|---|
| 6507-CPU in C (inkl. illegaler Opcodes) | fertig, besteht Klaus Dormanns 6502-Funktionstest |
| 6507-CPU in 68k-Assembler (`src/amiga/cpu6507.s`) | 135 Opcodes in Assembler, Rest über den C-Kern; besteht den Dormann-Test unter qemu-m68k, rendert identisch zum C-Kern |
| TIA-Video (Playfield, Player, Missiles, Ball, Kollisionen, HMOVE, VDEL) | fertig, farbtaktgenaues Catch-up-Rendering |
| TIA-Audio | fertig (Schaltungsmodell, 1 Sample pro Scanline) |
| RIOT (RAM, Timer, Ports) | fertig |
| Bankswitching | 2K, 4K, F8, F6, F4, F8SC, F6SC, F4SC, FA, E0, E7, 3F, FE, mit Auto-Erkennung |
| PAL/NTSC-Erkennung | fertig (Zeilen pro Frame, mit Hysterese) |
| Amiga: Video (Copper-Palette pro Zeile, C2P, Double-Buffer) | geschrieben, Konvertierung auf dem Host getestet, **noch nie auf Amiga gelaufen** |
| Amiga: BEAMCON0-Umschaltung, Paula, Joystick, Tastatur | geschrieben, **ungetestet** |
| Amiga: vbcc-Build | Makefile vorhanden, **noch nie mit vbcc/NDK übersetzt** (der Assembler-Kern ist mit vasm -Fhunk getestet) |
| Amiga: Zeitmessung (`PROFILE`, `BENCH=n`) | geschrieben, **ungetestet** |

Der Kern ist plattformneutrales C und wird hier auf Linux sowie als 68030-Binary unter
qemu-m68k (big-endian) getestet, mit C- und mit Assembler-CPU. Der übrige Amiga-Teil
wurde nur gegen nachgebaute NDK-Header syntaktisch geprüft.

Performance-Stand und Plan: siehe [docs/ARCHITEKTUR.md](docs/ARCHITEKTUR.md#performance).

## Bauen

### Host (macOS/Linux): Kern + Testrahmen

```sh
make            # build/a26host
make test       # CPU-Test, Test-ROMs, Video-Konvertierung
./build/a26host roms/spiel.bin -frames 300 -ppm shot.ppm -wav sound.wav
```

`a26host` lässt ein ROM ohne Bildschirm laufen und gibt Typ, Region, Zeilenzahl und
sichtbaren Bereich aus. Optional schreibt es das letzte Bild als PPM (320 Pixel breit,
wie auf dem Amiga) und den Ton als WAV. Weitere Optionen: `-type F8`, `-region pal`,
`-reset N` (Game Reset ab Frame N drücken), `-fire N`, `-bench`.

Mit m68k-Cross-GCC, `qemu-m68k` und `vasmm68k_mot` (Linux, z. B. im Docker-Container
auf dem Mac) gibt es außerdem:

```sh
make m68k-test                         # alle Tests als 68030-Binary, C- und asm-CPU
make m68k-profile PROFILE_ROM=roms/x.bin   # 68k-Befehle pro Frame, nach Funktionen
```

ROMs gehören in den Ordner `roms/`. Er ist von git ausgenommen, und `make test` lässt
alle `roms/*.bin` zusätzlich 300 Frames laufen.

### Amiga (vbcc)

Voraussetzungen: vbcc mit Target `m68k-amigaos` (`$VBCC` gesetzt, `vc` und
`vasmm68k_mot` im PATH) und das AmigaOS NDK 3.1 oder 3.2.

```sh
make -f Makefile.amiga NDK_INC=/pfad/zu/NDK3.2/Include_H
# -> build/amiga/A26 (mit Assembler-CPU; ASM_CPU=0 für die C-CPU)
```

In VS Code gibt es fertige Tasks (`Terminal > Run Task`): Host-Build, Tests und
Amiga-Build. Für den Amiga-Build muss die Umgebungsvariable `NDK_INC` gesetzt sein.

## Benutzung auf dem Amiga

```
A26 <rom> [PAL|NTSC] [COLORS=PAL|NTSC] [TYPE=F8|F6|...] [SKIP=n] [PORT1] [NOSOUND]
    [PROFILE] [BENCH=n]
```

- Region und Bankswitching werden automatisch erkannt. `PAL`/`NTSC` bzw. `TYPE=`
  erzwingen sie.
- `COLORS=PAL`: PAL-Palette bei NTSC-Timing (für PAL60-Spiele).
- `SKIP=n`: nur jedes (n+1)-te Frame darstellen. Ohne Angabe regelt der Emulator das
  automatisch (höchstens 2 Frames am Stück).
- `PORT1`: Joystick im Mausport steuert Spieler 2.
- `PROFILE`: gibt beim Beenden eine Zeittabelle aus. Sie zeigt pro Frame Emulation, Ton,
  Bildkonvertierung und Vsync-Wartezeit in Rasterzeilen, gemessen über den CIA-B-TOD.
  Das Budget liegt bei 312 (PAL) bzw. 262 (NTSC) Zeilen pro Frame.
- `BENCH=n`: lässt n Frames ohne Vsync und ohne Frameskip so schnell wie möglich laufen
  und gibt dann die Tabelle und die erreichten Frames pro Sekunde aus.

| Taste | Funktion |
|---|---|
| Joystick Port 2 / Cursor + Space/Alt | Spieler 1 |
| F1 / F2 | Game Reset / Game Select (gedrückt halten) |
| F3 | Farbe / Schwarzweiß |
| F4 / F5 | Difficulty links / rechts |
| F6 | Region: Auto → NTSC → PAL → Auto |
| P | Pause |
| HELP | Hard-Reset |
| ESC | Beenden |

Zielhardware: 68030 mit 25–50 MHz und **8372A Fat Agnus (ECS)** für die
PAL/NTSC-Umschaltung zur Laufzeit. Mit OCS-Agnus läuft der Emulator im nativen Modus
des Rechners, ohne Umschaltung.

## Lizenz

Noch nicht festgelegt. Der Code ist eigenständig geschrieben und enthält keinen
Stella-Code. Das TIA-Audio-Modell bildet dieselbe Hardware-Logik nach, die auch Stella
verwendet. Falls später Stella-Code übernommen wird, muss das Projekt GPL-2.0-kompatibel
lizenziert werden.
