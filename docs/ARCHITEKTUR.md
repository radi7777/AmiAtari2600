# Architektur

```
src/core/      plattformneutraler Emulationskern (C99, auch vbcc-tauglich)
  cpu.c        6507: kompletter NMOS-Befehlssatz, 1 Buszugriff = 1 Zyklus
  bus.c        Adressdekodierung (TIA / RIOT-RAM / RIOT-I/O / Cartridge)
  tia.c        TIA: Video (Catch-up-Rendering), Kollisionen, Audio, Eingänge
  riot.c       6532: 128 Byte RAM, Timer (lazy berechnet), Ports
  cart.c       ROM-Mapping, Bankswitching, Typ-Erkennung, Lese-Map für den asm-Kern
  cpu_asm.c    C-Anbindung des Assembler-Kerns (Callbacks, Einzelschritt über cpu.c)
  atari.c      Frame-Schleife, Eingabe-API, PAL/NTSC-Erkennung
  palette.c    NTSC-/PAL-Paletten (24 Bit)
src/amiga/     Amiga-Frontend (AmigaOS-Header, direkter Hardwarezugriff)
  cpu6507.s    6507-Interpreter in 68030-Assembler (vasm), Rückfall auf cpu.c
  hw.c         System-Takeover/-Restore, Agnus-Erkennung, BEAMCON0, VBL-Sync, Zeilenzähler
  vidconv.c    TIA-Zeile -> Farbregister + Copper-Moves + Bitplanes (portabel, host-getestet)
  video.c      Copper-Listen, Double-Buffer, Displayfenster PAL/NTSC
  audio.c      Paula-Ausgabe
  input.c      Joystick (JOYxDAT/CIA) und Tastatur (CIA-Polling mit Handshake)
  main_amiga.c Kommandozeile, Hauptschleife, Frameskip
src/host/      Testrahmen für Linux/macOS (PPM/WAV-Ausgabe)
tests/         CPU-Test, Test-ROMs (eigener Assembler), Video-Konvertierungstest,
               TIA-Äquivalenztest (optimierter gegen Referenz-Renderer)
tools/         asm6502.py: kleiner 6502-Assembler für Test-ROMs
               qemu_icount.py: 68k-Befehle pro Frame/Funktion unter qemu-m68k zählen
```

## Zeitmodell

`a26_cycles` ist der Master-Takt (CPU-Zyklen). Jeder `bus_read`/`bus_write` zählt einen
Zyklus. Interne Zyklen addiert die CPU selbst, und zwar immer *vor* dem abschließenden
Schreibzugriff. Dadurch landen TIA-Writes (WSYNC, RESPx, HMOVE …) auf dem richtigen Zyklus.

Der TIA läuft nicht im Gleichschritt mit der CPU. Vor jedem TIA-Zugriff wird der Strahl
bis zum aktuellen Farbtakt (`a26_cycles * 3`) nachgezogen, und zwar mit den *alten*
Registerwerten. Erst danach wird der neue Wert übernommen. Das ist farbtaktgenau und
kostet nur dort Rechenzeit, wo tatsächlich gezeichnet wird. Bei einigen Registern gibt
es kleine Übernahmeverzögerungen (PFx, NUSIZx, REFPx, VBLANK), die dem Verhalten der
Hardware bzw. von Stella nachgebildet sind.

Für die Objekte gibt es Masken-Tabellen: Pro Objekt wird ein Zeiger in eine
320-Einträge-Tabelle geführt, und `mask[x]` liefert direkt das Grafikbit für Pixel `x`.
Zeilen ohne Player/Missiles/Ball laufen über einen schnellen Pfad nur für Playfield
und Hintergrund.

Der RIOT-Timer wird nicht getaktet. Er berechnet sich beim Lesen aus dem Zyklus des
letzten Schreibzugriffs.

## Bildausgabe auf dem Amiga

Der TIA liefert 160 Pixel pro Zeile, jedes mit einem von 128 Farbwerten. Eine Zeile
braucht aber fast nie mehr als eine Handvoll davon. Der Amiga läuft deshalb in Lowres
mit 5 Bitplanes (32 Farbregister), und jedes TIA-Pixel wird zwei Amiga-Pixel breit
(160 → 320).

Die 32 Farbregister arbeiten als Cache (`vidconv.c`):

1. Für jede Zeile werden die benötigten TIA-Farben nachgeschlagen.
2. Fehlt eine Farbe, bekommt sie ein Register, das diese Zeile nicht braucht (LRU).
   Ein Copper-MOVE am Zeilenanfang lädt es.
3. Pro Zeile sind maximal `VC_MAX_MOVES` = 10 neue Farben möglich (Copper-Zeit im
   horizontalen Austastbereich). Darüber wird die nächstliegende bereits geladene Farbe
   verwendet.

Die Copper-Liste hat pro Zeile ein `WAIT` und 10 MOVE-Slots, unbenutzte Slots sind NOOPs.
Geschrieben werden nur die Slots, die sich geändert haben.

