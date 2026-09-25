# Atari 2600 Emulator für Amiga 68030 — Projektkontext

## Ziel
Nativer Atari-2600-Emulator (Codename: "Stella-Port") für klassisches AmigaOS auf 68030-Hardware (z. B. A1200 mit Blizzard/TK2-Beschleuniger), Zielchipsatz **ECS**. Kein SDL, keine Abstraktionsschichten — direkter Zugriff auf Custom-Chips für maximale Performance. Zielsystem läuft mit ca. 25–50 MHz 68030, optional mit 68882 FPU. Muss **sowohl PAL als auch NTSC** unterstützen.

## Ausgangslage / Referenzprojekte

- **Stella** (https://stella-emu.github.io) ist der aktuelle Referenz-Emulator, aber inzwischen auf C++20 + SDL2 + g++-11/clang++-10 ausgelegt — das ist auf 68k-Toolchains nicht sinnvoll baubar.
- Es existierte bereits **ein historischer Amiga-Port**: Matthew Stroup portierte Stella (Version ~1.1, ca. 1999) auf AmigaOS, veröffentlicht auf Aminet als `misc/emu/Stella.lha`. Dieser Code ist auf 68020/OS3 ausgelegt, ohne SDL2 (SDL2-Umstieg kam erst mit Stella 4.0, ca. 2014).
- Empfehlung: **Vor-SDL2-Quellcode-Basis** (Stella 1.x–3.x) als Ausgangspunkt für den CPU/TIA-Kern prüfen, statt aktuellen Stella-7.0-Stand zu forken. Ggf. alten Stroup-Amiga-Port auf Grafik-/Sound-Routinen hin durchsehen, um Boilerplate zu sparen.

## Architektur-Entscheidung: Kein SDL

SDL erzeugt auf 68030 zu viel Overhead (Event-Loop-Abstraktion, Software-Blitting, unnötige Speicherkopien). Stattdessen:

- **CPU/TIA-Emulationskern**: reines, portables C, kompiliert direkt mit vbcc. Kernstück ist die zyklengenaue Emulation des TIA-Grafikchips pro Scanline (nicht nur der 6507-CPU bei 1,19 MHz) — das ist der eigentliche Rechenaufwand.
- **Grafikausgabe**: kein SDL_Renderer/SDL_Texture. Direktes Schreiben in Amiga-Bitplane-Puffer, Anzeige über `graphics.library` bzw. direkte Custom-Register-Manipulation. Double-Buffering möglichst ohne OS-Calls.
- **Sound**: direkt über Paula (4 DMA-gesteuerte Hardware-Kanäle), eigene kleine Sample-Puffer-Verwaltung statt SDL-Audio-Queue.
- **Timing/Sync**: orientiert an Blitter-/Copper-Timing der Amiga-Hardware, nicht an SDL-Timer-Callbacks.

## Farb- und Auflösungs-Mapping (Atari TIA → Amiga ECS)

- **TIA-Farben**: 128 Farben Gesamtpalette (NTSC) / 104 (PAL), aber nur **4 Farben gleichzeitig pro Scanline** (Background, Playfield, Player0, Player1).
- **Amiga-Farbmodus**: Standard-Lowres reicht locker aus — 5 Bitplanes = 32 gleichzeitige Farben aus 4096 (12-Bit RGB). Kein EHB (64 Farben) oder HAM (4096 gleichzeitig) nötig, da TIA nie mehr als 4 Farben pro Zeile braucht.
- **Auflösung TIA**: 160 sichtbare Farbtakte/Zeile (≈320 "echte" Pixel bei Breitpixel-Darstellung), 192 Zeilen (NTSC) bzw. 228 Zeilen (PAL) sichtbar, bei insgesamt 228 Farbtakten/Zeile (68 horizontaler Blank + 160 sichtbar).
- **Ziel-Screenmode PAL**: 320×256, Lowres, 5 Bitplanes, 50 Hz.
- **Ziel-Screenmode NTSC**: 320×200, Lowres, 5 Bitplanes, 60 Hz.

## PAL/NTSC-Doppelunterstützung (Pflicht)

Projekt muss **beide Regionen unterstützen**, nicht nur eine. Technische Eckpunkte:

- Laufzeitumschaltung zwischen PAL und NTSC ist auf ECS-Amigas mit **8372A "Fat Agnus"** (Rev6-Board) ohne Reboot möglich, über das **BEAMCON0**-Custom-Chip-Register (softwareseitig gesteuertes Timing-Umschalten).
- Hardware-Pin 41 der Agnus (High=PAL, Low=NTSC) ist nur für permanente Boot-Konfiguration relevant, nicht für Runtime-Switching.
- **Mindestvoraussetzung Zielhardware**: 8372A-Agnus (1MB Fat Agnus, ECS). Ältere Agnus-Varianten (8370/8371) sind fest auf einen Modus verdrahtet und scheiden als Zielplattform aus.
- **Geplantes Verhalten**: Beim ROM-Laden Region (PAL/NTSC) aus TIA-Timing-Daten der ROM ableiten (Zeilenzahl/Sync-Muster) und Amiga-Screenmode + BEAMCON0-Timing automatisch passend umschalten — kein manueller Nutzereingriff, kein Reboot.
- Direkter BEAMCON0-Zugriff bevorzugen, kein Umweg über AmigaOS-Screenmode-Requester (passt zur No-SDL/No-OS-Overhead-Philosophie).

## Toolchain

- Compiler: **vbcc**, Target `m68k-amigaos`
- Flags: `-cpu=68030 -fpu=68882` (FPU nur falls vorhanden/optional abfragen)
- Build-Befehl (Grundmuster): `vc +aos68k -cpu=68030 -O2 datei.c -o programm` (Linking via vlink im Hintergrund)
- NDK: AmigaOS NDK 3.1/3.2 (Hyperion) für `hardware/custom.i`, `graphics.library`-Header etc.
- Performance-Feature: vbcc `__reg()`-Pragmas nutzen, um kritische Funktionsparameter (v. a. im Fetch-Decode-Execute-Loop der CPU-Emulation) in Register statt auf den Stack zu legen.

## Offene Punkte / nächste Schritte

1. Passende Vor-SDL2-Stella-Quellcode-Version identifizieren und Lizenz (GPL) beachten.
2. Alten Stroup-Amiga-Port (Aminet) auf brauchbare Grafik-/Sound-Routinen sichten.
3. TIA-Emulationskern isolieren und auf reine C-Portabilität prüfen (keine Plattform-Abhängigkeiten).
4. BEAMCON0-Umschaltlogik prototypisch testen (z. B. anhand des bekannten ntscswitch-Testprogramms als Referenz).
5. ROM-Region-Erkennung implementieren (Ableitung PAL/NTSC aus TIA-Sync-/Zeilendaten).
6. Paula-Sound-Ausgabe für TIA-Audio (2 Kanäle, einfache Wellenformen) konzipieren.
7. Benchmark-Ziel festlegen: volle Framerate (60 Hz NTSC / 50 Hz PAL) auf 68030 bei welcher Taktrate?

## Zielhardware (Referenz)

- Amiga mit ECS-Chipsatz, **8372A Fat Agnus (Rev6)** zwingend für PAL/NTSC-Runtime-Switch
- 68030-Beschleuniger (z. B. Blizzard, TK2), 25–50 MHz
- Optional 68882 FPU
- Bildschirmauflösung: 320×256 (PAL) / 320×200 (NTSC), Lowres, 5 Bitplanes, 32 Farben
