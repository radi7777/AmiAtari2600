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
  irq.s        VBL-Interrupt-Server (zählt Frames, weckt den Emulator-Task)
  hw.c         Display-Übernahme (systemfreundlich oder KILLOS), Agnus-Erkennung, BEAMCON0,
               VBL-Sync, Warten auf Rasterzeilen, Zeitmessung
  vidconv.c    TIA-Zeile -> Farbbank + Copper-Moves + Bitplanes (portabel, host-getestet)
  video.c      Copper-Listen, Double-Buffer, Displayfenster PAL/NTSC, Diff-Schreiben ins Chip-RAM
  audio.c      Paula-Ausgabe
  input.c      Joystick (JOYxDAT/CIA), Tastatur über input.device-Handler bzw. CIA-Polling
  main_amiga.c Kommandozeile, Hauptschleife, Frame-Verzögerung, Frameskip
src/host/      Testrahmen für Linux/macOS (PPM/WAV-Ausgabe)
tests/         CPU-Test, Test-ROMs (eigener Assembler), Video-Konvertierungstest,
               TIA-Äquivalenztest (optimierter gegen Referenz-Renderer),
               amiga_tests.sh: dieselben Tests als 68k-Binaries in Amiberry
tools/         asm6502.py: kleiner 6502-Assembler für Test-ROMs
               refcompare.py / refcheck.sh: Bildvergleich mit gopher2600
               gen_hmove_rom.py: HMOVE-Test-ROMs (alle Zyklen x alle HM-Werte)
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
es kleine Übernahmeverzögerungen (PFx, NUSIZx, REFPx, VBLANK, GRPx und ENAxx je 1
Farbtakt), die dem Verhalten der Hardware bzw. von Stella/gopher2600 nachgebildet sind.

Weitere Hardware-Details, die das Modell abbildet:

- **RESPx:** Die Hauptkopie eines Players entsteht erst beim Überlauf seines Zählers.
  Nach einem Reset mitten in der Zeile erscheint sie deshalb erst ab der nächsten Zeile,
  die weiteren Kopien (NUSIZ) schon in derselben.
- **HMOVE:** Die Wirkung hängt vom CPU-Zyklus des Writes ab. `hmove_disp[HM][Zyklus]`
  ist aus gopher2600 vermessen (`tools/gen_hmove_rom.py`) und folgt einem einfachen
  Modell (siehe `tia.c`): Ab 3·Zyklus+2 kommt alle 4 Farbtakte ein Zusatzimpuls, bis der
  Zähler den HM-Wert erreicht. Nur Impulse im (verlängerten) HBLANK bewegen das Objekt,
  der HMOVE-Balken schiebt alles um 8 Pixel nach rechts. Früh in der Zeile ergibt das
  die bekannte Bewegung, in der Mitte nichts, spät in der Zeile wirkt es erst in der
  nächsten Zeile (Zyklus-73/74-Trick ohne Balken).
- **HMxx-Writes während der Welle** ändern das Ziel. Liegt der neue Stoppwert schon
  hinter dem Zähler, bleibt das Objekt „verriegelt“ und bekommt dauerhaft Zusatzimpulse:
  17 Pixel pro Zeile nach links und je nach Position mod 4 veränderte Breite. Das ist
  der Sternenhimmel von Cosmic Ark. Die Verriegelung endet mit dem nächsten HMOVE oder
  mit HM = $80.

Für die Objekte gibt es Masken-Tabellen: Pro Objekt wird ein Zeiger in eine
320-Einträge-Tabelle geführt, und `mask[x]` liefert direkt das Grafikbit für Pixel `x`.
Zeilen ohne Player/Missiles/Ball laufen über einen schnellen Pfad nur für Playfield
und Hintergrund.

Der RIOT-Timer wird nicht getaktet. Er berechnet sich beim Lesen aus dem Zyklus des
letzten Schreibzugriffs: Nach dem Schreiben von N im Zyklus w liefert ein Lesen im Zyklus
r den Wert N − 1 − (r − w − 1) / Intervall. Nach dem Nulldurchgang zählt er von $FF
im Takt weiter und setzt das Timer-Flag; Lesen von INTIM löscht es wieder. Das stimmt
zyklusgenau mit gopher2600 überein (ein Zyklus Abweichung genügte, um INTIM-Warteschleifen
und damit VBLANK in einzelnen Frames zu verschieben).

