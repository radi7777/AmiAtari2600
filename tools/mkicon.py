#!/usr/bin/env python3
"""
mkicon.py - PNG -> Amiga tool or drawer icon (.info), standard library only.

The icon carries two images:
  - the classic planar image (2 planes, Workbench colours 0 = background,
    1 = black) for OS 2.x/3.0/3.1,
  - an OS 3.5-style colour image (FORM ICON with FACE + IMAG, RLE) that
    icon.library 44+ (OS 3.5, 3.9, 3.2) shows instead.

    python3 tools/mkicon.py res/AmiAtari2600.png build/amiga/AmiAtari2600.info
    python3 tools/mkicon.py --drawer res/AmiAtari2600.png build/dist/AmiAtari2600.info
"""
import struct
import sys
import zlib

STACK = 32768


def read_png(path):
    """8-bit RGB/RGBA/grey/palette PNG, not interlaced -> w, h, [(r,g,b,a)]"""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('not a PNG')
    pos, idat, plte, trns = 8, b'', None, None
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b'IHDR':
            w, h, bits, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
            if bits != 8 or inter:
                raise SystemExit('only 8-bit, non-interlaced PNGs')
        elif typ == b'PLTE':
            plte = body
        elif typ == b'tRNS':
            trns = body
        elif typ == b'IDAT':
            idat += body
    raw = zlib.decompress(idat)
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(line)
        prev = line
    px = []
    for line in rows:
        for x in range(w):
            v = line[x * bpp:(x + 1) * bpp]
            if ctype == 0:
                px.append((v[0], v[0], v[0], 255))
            elif ctype == 2:
                px.append((v[0], v[1], v[2], 255))
            elif ctype == 3:
                k = v[0]
                a = trns[k] if trns and k < len(trns) else 255
                px.append((plte[k * 3], plte[k * 3 + 1], plte[k * 3 + 2], a))
            elif ctype == 4:
                px.append((v[0], v[0], v[0], v[1]))
            else:
                px.append(tuple(v))
    return w, h, px


