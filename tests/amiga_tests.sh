#!/bin/sh
# amiga_tests.sh - run the core test binaries (make -f Makefile.amiga tests)
# on a 68k Amiga emulated by Amiberry and compare C and assembler core.
#
#   sh tests/amiga_tests.sh [rom.bin ...]
#
# Needs Amiberry (AMIBERRY=path to the binary) and an A1200 Kickstart
# (KICK=path). The test directory is mounted as the boot volume; the
# script waits for the results and quits the emulator.
set -u
cd "$(dirname "$0")/.."
AMIBERRY=${AMIBERRY:-/Applications/Amiberry.app/Contents/MacOS/Amiberry}
KICK=${KICK:-$HOME/Documents/Amiberry/Kickstarts/Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200).rom}
CPU=${CPU:-68030}
FRAMES=${FRAMES:-60}
D=build/amiga/run
rm -rf $D && mkdir -p $D/S $D/out
cp build/amiga/tests/* $D/
cp build/6502_functional_test.bin $D/kd.bin
ROMS="build/bars_ntsc.bin build/bars_pal.bin build/bank_f8.bin build/busy_ntsc.bin build/cpu_paths.bin $*"
{
    echo "Stack 200000"
    echo "FailAt 100"
    echo "cputest_asm kd.bin >out/cpu_asm.txt"
    for r in $ROMS; do
        n=$(basename "$r" .bin)
        cp "$r" $D/$n.bin
        echo "a26host_c $n.bin -frames $FRAMES -ppm out/c_$n.ppm -wav out/c_$n.wav -q >out/c_$n.txt"
        echo "a26host_asm $n.bin -frames $FRAMES -ppm out/a_$n.ppm -wav out/a_$n.wav -q >out/a_$n.txt"
    done
    echo "Echo >out/done.txt done"
} > $D/S/Startup-Sequence

ABS=$(cd $D && pwd)
cat > $D/test.uae <<EOC
use_gui=no
kickstart_rom_file=$KICK
chipset=aga
chipset_compatible=A1200
chipmem_size=4
z3mem_size=64
cpu_type=$CPU
cpu_model=$CPU
cpu_compatible=false
cpu_24bit_addressing=false
cpu_speed=max
cycle_exact=false
filesystem2=rw,DH0:test:$ABS,0
uaehf0=dir,rw,DH0:test:$ABS,0
sound_output=none
nr_floppies=0
EOC
"$AMIBERRY" -f $D/test.uae >$D/amiberry.log 2>&1 &
PID=$!
i=0
while [ ! -f $D/out/done.txt ] && [ $i -lt 600 ]; do sleep 1; i=$((i + 1)); done
kill $PID 2>/dev/null
[ -f $D/out/done.txt ] || { echo "FAIL timeout"; exit 1; }

FAILS=0
cat $D/out/cpu_asm.txt
grep -q PASSED $D/out/cpu_asm.txt || FAILS=$((FAILS + 1))
for r in $ROMS; do
    n=$(basename "$r" .bin)
    if cmp -s $D/out/c_$n.ppm $D/out/a_$n.ppm && cmp -s $D/out/c_$n.wav $D/out/a_$n.wav \
       && cmp -s $D/out/c_$n.txt $D/out/a_$n.txt; then
        echo "  ok   $n: C and asm core identical on 68k"
    else
        echo "  FAIL $n: C and asm core differ"; FAILS=$((FAILS + 1))
    fi
    # the big-endian C core must also match the host build
    ./build/a26host "$r" -frames $FRAMES -ppm $D/h_$n.ppm -wav $D/h_$n.wav -q >/dev/null
    cmp -s $D/h_$n.ppm $D/out/c_$n.ppm && cmp -s $D/h_$n.wav $D/out/c_$n.wav \
        || { echo "  FAIL $n: 68k build differs from host build"; FAILS=$((FAILS + 1)); }
done
[ $FAILS -eq 0 ] && echo "ALL AMIGA TESTS PASSED" || echo "$FAILS AMIGA TEST(S) FAILED"
exit $FAILS