Chunky-to-Planar (C2P) arbeitet mit Tabellen: 16 TIA-Pixel ergeben je Plane ein Langwort,
also 5 Langwort-Schreibzugriffe ins Chip-RAM. Pro Puffer liegt im Fast-RAM eine Kopie
der Register-Index-Zeilen, und unveränderte Zeilen werden gar nicht neu konvertiert.
Das spart auf dem 68030 den Großteil der Chip-RAM-Zugriffe, weil 2600-Bilder von Frame
zu Frame meist fast gleich bleiben.

Der Bildwechsel erfolgt über `COP1LC`: Der Copper startet beim nächsten Vertical Blank
mit der neuen Liste, es gibt also kein Tearing. Der sichtbare Bereich des Spiels wird
vertikal zentriert. Änderungen übernimmt der Emulator erst, wenn sie 30 Frames stabil
sind.

## PAL/NTSC

- Erkennung: gleitender Mittelwert der Scanlines pro Frame. Ab 287 Zeilen gilt PAL,
  darunter NTSC. In den ersten 60 Frames reichen 4 übereinstimmende Frames zum
  Umschalten, danach 16.
- Umschaltung: `BEAMCON0` Bit 5 (nur ECS-Agnus 8372A oder AGA). Die Agnus-ID stammt aus
  `VPOSR` (Bit 5 der ID = ECS). Danach setzt der Emulator Displayfenster (256 bzw. 200
  Zeilen), Palette und Paula-Periode neu.
- Beim Beenden stellt er BEAMCON0 aus `GfxBase->DisplayFlags` wieder her.

## Ton

Der TIA wird zweimal pro Scanline getaktet (31,4 kHz). Pro Scanline wird ein Sample
gespeichert. Paula spielt pro Amiga-Frame einen Puffer mit ca. einer Rasterzeile pro
Sample (Periode ≈ 227). Jeder TIA-Kanal liegt auf zwei Paula-Kanälen (links + rechts),
damit er in der Mitte klingt. Die Puffer werden ohne Interrupts gewechselt: Das neue
`AUDxLC` wird geschrieben, und Paula übernimmt es am Ende des laufenden Puffers.

## Assembler-CPU

`src/amiga/cpu6507.s` ist ein Hybrid-Kern. Die häufigen Opcodes (Laden/Speichern in allen
Adressierungsarten, ALU, Vergleiche, Branches, JSR/RTS, Stack, INC/DEC, Shifts) laufen in
Assembler. Alles andere springt zu `fallback`: Der Zustand wird an `cpu.c` übergeben,
genau ein Befehl wird in C ausgeführt, dann geht es in Assembler weiter. Damit ist der
Kern von Anfang an vollständig und kann Opcode für Opcode ausgebaut werden. Den Eintrag in
der Tabelle am Ende von `cpu6507.s` von `fallback` auf das neue `op_XX` ändern, und
Dormann-Test plus `make m68k-test` zeigen sofort, ob er stimmt.

Registerbelegung: d2 = A, d3 = X, d4 = Y, d5 = Zyklenzähler, d6/d7 = N-/Z-Quelle,
a3 = PC, a5 = Lese-Map, a6 = RAM. C, V, D und I liegen als Bytes im Kontext (`cpu_asm.h`).

Der PC ist ein direkter Zeiger ins ROM: Solange der Code in einer schnellen Seite läuft,
ist ein Opcode-Fetch nur `cmp.l PCEND,a3` / `move.b (a3)+,d0`. Erst am Seitenende, nach
Sprüngen oder nach einem Bankwechsel wird die Seite in der Tabelle nachgeschlagen. Code
in langsamen Seiten (Hotspot-Seite, RAM) läuft in einem „langsamen Modus“ über die
C-Callbacks. Branches innerhalb der Seite addieren nur den Offset. Genau dann entfällt
auch der Strafzyklus für den Seitenwechsel. `tests/roms/cpu_paths.asm` prüft diese Fälle
zyklengenau.

Speicherzugriffe laufen über eine Seitentabelle mit 256 Einträgen. Cartridge-Seiten zeigen
direkt ins ROM. Hotspot-Seiten, TIA und RIOT sind langsame Seiten und gehen über einen
C-Callback. Zero Page ab $80 und der Stack greifen direkt aufs RAM. Die Tabelle wird nur
nach Bankwechseln neu gebaut.

## Performance

Gemessen mit `make m68k-profile`: ausgeführte 68k-Befehle pro emuliertem Frame
(gcc -O2 -m68030, qemu-m68k; ohne Amiga-Bildkonvertierung und Chip-RAM):

| Test-ROM | Anfang (C-CPU) | asm-CPU | + neuer TIA-Renderer | + PC als Zeiger |
|---|---|---|---|---|
| `busy_ntsc` (CPU-lastig, ~23 6502-Befehle/Zeile, 2 TIA-Writes/Zeile) | 961.000 | 648.000 | 537.000 | **467.000** |
| `bars_ntsc` (Player-Sprite auf jeder Zeile, wenig CPU) | 1.035.000 | 1.019.000 | 273.000 | **264.000** |

