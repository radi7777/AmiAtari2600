# AmiAtari2600

Atari-2600-Emulator für klassische Amigas (68030, ECS-Chipsatz). Er läuft ohne SDL und
ohne OS-Grafik und greift direkt auf Copper, Bitplanes, Paula und BEAMCON0 zu.
Projektziele und Hintergrund stehen in [docs/PROJEKT.md](docs/PROJEKT.md), der Aufbau in
[docs/ARCHITEKTUR.md](docs/ARCHITEKTUR.md).

## Stand

| Teil | Status |
|---|---|
| 6507-CPU in C (inkl. illegaler Opcodes) | fertig, besteht Klaus Dormanns 6502-Funktionstest |
| 6507-CPU in 68k-Assembler (`src/amiga/cpu6507.s`) | 135 Opcodes in Assembler, Rest über den C-Kern, PC als direkter ROM-Zeiger; besteht den Dormann-Test auf 68k (Amiberry), rendert identisch zum C-Kern |
| TIA-Video (Playfield, Player, Missiles, Ball, Kollisionen, HMOVE, VDEL) | fertig, farbtaktgenaues Catch-up-Rendering; HMOVE zyklusgenau (alle Zyklen × alle HM-Werte gegen gopher2600 vermessen), inkl. Cosmic-Ark-Sterne; 15 von 21 getesteten Spielen pixelgleich mit gopher2600 |
| TIA-Audio | fertig (Schaltungsmodell, 1 Sample pro Scanline) |
| RIOT (RAM, Timer, Ports) | fertig, Timer zyklusgenau wie gopher2600 |
| Bankswitching | 2K, 4K, F8, F6, F4, F8SC, F6SC, F4SC, FA, E0, E7, 3F, FE, mit Auto-Erkennung |
| PAL/NTSC-Erkennung | fertig (Zeilen pro Frame, mit Hysterese) |
| Amiga: Video (2 Farbbänke, Copper, C2P, Double-Buffer, nur geänderte Langwörter ins Chip-RAM) | läuft auf echter Hardware (A1200 mit Emu68/68040, KS 3.2) und in Amiberry |
| Amiga: Eingabe (späte Joystick-Abfrage, Frame-Verzögerung) | läuft auf echter Hardware |
| Amiga: systemfreundlicher Modus (OS-Interrupts laufen weiter) / `KILLOS` | läuft auf echter Hardware |
| Amiga: BEAMCON0-Umschaltung, Paula | läuft; PAL/NTSC-Wechsel mitten im Spiel noch nicht auf echter Hardware geprüft |
| Amiga: vbcc-Build | fertig (`make -f Makefile.amiga`) |

Der Kern ist plattformneutrales C und wird auf dem Host sowie als 68k-Binary in Amiberry
getestet (`tests/amiga_tests.sh`), mit C- und mit Assembler-CPU. Die Genauigkeit wird mit
`tools/refcheck.sh` gegen gopher2600 geprüft.