## Genauigkeit: Referenzvergleich

`tools/refcheck.sh` lässt ROMs parallel im eigenen Kern und in gopher2600 laufen (über
ein kleines Go-Programm, das Frames als TIA-Farbindizes ausgibt) und vergleicht die Bilder
pixelweise, mit automatischer Suche nach Frame- und Zeilenversatz. Mit `-clean` startet
der Kern mit leerem RAM und INTIM = 0 wie gopher2600, sonst würden Spiele, die ihren
Zufall aus dem Startzustand ziehen, von Anfang an verschieden laufen.

Stand (Frames 300–309 im Attract-Modus, 21 Spiele): 15 pixelgenau, darunter Cosmic Ark,
Pitfall, River Raid, Solaris, Ms. Pac-Man, H.E.R.O., Space Invaders, Yars' Revenge,
Missile Command, Kaboom und Combat. Bei Asteroids, Battlezone, Demon Attack, Enduro,
Jr. Pac-Man und Moon Patrol laufen die Spielzustände nach einiger Zeit auseinander
(Asteroids z. B. ab Frame 257), die Positionen und das Timing einzelner Zeilen stimmen.
Die Ursache ist noch offen (Startzustand von CPU/TIA oder ein seltener Timing-Fall).

## Bildausgabe auf dem Amiga

Der TIA liefert 160 Pixel pro Zeile, jedes mit einem von 128 Farbwerten. Eine Zeile
braucht aber fast nie mehr als eine Handvoll davon. Der Amiga läuft deshalb in Lowres
mit 5 Bitplanes, und jedes TIA-Pixel wird zwei Amiga-Pixel breit (160 → 320).

Die 32 Farbregister sind in zwei Bänke geteilt (`vidconv.c`): Gerade Zeilen nutzen
COLOR00–15, ungerade COLOR16–31. Bitplane 5 wählt die Bank und ist pro Zeile konstant
(einmal pro Moduswechsel geschrieben). Der Copper lädt die Farben einer Zeile schon
während der Vorzeile, die ja die andere Bank zeigt. Dafür bleibt eine ganze Rasterzeile
Copper-Zeit, also bis zu 15 Farben pro Zeile (Register 0 jeder Bank ist Schwarz, COLOR00
ist auch der Rand). Über 16 Farben hinaus wird die nächstliegende genommen.

Dadurch hängt eine konvertierte Zeile nur noch von ihren eigenen 160 TIA-Pixeln ab:

1. Pro Puffer liegt im Fast-RAM eine Kopie der TIA-Zeilen. Unveränderte Zeilen werden
   übersprungen, ohne dass etwas berechnet oder geschrieben wird.
2. Geänderte Zeilen werden konvertiert (4 Planes, Paar-Tabelle + 4×4-Byte-Transpose)
   und mit Fast-RAM-Kopien der Bitplane- und Copper-Langwörter verglichen. Nur Langwörter,
   die sich wirklich geändert haben, gehen ins Chip-RAM. Ein bewegter Sprite ändert meist
   nur ein oder zwei Langwörter pro Zeile.

Chip-RAM-Zugriffe sind auf beschleunigten Amigas (PiStorm, Turbokarten) das Teuerste
überhaupt; auf der Testmaschine (Emu68, 68040) sank die Konvertierung damit von 89 auf
9–25 Rasterzeilen pro Frame.

Der Bildwechsel erfolgt über `COP1LC`: Der Copper startet beim nächsten Vertical Blank
mit der neuen Liste, es gibt also kein Tearing. Der sichtbare Bereich des Spiels wird
vertikal zentriert (NTSC: 224 Zeilen ab Rasterzeile $1C, PAL: 256 ab $2C). Änderungen
übernimmt der Emulator erst, wenn sie 30 Frames stabil sind.

## Eingabe und Latenz

Ziel: so wenig Zeit wie möglich zwischen Joystick und Bild.

- **Späte Abfrage:** Der Joystick wird nicht am Frame-Anfang gelesen, sondern in dem
  Moment, in dem das Spiel SWCHA oder INPT4/5 liest (`a26_input_hook`, höchstens einmal
  pro emulierter Zeile). Die Tastatur wird einmal pro Frame übernommen.
