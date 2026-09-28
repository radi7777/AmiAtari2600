#!/bin/sh
# run_tests.sh - regression tests for the portable core.
#   A26HOST / CPUTEST may point to alternative binaries (e.g. qemu-m68k runs).
set -u
cd "$(dirname "$0")/.."
B=build
A26HOST=${A26HOST:-$B/a26host}
CPUTEST=${CPUTEST:-$B/cpu_functional}
VIDTEST=${VIDTEST:-$B/vidconv_test}
TIATEST=${TIATEST:-$B/tia_equiv}
FAILS=0

ok()   { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; FAILS=$((FAILS + 1)); }

echo "== CPU"
KD=$B/6502_functional_test.bin
if [ ! -f $KD ]; then
    curl -sSfL -m 60 -o $KD \
      https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/6502_functional_test.bin \
      || rm -f $KD
fi
if [ -f $KD ]; then
    if $CPUTEST $KD; then ok "Klaus Dormann 6502 functional test"; else fail "6502 functional test"; fi
else
    echo "  skip 6502 functional test (download failed)"
fi

echo "== Test ROMs"
python3 tools/asm6502.py tests/roms/bars_ntsc.asm $B/bars_ntsc.bin || fail "assemble bars_ntsc"
python3 tools/asm6502.py tests/roms/bars_pal.asm $B/bars_pal.bin || fail "assemble bars_pal"
python3 tools/asm6502.py tests/roms/bank_f8.asm $B/bank_f8.bin --size 8192 || fail "assemble bank_f8"
python3 tools/asm6502.py tests/roms/busy_ntsc.asm $B/busy_ntsc.bin || fail "assemble busy_ntsc"
python3 tools/asm6502.py tests/roms/cpu_paths.asm $B/cpu_paths.bin --size 8192 || fail "assemble cpu_paths"

out=$($A26HOST $B/bars_ntsc.bin -frames 60 -ppm $B/bars_ntsc.ppm -wav $B/bars_ntsc.wav -q)
echo "$out" | grep -q "type: 4K  region: NTSC  lines: 262 (avg 262)  visible: 40-231" \
    && ok "NTSC timing: 262 lines, 192 visible" || fail "NTSC timing: $out"
# PF1=$F0 -> pixels 16-31 and 96-111 yellow; P0 %10000001 at 54 and 61 red
python3 tests/check.py row $B/bars_ntsc.ppm 100 \
    15:500084 16:fcfc54 31:fcfc54 32:500084 54:b83232 55:500084 61:b83232 96:fcfc54 111:fcfc54 112:500084 \
    && ok "playfield + player 0 pixel positions" || fail "playfield/player positions"
# AUDC0=4 (div 2), AUDF0=10 -> 31440 / 11 / 2 = ~1429 Hz
python3 tests/check.py tone $B/bars_ntsc.wav 1429 3 && ok "audio pure tone pitch" || fail "audio pitch"

out=$($A26HOST $B/bars_pal.bin -frames 60 -q)
echo "$out" | grep -q "region: PAL  lines: 312" && ok "PAL detected from 312 lines" || fail "PAL detection: $out"

out=$($A26HOST $B/bank_f8.bin -frames 30 -ppm $B/bank_f8.ppm -q)
echo "$out" | grep -q "type: F8" && ok "F8 autodetected" || fail "F8 detection: $out"
python3 tests/check.py row $B/bank_f8.ppm 50 0:2d32b8 159:2d32b8 && ok "F8 bank switch" || fail "F8 bank switch"

out=$($A26HOST $B/cpu_paths.bin -frames 31 -ppm $B/cpu_paths.ppm -q)
echo "$out" | grep -q "lines: 262" || fail "cpu_paths timing: $out"
# P0: loop with a page-crossing branch (pixel 39 without the penalty),
# P1: loop running in RIOT RAM, blue background: set by bank 0 after a
# bankswitch from code in page $1Fxx
python3 tests/check.py row $B/cpu_paths.ppm 100 0:2d32b8 41:2d32b8 42:5cba5c 43:2d32b8 50:2d32b8 51:b83232 52:2d32b8 \
  && ok "branch page-crossing penalty, slow-page code, bankswitch from \$1Fxx" \
  || fail "cpu_paths"

echo "== TIA renderer (optimised vs. reference per-pixel renderer)"
if $TIATEST; then ok "tia_equiv"; else fail "tia_equiv"; fi
if [ -x $B/tia_check ] && [ "$TIATEST" = "$B/tia_equiv" ]; then
    out=$($B/tia_check | grep -c stale)
    [ "$out" = "0" ] && ok "TIA derived state always consistent" || fail "tia_check: $out stale states"
fi

echo "== Amiga video conversion (copper palette + c2p, simulated)"
if $VIDTEST $B/bars_ntsc.bin; then ok "vidconv"; else fail "vidconv"; fi

if [ -x $B/unzip_test ] && command -v python3 >/dev/null; then
    echo "== ROMs from zip files (stored, deflate levels 1 and 9, zip -9)"
    Z=$B/ziptest
    rm -rf $Z; mkdir -p $Z
    zok=1
    for r in $B/*.bin roms/*.bin; do
        [ -f "$r" ] || continue
        n=$(basename "$r" .bin)
        python3 - "$r" "$Z/$n" <<'PY' || zok=0
import sys, zipfile
src, out = sys.argv[1], sys.argv[2]
for tag, method, level in (("s", zipfile.ZIP_STORED, None), ("d1", zipfile.ZIP_DEFLATED, 1),
                           ("d9", zipfile.ZIP_DEFLATED, 9)):
    with zipfile.ZipFile("%s_%s.zip" % (out, tag), "w", method, compresslevel=level) as z:
        z.writestr("readme.txt", "not a rom")
        z.write(src, arcname="Game (USA).bin")
PY
        if command -v zip >/dev/null; then
            (cp "$r" "$Z/$n.a26" && cd $Z && zip -q -9 "${n}_z.zip" "$n.a26" && rm "$n.a26") || zok=0
        fi
        for zf in $Z/${n}_*.zip; do
            $B/unzip_test "$zf" "$r" || zok=0
        done
    done
    [ $zok = 1 ] && ok "unzip" || fail "unzip"
fi

if [ -x $B/snapimg_test ]; then
    echo "== GUI screenshot cache"
    if $B/snapimg_test $B/snapimg_test.a26i >/dev/null; then ok "snapimg"; else fail "snapimg"; fi
fi

if [ -d roms ]; then
    echo "== Local ROMs (roms/*.bin, not in git)"
    for r in roms/*.bin roms/*.a26; do
        [ -f "$r" ] || continue
        $A26HOST "$r" -frames 300 -q || fail "$r"
    done
fi

echo
if [ $FAILS -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "$FAILS TEST(S) FAILED"; fi
exit $FAILS
