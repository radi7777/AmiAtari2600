/*
 * snapimg.c - screenshots prepared once for quick display (see snapimg.h).
 *
 * File format (big endian):
 *   "A26I" version(1) w(2) h(2) ncol(2) palette(ncol * 3)
 *   PackBits data of the w * h indices: n = 0..127 -> n + 1 literal bytes,
 *   n = 129..255 -> the next byte 257 - n times.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snapimg.h"

#define VERSION 1

/* is (r,g,b) visibly different from the border colour? */
static int differs(const unsigned char *p, const unsigned char *b)
{
    int dr = p[0] - b[0], dg = p[1] - b[1], db = p[2] - b[2];
    return dr * dr + dg * dg + db * db > 3 * 24 * 24;
}

/* palette of rgb (n pixels), colours masked with mask; -1 if > 256 */
static int build_palette(SnapImg *im, const unsigned char *rgb, long n, unsigned mask)
{
    enum { HASH = 4096 };
    static long key[HASH];              /* rgb + 1, 0 = free */
    static unsigned char idx[HASH];
    long i;
    memset(key, 0, sizeof(key));
    im->ncol = 0;
    for (i = 0; i < n; i++, rgb += 3) {
        unsigned r = rgb[0] & mask, g = rgb[1] & mask, b = rgb[2] & mask;
        long k = ((long)r << 16 | g << 8 | b) + 1;
        unsigned h = (unsigned)((k * 2654435761UL) >> 20) & (HASH - 1);
        while (key[h] && key[h] != k) h = (h + 1) & (HASH - 1);
        if (!key[h]) {
            if (im->ncol == 256) return -1;
            key[h] = k;
            idx[h] = (unsigned char)im->ncol;
            im->pal[im->ncol * 3] = (unsigned char)r;
            im->pal[im->ncol * 3 + 1] = (unsigned char)g;
            im->pal[im->ncol * 3 + 2] = (unsigned char)b;
            im->ncol++;
        }
        im->pix[i] = idx[h];
    }
    return 0;
}

int snapimg_from_rgb(SnapImg *im, int w, int h, SnapGetRow getrow, void *ctx)
{
    static const unsigned masks[] = { 0xFF, 0xF8, 0xF0, 0xE0, 0xC0 };
    unsigned char *row, *rgb, border[3];
    int x, y, x0 = w, x1 = -1, y0 = h, y1 = -1, cw, ch, dw, dh, prev = -1, m;
    memset(im, 0, sizeof(*im));
    if (w <= 0 || h <= 0) return -1;
    row = (unsigned char *)malloc((size_t)w * 3);
    if (!row) return -1;
    /* the border has the colour of the top left pixel */
    for (y = 0; y < h; y++) {
        if (getrow(ctx, y, row)) { free(row); return -1; }
        if (y == 0) memcpy(border, row, 3);
        for (x = 0; x < w; x++)
            if (differs(row + x * 3, border)) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                y1 = y;
            }
    }
    if (x1 < x0) { x0 = 0; x1 = w - 1; y0 = 0; y1 = h - 1; }
    cw = x1 - x0 + 1;
    ch = y1 - y0 + 1;
    /* fit into MAXW x MAXH, never enlarged */
    dw = cw; dh = ch;
    if (dw > SNAPIMG_MAXW) { dh = dh * SNAPIMG_MAXW / dw; dw = SNAPIMG_MAXW; }
    if (dh > SNAPIMG_MAXH) { dw = dw * SNAPIMG_MAXH / dh; dh = SNAPIMG_MAXH; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    rgb = (unsigned char *)malloc((size_t)dw * dh * 3);
    im->pix = (unsigned char *)malloc((size_t)dw * dh);
    if (!rgb || !im->pix) { free(row); free(rgb); snapimg_free(im); return -1; }
    for (y = 0; y < dh; y++) {
        int sy = y0 + (int)((long)y * ch / dh);
        if (sy != prev && getrow(ctx, sy, row)) { free(row); free(rgb); snapimg_free(im); return -1; }
        prev = sy;
        for (x = 0; x < dw; x++)
            memcpy(rgb + ((long)y * dw + x) * 3, row + (x0 + (long)x * cw / dw) * 3, 3);
    }
    free(row);
    im->w = dw;
    im->h = dh;
    /* exact colours if at most 256, else fewer bits per channel */
    for (m = 0; m < 5; m++)
        if (!build_palette(im, rgb, (long)dw * dh, masks[m])) break;
    free(rgb);
    if (m == 5) { snapimg_free(im); return -1; }
    return 0;
}

static void put16(FILE *f, int v)
{
    fputc((v >> 8) & 255, f);
    fputc(v & 255, f);
}

static int get16(FILE *f)
{
    int a = fgetc(f), b = fgetc(f);
    return (a < 0 || b < 0) ? -1 : a << 8 | b;
}

int snapimg_save(const char *path, const SnapImg *im)
{
    FILE *f = fopen(path, "wb");
    long n = (long)im->w * im->h, i = 0;
    if (!f) return -1;
    fwrite("A26I", 1, 4, f);
    fputc(VERSION, f);
    put16(f, im->w);
    put16(f, im->h);
    put16(f, im->ncol);
    fwrite(im->pal, 1, (size_t)im->ncol * 3, f);
    while (i < n) {
        long run = 1, lit;
        while (i + run < n && run < 128 && im->pix[i + run] == im->pix[i]) run++;
        if (run >= 3) {
            fputc(257 - run, f);
            fputc(im->pix[i], f);
            i += run;
            continue;
        }
        /* literals up to the next run of 3 */
        for (lit = 1; i + lit < n && lit < 128; lit++)
            if (i + lit + 2 < n && im->pix[i + lit] == im->pix[i + lit + 1] &&
                im->pix[i + lit] == im->pix[i + lit + 2])
                break;
        fputc((int)lit - 1, f);
        fwrite(im->pix + i, 1, (size_t)lit, f);
        i += lit;
    }
    if (fclose(f)) return -1;
    return 0;
}

int snapimg_load(const char *path, SnapImg *im)
{
    FILE *f = fopen(path, "rb");
    char magic[4];
    long n, i = 0;
    memset(im, 0, sizeof(*im));
    if (!f) return -1;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "A26I", 4) || fgetc(f) != VERSION)
        goto bad;
    im->w = get16(f);
    im->h = get16(f);
    im->ncol = get16(f);
    if (im->w <= 0 || im->h <= 0 || im->w > 1024 || im->h > 1024 ||
        im->ncol <= 0 || im->ncol > 256)
        goto bad;
    if (fread(im->pal, 1, (size_t)im->ncol * 3, f) != (size_t)im->ncol * 3) goto bad;
    n = (long)im->w * im->h;
    im->pix = (unsigned char *)malloc((size_t)n);
    if (!im->pix) goto bad;
    while (i < n) {
        int c = fgetc(f);
        if (c < 0) goto bad;
        if (c < 128) {
            long len = c + 1;
            if (i + len > n || fread(im->pix + i, 1, (size_t)len, f) != (size_t)len) goto bad;
            i += len;
        } else if (c > 128) {
            long len = 257 - c;
            int v = fgetc(f);
            if (v < 0 || i + len > n) goto bad;
            memset(im->pix + i, v, (size_t)len);
            i += len;
        }
    }
    for (i = 0; i < n; i++)
        if (im->pix[i] >= im->ncol) goto bad;
    fclose(f);
    return 0;
bad:
    fclose(f);
    snapimg_free(im);
    return -1;
}

void snapimg_free(SnapImg *im)
{
    free(im->pix);
    im->pix = NULL;
    im->w = im->h = im->ncol = 0;
}