Performance-Stand und Plan: siehe [docs/ARCHITEKTUR.md](docs/ARCHITEKTUR.md#performance).

## Bauen

### Host (macOS/Linux): Kern + Testrahmen

```sh
make            # build/a26host
make test       # CPU-Test, Test-ROMs, TIA-Äquivalenz, Video-Konvertierung
./build/a26host roms/spiel.bin -frames 300 -ppm shot.ppm -wav sound.wav
```

`a26host` lässt ein ROM ohne Bildschirm laufen und gibt Typ, Region, Zeilenzahl und
sichtbaren Bereich aus. Optional schreibt es das letzte Bild als PPM (320 Pixel breit,
wie auf dem Amiga) und den Ton als WAV. Weitere Optionen: `-type F8`, `-region pal`,
`-reset N` (Game Reset ab Frame N drücken), `-fire N`, `-bench`, `-clean` (leeres RAM,
INTIM = 0, wie gopher2600), `-raw prefix -rawfrom N` (Frames als TIA-Farbindizes).
`make build/a26trace` baut eine Variante, die mit `-trace N` alle TIA-Writes von Frame N
mit Zeile, Zyklus und Pixel ausgibt.

**68k-Tests in Amiberry** (ohne qemu): `make -f Makefile.amiga tests` baut Testrahmen und
CPU-Test als Amiga-Programme (C- und asm-Kern), `sh tests/amiga_tests.sh [roms …]` startet
Amiberry mit dem Testordner als Bootlaufwerk, wartet auf die Ergebnisse und verlangt
identische Bilder und Töne von C- und asm-Kern und vom Host-Build.

**Genauigkeit gegen gopher2600:** `REFDUMP=/pfad/zu/refdump sh tools/refcheck.sh 300 10 roms/*.bin`
vergleicht die Frames 300–309 pixelweise (refdump ist ein kleines Go-Programm auf Basis
von gopher2600, das Frames als TIA-Farbindizes schreibt). `tools/refcompare.py … --detail`
zeigt die abweichenden Zeilen.

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

Bringt die vbcc-Installation keine passende Config mit (z. B. die aus der
m68k-amigaos-gcc-Toolchain, deren `aos68k` auf fremde Pfade zeigt), eine angepasste Kopie
per `CONFIG=+/pfad/zu/aos68k` übergeben.

In VS Code gibt es fertige Tasks (`Terminal > Run Task`): Host-Build, Tests und
Amiga-Build. Für den Amiga-Build muss die Umgebungsvariable `NDK_INC` gesetzt sein.

## Benutzung auf dem Amiga

```
A26 <rom> [PAL|NTSC] [COLORS=PAL|NTSC] [TYPE=F8|F6|...] [SKIP=n] [DELAY=n] [PORT1]
    [NOSOUND] [KILLOS] [PROFILE] [BENCH=n] [FRAMES=n]
```

- Region und Bankswitching werden automatisch erkannt. `PAL`/`NTSC` bzw. `TYPE=`
  erzwingen sie.
- `COLORS=PAL`: PAL-Palette bei NTSC-Timing (für PAL60-Spiele).
- `SKIP=n`: nur jedes (n+1)-te Frame darstellen. Ohne Angabe regelt der Emulator das
  automatisch (höchstens 2 Frames am Stück).
- `DELAY=n`: Emulation jedes Frames erst n Rasterzeilen nach dem Vertical Blank starten.
  Ohne Angabe regelt der Emulator das automatisch so, dass der Frame kurz vor dem
  nächsten Vertical Blank fertig ist: Das verkürzt die Zeit zwischen Joystick und Bild.
  `DELAY=0` schaltet das ab.
- `PORT1`: Joystick im Mausport steuert Spieler 2.
- `PRI=n`: Task-Priorität während des Spiels. Standard ist 19: über Netzwerk- und
  WLAN-Tasks, die sonst gelegentlich 100–150 ms Rechenzeit belegen (sichtbarer Hänger),
  und unter input.device (20), damit die Tastatur immer bedienbar bleibt.
- `KILLOS`: schaltet während des Spiels die OS-Interrupts ab (etwas schneller auf
  langsamen Maschinen). Standard ist der systemfreundliche Modus, in dem Timer,
  Tastatur und Netzwerk weiterlaufen.
- `PROFILE`: gibt beim Beenden eine Zeittabelle aus. Sie zeigt pro Frame Wartezeit vor
  der Emulation (Frame-Verzögerung), Emulation, Ton, Bildkonvertierung und
  Vsync-Wartezeit in Rasterzeilen. Das Budget liegt bei 312 (PAL) bzw. 262 (NTSC)
  Zeilen pro Frame.
- `FRAMES=n`: nach n Frames beenden (für Messungen mit `PROFILE`).
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

## Voraussetzungen

**Emulator (`A26`):**
- Für volle Geschwindigkeit mindestens **68060 mit 50 MHz** (oder vergleichbar, z. B.
  Emu68/PiStorm). Langsamere CPUs laufen mit Frameskip.
- Für die PAL/NTSC-Umschaltung zur Laufzeit **8372A Fat Agnus (ECS)**. Mit OCS-Agnus
  läuft der Emulator im nativen Modus des Rechners, ohne Umschaltung.
- Keine weiteren Bibliotheken oder Tools. ROMs können direkt aus `.zip`-Dateien geladen
  werden, ein externes unzip ist nicht nötig.

**Oberfläche (`A26GUI`):**
- **MUI 3.8+**, OS 3.x. Für Screenshots wird ein **picture.datatype V43** gebraucht
  (OS 3.2, 3.5/3.9) und ein **PNG-Datatype** (nicht bei jedem OS dabei, sonst Aminet).
- Optional **curl** und **AmiSSL** (beides Aminet): Damit lädt die GUI die Spieledatenbank
  (libretro-database) und die Screenshots (libretro-thumbnails) selbst über https.
  Ohne curl/AmiSSL geht alles andere auch. Datenbank und Bilder kann man dann selbst
  kopieren: die `.dat`-Dateien nach `db/` (`nointro.dat`, `publisher.dat`,
  `developer.dat`, `releaseyear.dat`, `genre.dat`), die Bilder als `snaps/<CRC32>.png`
  (8 Hex-Ziffern in Großbuchstaben, z. B. `snaps/1E86DE5A.png`), beides im Programmordner.
- cybergraphics.library wird benutzt, wenn vorhanden (RTG-Bildschirm), ist aber nicht
  nötig.

## Lizenz

MIT, siehe [LICENSE](LICENSE). Der Code ist eigenständig geschrieben und enthält keinen
Stella-Code. Das TIA-Audio-Modell bildet dieselbe Hardware-Logik nach, die auch Stella
verwendet.

Die Spieledatenbank und die Screenshots gehören nicht zum Projekt. Die GUI lädt sie erst
beim Benutzer von libretro. ROMs werden nicht mitgeliefert.

`tools/refdump` wird gegen gopher2600 (GPL) gebaut. Es ist nur ein Entwicklerwerkzeug,
ein fertiges Binary davon wird nicht verteilt.
