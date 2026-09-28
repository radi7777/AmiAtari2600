#!/usr/bin/env python3
"""
pcprof.py - turn the PROFPC output of A26 into a per-function profile.

    pcprof.py build/amiga/A26.map profile.txt [-n 40]

profile.txt holds the lines printed by `A26 rom PROFPC` ("PC <hex offset>
<count>"); the map comes from `make -f Makefile.amiga map`. Samples are
attributed to the nearest preceding named symbol of the CODE section
(compiler labels like l123 are skipped).
"""
import bisect
import re
import sys


def load_map(path):
    syms = []
    in_code = False
    for line in open(path, errors="replace"):
        if line.startswith("Symbols of "):
            in_code = line.strip() == "Symbols of CODE:"
            continue
        if not in_code:
            continue
        m = re.match(r"\s+0x([0-9a-f]+) (\S+):", line)
        if m and not re.fullmatch(r"l\d+", m.group(2)):
            syms.append((int(m.group(1), 16), m.group(2)))
    syms.sort()
    return syms


def main():
    args = sys.argv[1:]
    top = 40
    if "-n" in args:
        i = args.index("-n")
        top = int(args[i + 1])
        del args[i:i + 2]
    syms = load_map(args[0])
    addrs = [a for a, _ in syms]
    total = other = 0
    per = {}
    for line in open(args[1], errors="replace"):
        m = re.match(r"PCPROF total (\d+) other (\d+)", line)
        if m:
            total, other = int(m.group(1)), int(m.group(2))
            continue
        m = re.match(r"PC ([0-9a-f]+) (\d+)", line)
        if not m:
            continue
        off, n = int(m.group(1), 16), int(m.group(2))
        k = bisect.bisect_right(addrs, off) - 1
        name = syms[k][1] if k >= 0 else "?"
        per[name] = per.get(name, 0) + n
    if not total:
        total = sum(per.values()) + other
    print("%d samples, %.1f%% outside our code (OS, ROM)" % (total, 100.0 * other / max(total, 1)))
    for name, n in sorted(per.items(), key=lambda x: -x[1])[:top]:
        print("%6.2f%%  %7d  %s" % (100.0 * n / total, n, name))


if __name__ == "__main__":
    main()
