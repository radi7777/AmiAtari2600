/*
 * unzip.c - ROMs straight from .zip files (see unzip.h).
 *
 * The inflate part follows RFC 1951 in the simplest form: canonical
 * Huffman codes decoded bit by bit from code counts. That is slow per
 * symbol but an Atari ROM is at most a few dozen KB, so unpacking one
 * takes a fraction of a second even on a 68030.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unzip.h"

/* ---------------------------------------------------------------- CRC32 */
u32 zip_crc32(u32 crc, const u8 *p, u32 n)
{
    static u32 table[256];
    if (!table[1]) {
        u32 c;
        int i, k;
        for (i = 0; i < 256; i++) {
            c = (u32)i;
            for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* -------------------------------------------------------------- inflate */
typedef struct {
    const u8 *in, *inend;
    u32 bitbuf;
    int bitcnt;
    u8 *out;
    u32 outpos, outlen;
    int err;
} Inf;

typedef struct {
    u16 count[16];              /* codes per length */
    u16 symbol[320];            /* symbols ordered by code */
} Huff;

static u32 bits(Inf *s, int n)
{
    u32 v;
    while (s->bitcnt < n) {
        if (s->in >= s->inend) { s->err = 1; return 0; }
        s->bitbuf |= (u32)*s->in++ << s->bitcnt;
        s->bitcnt += 8;
    }
    v = s->bitbuf & ((1UL << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static int decode(Inf *s, const Huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        code |= (int)bits(s, 1);
        if (s->err) return -1;
        if (code - h->count[len] < first) return h->symbol[index + (code - first)];
        index += h->count[len];
        first += h->count[len];
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static int build(Huff *h, const u8 *lengths, int n)
{
    u16 offs[16];
    int i, left = 1;
    for (i = 0; i < 16; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    for (i = 1; i < 16; i++) {          /* over-subscribed set: invalid */
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (u16)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (u16)i;
    return 0;
}

static const u16 len_base[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
                                  35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8  len_extra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 dist_base[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,
                                   769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u8  dist_extra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,
                                    12,12,13,13 };

static int codes(Inf *s, const Huff *lit, const Huff *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return -1;
            s->out[s->outpos++] = (u8)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            u32 len, d;
            sym -= 257;
            if (sym >= 29) return -1;
            len = len_base[sym] + bits(s, len_extra[sym]);
            sym = decode(s, dist);
            if (sym < 0 || sym >= 30) return -1;
            d = dist_base[sym] + bits(s, dist_extra[sym]);
            if (s->err || d > s->outpos || s->outpos + len > s->outlen) return -1;
            while (len--) {
                s->out[s->outpos] = s->out[s->outpos - d];
                s->outpos++;
            }
        }
    }
}

static int fixed(Inf *s)
{
    static Huff lit, dist;
    static int done;
    if (!done) {
        u8 l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build(&lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        build(&dist, l, 30);
        done = 1;
    }
    return codes(s, &lit, &dist);
}

static int dynamic(Inf *s)
{
    static const u8 order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    u8 lengths[320];
    Huff lencode, lit, dist;
    int nlen, ndist, ncode, i;
    nlen = (int)bits(s, 5) + 257;
    ndist = (int)bits(s, 5) + 1;
    ncode = (int)bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return -1;
    memset(lengths, 0, sizeof(lengths));
    for (i = 0; i < ncode; i++) lengths[order[i]] = (u8)bits(s, 3);
    if (s->err || build(&lencode, lengths, 19)) return -1;
    for (i = 0; i < nlen + ndist; ) {
        int sym = decode(s, &lencode), rep;
        u8 v = 0;
        if (sym < 0) return -1;
        if (sym < 16) { lengths[i++] = (u8)sym; continue; }
        if (sym == 16) {
            if (!i) return -1;
            v = lengths[i - 1];
            rep = 3 + (int)bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + (int)bits(s, 3);
        } else {
            rep = 11 + (int)bits(s, 7);
        }
        if (s->err || i + rep > nlen + ndist) return -1;
        while (rep--) lengths[i++] = v;
    }
    if (!lengths[256]) return -1;       /* no end-of-block code */
    if (build(&lit, lengths, nlen) || build(&dist, lengths + nlen, ndist)) {
        /* incomplete sets are allowed (single code); build only rejects
         * over-subscribed ones */
        return -1;
    }
    return codes(s, &lit, &dist);
}

int zip_inflate(const u8 *in, u32 inlen, u8 *out, u32 outlen)
{
    Inf s;
    int last;
    memset(&s, 0, sizeof(s));
    s.in = in;
    s.inend = in + inlen;
    s.out = out;
    s.outlen = outlen;
    do {
        int type;
        last = (int)bits(&s, 1);
        type = (int)bits(&s, 2);
        if (s.err) return -1;
        if (type == 0) {                /* stored */
            u32 len;
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.inend - s.in < 4) return -1;
            len = s.in[0] | (u32)s.in[1] << 8;
            if ((len ^ 0xFFFF) != (s.in[2] | (u32)s.in[3] << 8)) return -1;
            s.in += 4;
            if ((u32)(s.inend - s.in) < len || s.outpos + len > s.outlen) return -1;
            memcpy(s.out + s.outpos, s.in, len);
            s.in += len;
            s.outpos += len;
        } else if (type == 1) {
            if (fixed(&s)) return -1;
        } else if (type == 2) {
            if (dynamic(&s)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return s.outpos == outlen ? 0 : -1;
}

/* ------------------------------------------------------------------ zip */
static u32 le16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static int ends_with(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext), i;
    if (a < b) return 0;
    for (i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != ext[i]) return 0;
    }
    return 1;
}

int zip_is_zip(const char *name)
{
    return ends_with(name, ".zip");
}

int zip_find_rom(const char *path, ZipEntry *e)
{
    FILE *f = fopen(path, "rb");
    u8 tail[1024], *dir = NULL, *p;
    long size, start, i;
    u32 n, dirsize, diroff, k;
    int best = -1;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 22) goto bad;
    /* end of central directory record: usually the last 22 bytes (a zip
     * comment would move it; up to 1 KB of comment is looked through) */
    start = size > (long)sizeof(tail) ? size - (long)sizeof(tail) : 0;
    if (fseek(f, start, SEEK_SET)) goto bad;
    n = (u32)fread(tail, 1, (size_t)(size - start), f);
    for (i = (long)n - 22; i >= 0; i--)
        if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) break;
    if (i < 0) goto bad;
    n = le16(tail + i + 10);                    /* entries */
    dirsize = le32(tail + i + 12);
    diroff = le32(tail + i + 16);
    if (!n || dirsize > 1024UL * 1024UL || (long)(diroff + dirsize) > size) goto bad;
    dir = (u8 *)malloc(dirsize);
    if (!dir || fseek(f, (long)diroff, SEEK_SET) || fread(dir, 1, dirsize, f) != dirsize) goto bad;
    for (p = dir, k = 0; k < n; k++) {
        ZipEntry c;
        u32 nl, xl, cl, score, flags;
        if ((u32)(p - dir) + 46 > dirsize || le32(p) != 0x02014B50UL) goto bad;
        nl = le16(p + 28); xl = le16(p + 30); cl = le16(p + 32);
        if ((u32)(p - dir) + 46 + nl > dirsize) goto bad;
        memset(&c, 0, sizeof(c));
        memcpy(c.name, p + 46, nl < sizeof(c.name) - 1 ? nl : sizeof(c.name) - 1);
        flags = le16(p + 8);
        c.method = (u16)le16(p + 10);
        c.crc = le32(p + 16);
        c.csize = le32(p + 20);
        c.usize = le32(p + 24);
        c.offset = le32(p + 42);
        p += 46 + nl + xl + cl;
        if (c.usize < 2048 || c.usize > 512UL * 1024UL) continue;
        if (c.method != 0 && c.method != 8) continue;
        if (flags & 1) continue;                                /* encrypted */
        score = (ends_with(c.name, ".bin") || ends_with(c.name, ".a26") ||
                 ends_with(c.name, ".rom")) ? 2 : 1;
        if ((int)score > best) { *e = c; best = (int)score; }
    }
    free(dir);
    fclose(f);
    return best > 0 ? 0 : -1;
bad:
    free(dir);
    fclose(f);
    return -1;
}

u8 *zip_extract(const char *path, const ZipEntry *e)
{
    FILE *f = fopen(path, "rb");
    u8 hdr[30], *in = NULL, *out = NULL;
    if (!f) return NULL;
    if (fseek(f, (long)e->offset, SEEK_SET) || fread(hdr, 1, 30, f) != 30 ||
        le32(hdr) != 0x04034B50UL)
        goto bad;
    if (fseek(f, (long)(e->offset + 30 + le16(hdr + 26) + le16(hdr + 28)), SEEK_SET)) goto bad;
    in = (u8 *)malloc(e->csize ? e->csize : 1);
    out = (u8 *)malloc(e->usize);
    if (!in || !out || fread(in, 1, e->csize, f) != e->csize) goto bad;
    if (e->method == 0) {
        if (e->csize != e->usize) goto bad;
        memcpy(out, in, e->usize);
    } else if (zip_inflate(in, e->csize, out, e->usize)) {
        goto bad;
    }
    if (zip_crc32(0, out, e->usize) != e->crc) goto bad;
    free(in);
    fclose(f);
    return out;
bad:
    free(in);
    free(out);
    fclose(f);
    return NULL;
}