- **Frame-Verzögerung:** Auf schnellen Maschinen ist ein Frame nach einem Bruchteil der
  Bildzeit fertig. Statt direkt nach dem Vertical Blank zu starten und dann zu warten,
  startet die Emulation so spät, dass sie kurz vor dem nächsten Vertical Blank fertig
  ist. Die Startzeile folgt dem langsamsten der letzten 64 Frames plus 25 % plus 12
  Zeilen; nach einem verpassten Vertical Blank gibt es für eine Weile mehr Reserve.
  `DELAY=n` setzt die Startzeile fest, `DELAY=0` schaltet das ab.
- **Anzeige:** Das fertige Bild wird über `COP1LC` zum nächsten Vertical Blank
  aktiviert. Zwischen Joystick-Abfrage und Anzeige liegt damit nur der Rest des
  aktuellen Frames.
- **Frameskip** greift nur, wenn ein Vertical Blank verpasst wurde (langsame Amigas).

## Systemübernahme

Standard ist ein systemfreundlicher Modus: Das OS behält seine Interrupts, Timer,
Tastatur und Netzwerk laufen weiter. Der Emulator übernimmt das Display mit
`LoadView(NULL)` und einer eigenen Copper-Liste und läuft mit Task-Priorität 1.

- Vertical Blank: eigener Interrupt-Server (`irq.s`). Er zählt Frames und weckt den
  Emulator-Task per Signal. Er ist in Assembler geschrieben, weil ein Interrupt-Server
  mit gesetztem Z-Flag zurückkehren muss, sonst überspringt Exec alle Server dahinter.
  Der C-Compiler garantiert das nicht, und das hat die Testmaschine zum Absturz gebracht.
- Warten auf Rasterzeilen: Schlafen über `timer.device` (UNIT_MICROHZ), die letzten
  Zeilen wird der Strahl aktiv abgefragt. Andere Tasks bekommen so Rechenzeit.
- Tastatur: `input.device`-Handler mit Priorität 100, der RAWKEY- und RAWMOUSE-Events
  verschluckt, damit die Workbench dahinter nichts abbekommt.
- Zeitmessung: VBL-Zähler plus Strahlposition. Den CIA-B-TOD liest das OS selbst, und
  das zerstört sein Latch.

`KILLOS` ist der alte Modus: Forbid, alle Interrupts und DMA aus, Tastatur über
CIA-Polling (dabei gehen die CIA-Interrupt-Flags des OS verloren, das Netzwerk erholt
sich danach oft lange nicht).

## PAL/NTSC

- Erkennung: gleitender Mittelwert der Scanlines pro Frame. Ab 287 Zeilen gilt PAL,
  darunter NTSC. In den ersten 60 Frames reichen 4 übereinstimmende Frames zum
  Umschalten, danach 16.
- Umschaltung: `BEAMCON0` Bit 5 (nur ECS-Agnus 8372A oder AGA). Die Agnus-ID stammt aus
  `VPOSR` (Bit 5 der ID = ECS). Danach setzt der Emulator Displayfenster (256 bzw. 224
  Zeilen), Palette und Paula-Periode neu.
- Beim Beenden stellt er BEAMCON0 aus `GfxBase->DisplayFlags` wieder her.

## Ton

Der TIA wird zweimal pro Scanline getaktet (31,4 kHz). Pro Scanline wird ein Sample
gespeichert. Paula spielt pro Amiga-Frame einen Puffer mit ca. einer Rasterzeile pro
Sample (Periode ≈ 227). Jeder TIA-Kanal liegt auf zwei Paula-Kanälen (links + rechts),
damit er in der Mitte klingt. Die Puffer werden ohne Interrupts gewechselt: Das neue
`AUDxLC` wird geschrieben, und Paula übernimmt es am Ende des laufenden Puffers. Die
Samples entstehen im Fast-RAM und gehen als Langwörter ins Chip-RAM.

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

| Test-ROM | Anfang (C-CPU) | asm-CPU | + neuer TIA-Renderer | + PC als Zeiger | + direkte TIA-Writes |
|---|---|---|---|---|---|
| `busy_ntsc` (CPU-lastig, ~23 6502-Befehle/Zeile, 2 TIA-Writes/Zeile) | 961.000 | 648.000 | 537.000 | 467.000 | **443.000** |
| `bars_ntsc` (Player-Sprite auf jeder Zeile, wenig CPU) | 1.035.000 | 1.019.000 | 273.000 | 264.000 | **249.000** |

