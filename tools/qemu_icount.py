#!/usr/bin/env python3
"""
qemu_icount.py - count executed 68k instructions of a program run under
qemu-m68k (Linux user mode). Used to estimate 68030 cost per emulated
frame without real hardware:

    tools/qemu_icount.py build/a26host.m68k rom.bin -frames 10
    -> prints the number of executed 68k instructions

    tools/qemu_icount.py --by-func build/a26host.m68k rom.bin -frames 10
    -> additionally lists the functions with the most instructions
       (needs m68k-linux-gnu-nm)

It logs every executed translation block (-d in_asm,exec,nochain) and
multiplies by the block sizes. Instruction counts are not cycles: on a real
68030 the average is roughly 4-6 clocks per instruction, more when the
256-byte caches miss or chip RAM is accessed.
"""
import collections
import os
import re
import subprocess
import sys
import tempfile


def symbols(binary):
    out = subprocess.run(["m68k-linux-gnu-nm", "-n", binary], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), parts[2]))
    return syms


def count(argv, by_func=False):
    fd, log = tempfile.mkstemp(suffix=".log")
    os.close(fd)
    try:
        subprocess.run(["qemu-m68k", "-cpu", "m68030", "-d", "in_asm,exec,nochain", "-D", log] + argv,
                       check=True, stdout=subprocess.DEVNULL)
        size, cur = {}, None
        execs = collections.Counter()
        with open(log, errors="ignore") as f:
            for line in f:
                if line.startswith("IN:"):
                    cur = None
                    continue
                m = re.match(r"^0x([0-9a-f]+):\s", line)
                if m:
                    if cur is None:
                        cur = int(m.group(1), 16)
                        size[cur] = 0
                    size[cur] += 1
                    continue
                m = re.search(r"Trace \d+: 0x[0-9a-f]+ \[[0-9a-f]+/([0-9a-f]+)/", line)
                if m:
                    execs[int(m.group(1), 16)] += 1
        total = sum(size.get(a, 0) * c for a, c in execs.items())
        if by_func:
            import bisect
            syms = symbols(argv[0])
            addrs = [a for a, _ in syms]
            per = collections.Counter()
            for a, c in execs.items():
                i = bisect.bisect_right(addrs, a) - 1
                per[syms[i][1] if i >= 0 else "?"] += size.get(a, 0) * c
            for name, n in per.most_common(15):
                print("%10d  %5.1f%%  %s" % (n, 100.0 * n / max(total, 1), name))
        return total
    finally:
        os.unlink(log)


def main():
    args = sys.argv[1:]
    by_func = False
    if args and args[0] == "--by-func":
        by_func = True
        args = args[1:]
    if not args:
        print(__doc__)
        return 2
    print(count(args, by_func))
    return 0


if __name__ == "__main__":
    sys.exit(main())
