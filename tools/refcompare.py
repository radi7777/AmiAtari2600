#!/usr/bin/env python3
"""
refcompare.py - compare a26host frames with a reference emulator.

    refcompare.py ours_prefix ref_prefix first count [--img out.ppm] [--detail]

Both sides are raw frame dumps ("<lines>\\n" + lines x 160 TIA colour
bytes): ours from `a26host -raw prefix -rawfrom F`, the reference from the
gopher2600-based refdump tool. Frame numbering and the first line of a
frame may differ between emulators, so each of our frames is matched
against nearby reference frames and vertical offsets; the best match is
reported. Pixels in lines that only one side has are ignored.
"""
import os
import re
import sys


def load(path):
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    nl = data.index(b"\n")
    lines = int(data[:nl])
    px = data[nl + 1:]
    return [px[y * 160:(y + 1) * 160] for y in range(lines)]


def compare(a, b, dy):
    """mismatching pixels of a vs b shifted by dy lines, and lines compared"""
    bad = n = 0
    for y in range(len(a)):
        yb = y + dy
        if 0 <= yb < len(b):
            la, lb = a[y], b[yb]
            if la != lb:
                bad += sum(1 for x in range(160) if la[x] != lb[x])
            n += 1
    return bad, n


def palette():
    src = open(os.path.join(os.path.dirname(__file__), "..", "src", "core", "palette.c")).read()
    body = src[src.index("palette_ntsc"):]
    return [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{6})", body)[:128]]


def main():
    args = sys.argv[1:]
    img = None
    detail = "--detail" in args
    if detail:
        args.remove("--detail")
    if "--img" in args:
        i = args.index("--img")
        img = args[i + 1]
        del args[i:i + 2]
    ours, ref, first, count = args[0], args[1], int(args[2]), int(args[3])
    total_bad = 0
    worst = None
    for f in range(first, first + count):
        a = load("%s%04d.raw" % (ours, f))
        if a is None:
            continue
        best = None
        for df in range(-3, 4):
            b = load("%s%04d.raw" % (ref, f + df))
            if b is None:
                continue
            for dy in range(-8, 9):
                bad, n = compare(a, b, dy)
                if n < len(a) - 20:
                    continue
                if best is None or bad < best[0]:
                    best = (bad, df, dy, b)
        if best is None:
            print("frame %d: no reference" % f)
            continue
        bad, df, dy, b = best
        total_bad += bad
        print("frame %d: %6d px differ (ref frame %+d, line offset %+d)" % (f, bad, df, dy))
        if detail and bad:
            for y in range(len(a)):
                if 0 <= y + dy < len(b) and a[y] != b[y + dy]:
                    xs = [x for x in range(160) if a[y][x] != b[y + dy][x]]
                    print("   line %3d x %3d-%3d (%3d px) ours %02x ref %02x" %
                          (y, xs[0], xs[-1], len(xs), a[y][xs[0]], b[y + dy][xs[0]]))
            detail = False
        if worst is None or bad > worst[0]:
            worst = (bad, f, a, b, dy)
    print("total differing pixels: %d" % total_bad)
    if img and worst:
        bad, f, a, b, dy = worst
        pal = palette()
        h = len(a)
        out = bytearray()
        for y in range(h):
            row = bytearray()
            lb = b[y + dy] if 0 <= y + dy < len(b) else bytes(160)
            for part in (a[y], lb):
                for x in range(160):
                    c = pal[part[x] >> 1]
                    row += bytes(((c >> 16) & 255, (c >> 8) & 255, c & 255))
            for x in range(160):
                row += b"\xff\x00\x00" if a[y][x] != lb[x] else b"\x00\x00\x00"
            out += row
        with open(img, "wb") as fp:
            fp.write(b"P6\n480 %d\n255\n" % h)
            fp.write(out)
        print("worst frame %d written to %s (ours | reference | diff)" % (f, img))


if __name__ == "__main__":
    main()