def quantize(px, maxcol):
    """index 0 = transparent, then up to maxcol-1 colours (median cut)"""
    opaque = [p[:3] for p in px if p[3] >= 128]
    boxes = [sorted(set(opaque))]
    while len(boxes) < maxcol - 1:
        boxes.sort(key=len)
        box = boxes.pop()
        if len(box) < 2:
            boxes.append(box)
            break
        ch = max(range(3), key=lambda c: max(p[c] for p in box) - min(p[c] for p in box))
        box.sort(key=lambda p: p[ch])
        boxes += [box[:len(box) // 2], box[len(box) // 2:]]
    pal = [(0, 0, 0)]
    for box in boxes:
        pal.append(tuple(sum(p[c] for p in box) // len(box) for c in range(3)))

    def near(c):
        return min(range(1, len(pal)),
                   key=lambda k: sum((pal[k][j] - c[j]) ** 2 for j in range(3)))
    cache = {}
    idx = []
    for p in px:
        if p[3] < 128:
            idx.append(0)
        else:
            if p[:3] not in cache:
                cache[p[:3]] = near(p[:3])
            idx.append(cache[p[:3]])
    return pal, idx


class Bits:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, v, bits):
        for b in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> b) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc, self.n = 0, 0

    def done(self):
        if self.n:
            self.out.append(self.acc << (8 - self.n))
        return bytes(self.out)


def rle(values, depth):
    """ByteRun1 on a bit stream: 8-bit control, depth-bit values"""
    bs, i, n = Bits(), 0, len(values)
    while i < n:
        run = 1
        while i + run < n and run < 128 and values[i + run] == values[i]:
            run += 1
        if run >= 2:
            bs.put(257 - run, 8)
            bs.put(values[i], depth)
            i += run
            continue
        j = i
        while j < n and j - i < 128 and not (j + 1 < n and values[j + 1] == values[j]):
            j += 1
        j = max(j, i + 1)
        bs.put(j - i - 1, 8)
        for v in values[i:j]:
            bs.put(v, depth)
        i = j
    return bs.done()


def chunk(tag, body):
    return tag + struct.pack('>I', len(body)) + body + (b'\0' if len(body) & 1 else b'')


def glow(w, h, pal, idx):
    depth = max(1, (len(pal) - 1).bit_length())
    img = rle(idx, depth)
    palbytes = bytes(c for rgb in pal for c in rgb)
    pdat = rle(list(palbytes), 8)
    face = struct.pack('>BBBBH', w - 1, h - 1, 0, 0x11, len(palbytes) - 1)
    imag = struct.pack('>BBBBBBHH', 0, len(pal) - 1, 3, 1, 1, depth,
                       len(img) - 1, len(pdat) - 1) + img + pdat
    body = b'ICON' + chunk(b'FACE', face) + chunk(b'IMAG', imag)
    return chunk(b'FORM', body)


def planar(w, h, idx, pal):
    """classic image: dark pixels -> colour 1 (black), light -> 2 (white)"""
    words = (w + 15) // 16
    planes = [bytearray(words * 2 * h) for _ in range(2)]
    for y in range(h):
        for x in range(w):
            k = idx[y * w + x]
            if not k:
                continue
            r, g, b = pal[k]
            c = 2 if r * 3 + g * 6 + b > 1400 else 1
            o = y * words * 2 + x // 8
            for p in range(2):
                if c >> p & 1:
                    planes[p][o] |= 0x80 >> (x & 7)
    image = struct.pack('>hhhhhIBBI', 0, 0, w, h, 2, 1, 3, 0, 0)
    return image + bytes(planes[0]) + bytes(planes[1])


WBDRAWER, WBTOOL = 2, 3


def diskobject(w, h, tooltypes, typ):
    gadget = struct.pack('>IhhhhHHHIIIIIHI', 0, 0, 0, w, h + 1, 4, 3, 1,
                         1, 0, 0, 0, 0, 0, 1)
    return struct.pack('>HH', 0xE310, 1) + gadget + struct.pack(
        '>BBIIiiIII', typ, 0, 0, 1 if tooltypes else 0,
        -0x80000000, -0x80000000, 1 if typ == WBDRAWER else 0, 0,
        STACK if typ == WBTOOL else 0)


def drawerdata():
    """NewWindow for the drawer window + CurrentX/Y (the OS 2+ part,
    flags and view modes, follows the images)"""
    return struct.pack('>hhhhBBIIIIIIIhhHHH', 60, 40, 360, 160, 255, 255, 0, 0,
                       0, 0, 0, 0, 0, 90, 40, 0xFFFF, 0xFFFF, 1) + struct.pack('>ii', 0, 0)


def strings(lst):
    out = struct.pack('>I', (len(lst) + 1) * 4)
    for s in lst:
        b = s.encode('latin-1') + b'\0'
        out += struct.pack('>I', len(b)) + b
    return out


def main():
    args = sys.argv[1:]
    drawer = '--drawer' in args
    if drawer:
        args.remove('--drawer')
    if len(args) != 2:
        raise SystemExit(__doc__)
    w, h, px = read_png(args[0])
    if w > 256 or h > 256:
        raise SystemExit('icon too big')
    pal, idx = quantize(px, 32)
    tooltypes = []
    data = diskobject(w, h, tooltypes, WBDRAWER if drawer else WBTOOL)
    if drawer:
        data += drawerdata()
    data += planar(w, h, idx, pal)
    if tooltypes:
        data += strings(tooltypes)
    if drawer:
        data += struct.pack('>IH', 0, 0)    # dd_Flags, dd_ViewModes: defaults
    data += glow(w, h, pal, idx)
    open(args[1], 'wb').write(data)
    print('%s: %dx%d, %d colours, %d bytes' % (args[1], w, h, len(pal) - 1, len(data)))


if __name__ == '__main__':
    main()
