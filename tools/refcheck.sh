#!/bin/sh
# refcheck.sh - compare a26host with the gopher2600-based refdump tool
#   REFDUMP=/path/to/refdump sh tools/refcheck.sh [first] [count] rom.bin ...
set -u
cd "$(dirname "$0")/.."
REFDUMP=${REFDUMP:-refdump}
FIRST=$1; COUNT=$2; shift 2
mkdir -p build/ref
for r in "$@"; do
    n=$(basename "$r" .bin)
    rm -f build/ref/${n}_*.raw
    ./build/a26host "$r" -clean -frames $((FIRST + COUNT)) -raw build/ref/${n}_o_ -rawfrom $FIRST -q >/dev/null
    s=$((FIRST - 5)); [ $s -lt 0 ] && s=0
    $REFDUMP "$r" $s $((COUNT + 10)) build/ref/${n}_r_ >/dev/null 2>&1
    printf "%-14s " "$n"
    python3 tools/refcompare.py build/ref/${n}_o_ build/ref/${n}_r_ $FIRST $COUNT --img build/ref/$n.ppm | grep total
done
