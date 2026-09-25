# Architektur

```
src/core/      plattformneutraler Emulationskern (C99, auch vbcc-tauglich)
  cpu.c        6507: kompletter NMOS-Befehlssatz, 1 Buszugriff = 1 Zyklus
  bus.c        Adressdekodierung (TIA / RIOT-RAM / RIOT-I/O / Cartridge)
  tia.c        TIA: Video (Catch-up-Rendering), Kollisionen, Audio, Eingänge
  riot.c       6532: 128 Byte RAM, Timer (lazy berechnet), Ports
  cart.c       ROM-Mapping, Bankswitching, Typ-Erkennung
  atari.c      Frame-Schleife, Eingabe-API, PAL/NTSC-Erkennung
  palette.c    NTSC-/PAL-Paletten (24 Bit)
src/amiga/     Amiga-Frontend (AmigaOS-Header, direkter Hardwarezugriff)
  hw.c         System-Takeover/-Restore, Agnus-Erkennung, BEAMCON0, VBL-Sync
  vidconv.c    TIA-Zeile -> Farbregister + Copper-Moves + Bitplanes (portabel, host-getestet)
  video.c      Copper-Listen, Double-Buffer, Displayfenster PAL/NTSC
  audio.c      Paula-Ausgabe
  input.c      Joystick (JOYxDAT/CIA) und Tastatur (CIA-Polling mit Handshake)
  main_amiga.c Kommandozeile, Hauptschleife, Frameskip
src/host/      Testrahmen für Linux/macOS (PPM/WAV-Ausgabe)
tests/         CPU-Test, Test-ROMs (eigener Assembler), Video-Konvertierungstest
tools/         asm6502.py: kleiner 6502-Assembler für Test-ROMs
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

## Offene Punkte / nächste Schritte

1. **Erster echter Build mit vbcc + NDK** auf dem Mac und die nötigen Korrekturen.
2. **Test in einem Emulator** (FS-UAE/Amiberry auf dem Mac, A1200 bzw. A3000 mit
   68030 und ECS), danach auf echter Hardware.
3. **Performance auf dem 68030 messen.** Noch gibt es keine Zahlen. Kandidaten fürs
   Tuning:
   - `vbcc -O2 -speed` gegen `-O3` vergleichen
   - `__reg()` im CPU-Dispatch
   - Opcode-Fetch direkt aus den Cartridge-Segmenten statt über `bus_read`
   - TIA-Pixelschleife in Assembler
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
