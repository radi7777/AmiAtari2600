#!/usr/bin/env python3
"""check.py - assertions on a26host output (PPM row colours, WAV pitch)."""
import struct
import sys


def ppm_row(path, y):
    data = open(path, "rb").read()
    magic, dims, maxv, px = data.split(b"\n", 3)
    w, h = map(int, dims.split())
    if y < 0:
        y += h
    return [px[(y * w + x) * 3:(y * w + x) * 3 + 3].hex() for x in range(0, w, 2)]


def runs(row):
    out, prev = [], None
    for x, c in enumerate(row):
        if c != prev:
            out.append((x, c))
            prev = c
    return out


def wav_freq(path):
    data = open(path, "rb").read()
    rate = struct.unpack("<I", data[24:28])[0]
    n = (len(data) - 44) // 2
    s = struct.unpack("<%dh" % n, data[44:44 + n * 2])
    mean = sum(s) / n
    rising = sum(1 for i in range(1, n) if s[i - 1] <= mean < s[i])
    return rising * rate / n, max(s) - min(s)


def main():
    kind = sys.argv[1]
    if kind == "row":            # row <ppm> <y> <x:colour> ...
        row = ppm_row(sys.argv[2], int(sys.argv[3]))
        for spec in sys.argv[4:]:
            x, c = spec.split(":")
            if row[int(x)] != c:
                print("FAIL %s: pixel %s is %s, expected %s  (runs: %s)" %
                      (sys.argv[2], x, row[int(x)], c, runs(row)))
                return 1
        return 0
    if kind == "tone":           # tone <wav> <hz> <tolerance%>
        f, amp = wav_freq(sys.argv[2])
        want, tol = float(sys.argv[3]), float(sys.argv[4])
        if amp == 0 or abs(f - want) > want * tol / 100:
            print("FAIL %s: tone %.1f Hz (amp %d), expected %.1f Hz" % (sys.argv[2], f, amp, want))
            return 1
        return 0
    print("unknown check", kind)
    return 2


if __name__ == "__main__":
    sys.exit(main())