Budget für 60 fps: etwa 130.000–200.000 Befehle pro Frame auf einem 68030 mit 50 MHz
(bei ~4–6 Takten pro Befehl), die Hälfte bei 25 MHz.

### Echte Hardware (A1200, Emu68/PiStorm, 68040, Kickstart 3.2)

Rasterzeilen pro Frame (Budget NTSC: 262), gemessen mit `PROFILE`/`BENCH=300`:

| Spiel | Emulation | Ton | Bild (vorher → jetzt) | Tempo ohne Vsync |
|---|---|---|---|---|
| Demon Attack | 46 | 9 → 5 | 89 → 23 | 108 → 209 fps |
| River Raid | 81 | 5 | 30 → 9–12 | 159 fps |
| Enduro | 72 | 5 | 73 → 25 | 153 fps |

In Echtzeit: konstant 60 fps, die Frame-Verzögerung startet die Emulation im Schnitt
75 Zeilen (~5 ms) nach dem Vertical Blank. Auf dieser Maschine ist das Chip-RAM der
Engpass, nicht die CPU; auf einem echten 68030 ist es umgekehrt, dort muss vor allem
der Kern (CPU + TIA) schneller werden.

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

`busy_ntsc`, pro Frame (~1.050 TIA-Writes, 3 Render-Abschnitte pro Zeile):
- Playfield füllen ~155.000 (≈ 600 pro Zeile). Die Blockschleife selbst braucht ~6 Befehle
  pro 4-Pixel-Block; der Rest sind Aufbau und Ränder je Abschnitt.
- Dispatch-Schleife ~59.000
- TIA-Write-Pfad ~100.000: `tia_write`, `update_to` und `render` je ~33.000;
  `asm_tia_write`, `write_class` und `tia_write_direct` zusammen ~30.000
- Opcodes der Rest

TIA-Writes gehen aus dem asm-Kern direkt an `tia_write`, ohne `bus_write` und ohne
Bankwechsel-Prüfung. Ausnahme sind 3F-Carts, bei denen TIA-Writes die Bank umschalten.

### Nächste Schritte, nach Hebelwirkung geordnet

1. **Hotspot-Seite $1Fxx** nur für $1FE0–$1FFF langsam behandeln. Viele Spiele haben
   Code in der letzten Seite, der im Moment komplett im langsamen Modus läuft.
2. **Frameskip ohne Pixelarbeit:** In übersprungenen Frames nur Kollisionen berechnen.
   Das spart das Playfield-Füllen vollständig und bringt am meisten, sobald Frameskip
   ohnehin nötig ist.
3. **Unveränderte Zeilen erkennen:** Zeilen mit gleichem Registerverlauf wie im
   Vorframe nicht neu zeichnen; den Amiga-seitigen Vergleich für C2P gibt es schon.
   Die Kollisionen müssen dabei weiter stimmen.
4. **Auf echter Hardware messen** (`BENCH=500`, `PROFILE`): qemu zählt Befehle, keine
   Takte. Chip-RAM-Zugriffe und die 256-Byte-Caches des 68030 kann nur echte Hardware
   bzw. WinUAE im Cycle-Exact-Modus zeigen.

## Offene Punkte / nächste Schritte

1. **Echter 68030:** Messung auf echter Hardware oder WinUAE (cycle-exact) fehlt noch.
   Amiberry unter macOS emuliert den 68030 nicht in Echtzeit-Tempo und taugt nur für
   Funktionstests. Danach die Punkte aus „Nächste Schritte“ oben (Hotspot-Seite,
   Frameskip ohne Pixelarbeit).
2. **Genauigkeit:** Auseinanderlaufen einiger Attract-Modi gegenüber gopher2600 klären
   (siehe [Referenzvergleich](#genauigkeit-referenzvergleich)); HMOVE-Artefakte, während
   ein Objekt gerade gezeichnet wird (Missile verschwindet, Ball wird breiter), fehlen
   noch; Verriegelung über das Frame-Ende hinweg; Paddles im Frontend.
3. **PAL-Palette:** Die Zeile für Farbton D ist interpoliert und muss mit Stella
   abgeglichen werden.
4. **Workbench-Start** (Icon, ASL-Requester) fehlt, bisher nur CLI.
5. **Ton bei zu langsamer Emulation:** Paula wiederholt dann den alten Puffer. Besser
   wäre ein Ringpuffer mit Audio-Interrupt. Außerdem driften Paula-Puffer und Frames
   langsam gegeneinander.