Budget für 60 fps: etwa 130.000–200.000 Befehle pro Frame auf einem 68030 mit 50 MHz
(bei ~4–6 Takten pro Befehl), die Hälfte bei 25 MHz.

### TIA-Renderer

`render()` zeichnet einen Abschnitt in zwei Schritten:

1. **Playfield/Hintergrund:** Das Playfield wechselt nur alle 4 Pixel. Ganze Blöcke
   werden deshalb als ein Langwort geschrieben, mit einer mitlaufenden Bitmaske je
   Bildhälfte.
2. **Objekte als Stempel:** Nur in den vorberechneten Pixelbereichen der aktiven Objekte
   (Kopien × Breite, direkt aus den Masken-Tabellen abgeleitet) läuft die volle
   Prioritäts- und Kollisionslogik. Überall sonst ist das Ergebnis per Definition
   Playfield oder Hintergrund ohne Kollision.

Zusätzlich lösen Schreibzugriffe ohne sichtbare Wirkung (HMxx, HMCLR, unveränderte
Farben/CTRLPF/ENAxx/VDEL/RESMP) kein Nachziehen des Strahls aus. Register mit
Übernahmeverzögerung (PFx, NUSIZx, REFPx, VBLANK) sind davon ausgenommen: Ihr Nachziehen
zeichnet ein paar Pixel voraus und verschiebt damit die Wirkung folgender Writes.

Der alte Pixel-für-Pixel-Renderer bleibt als Referenz erhalten (`-DA26_TIA_REFERENCE`).
`tests/tia_equiv.c` füttert beide mit zufälligen, 2600-typischen Schreibfolgen
(40 Seeds × 60 Frames) und verlangt identische Pixel und identische Kollisionswerte.
Jede weitere Optimierung muss diesen Test bestehen.

Die CPU kostet mit dem Zeiger-PC etwa 23 68k-Befehle pro emuliertem 6502-Befehl,
anfangs waren es ~100 mit dem C-Kern und ~40 mit dem ersten asm-Kern.

### Wo die Zeit jetzt hingeht (asm-CPU)

- `busy_ntsc`:
  - TIA-Rendering (`update_to` inkl. Playfield) ~47 %
  - Dispatch-Schleife (`loop`) ~13 %
  - Weg eines TIA-Writes (asm → `slow_write` → `cb_write` → `bus_write` → `tia_write`)
    ~17 %
  - Opcodes der Rest
- `bars_ntsc`: Playfield ~25 %, Objektpixel (`eval_pixels`) ~17 %

### Nächste Schritte, nach Hebelwirkung geordnet

1. **TIA-Writes direkt aus Assembler:** Zero-Page-Writes nach $00–$3F sofort an
   `tia_write` statt über `slow_write` → `cb_write` → `bus_write`.
2. **Hotspot-Seite $1Fxx** nur für $1FE0–$1FFF langsam behandeln. Viele Spiele haben
   Code in der letzten Seite, der im Moment im langsamen Modus läuft.
3. **Unveränderte Zeilen erkennen:** Zeilen mit gleichem Registerverlauf wie im
   Vorframe nicht neu zeichnen; den Amiga-seitigen Vergleich für C2P gibt es schon.
   Die Kollisionen müssen dabei weiter stimmen.
4. **Frameskip ohne Pixelarbeit:** In übersprungenen Frames nur Kollisionen berechnen.
5. **Auf echter Hardware messen** (`BENCH=500`, `PROFILE`): qemu zählt Befehle, keine
   Takte. Chip-RAM-Zugriffe und die 256-Byte-Caches des 68030 kann nur echte Hardware
   bzw. WinUAE im Cycle-Exact-Modus zeigen.

## Offene Punkte / nächste Schritte

1. **Erster echter Build mit vbcc + NDK** auf dem Mac und die nötigen Korrekturen.
2. **Test in einem Emulator** (FS-UAE/Amiberry auf dem Mac, A1200 bzw. A3000 mit
   68030 und ECS), danach auf echter Hardware.
3. **Performance auf dem 68030 messen und optimieren** (siehe [Performance](#performance)).
4. **Genauigkeit gegen echte Spiele prüfen** (per Screenshot-Vergleich mit Stella):
   - HMOVE mitten in der Zeile ("Cosmic Ark"-Sterne)
   - Verzögerung beim RESMP-Entriegeln
   - Paddles (Grundgerüst vorhanden, im Frontend noch nicht angeschlossen)
5. **PAL-Palette:** Die Zeile für Farbton D ist interpoliert und muss mit Stella
   abgeglichen werden.
6. **Agnus-ID-Werte** ($20/$30 ECS, $22/$23 AGA) an echter Hardware bestätigen.
7. **Workbench-Start** (Icon, ASL-Requester) fehlt, bisher nur CLI.
8. **Ton bei zu langsamer Emulation:** Paula wiederholt dann den alten Puffer. Besser
   wäre ein Ringpuffer mit Audio-Interrupt.
