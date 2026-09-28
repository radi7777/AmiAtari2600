/*
 * snapimg_test.c - screenshot cache (src/gui/snapimg.c): border cut,
 * reduction, palette and the file round trip.
 *
 *   snapimg_test [tmpfile]          synthetic pictures, exit code 0 = ok
 *   snapimg_test -raw W H in.rgb    converts a raw RGB file, prints stats
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gui/snapimg.h"

typedef struct { int w, h; const unsigned char *rgb; } Src;

static int getrow(void *ctx, int y, unsigned char *out)
{
    const Src *s = (const Src *)ctx;
    memcpy(out, s->rgb + (long)y * s->w * 3, (size_t)s->w * 3);
    return 0;
}

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static void roundtrip(const SnapImg *im, const char *tmp)
{
    SnapImg b;
    CHECK(snapimg_save(tmp, im) == 0, "save");
    CHECK(snapimg_load(tmp, &b) == 0, "load");
    CHECK(b.w == im->w && b.h == im->h && b.ncol == im->ncol, "header");
    CHECK(!memcmp(b.pal, im->pal, (size_t)im->ncol * 3), "palette");
    CHECK(b.pix && !memcmp(b.pix, im->pix, (size_t)im->w * im->h), "pixels");
    snapimg_free(&b);
}

int main(int argc, char **argv)
{
    const char *tmp = argc > 1 && strcmp(argv[1], "-raw") ? argv[1] : "/tmp/snapimg_test.a26i";
    SnapImg im;
    Src s;
    unsigned char *rgb;
    int x, y;
    if (argc == 5 && !strcmp(argv[1], "-raw")) {
        FILE *f = fopen(argv[4], "rb");
        long n;
        s.w = atoi(argv[2]);
        s.h = atoi(argv[3]);
        n = (long)s.w * s.h * 3;
        rgb = (unsigned char *)malloc((size_t)n);
        if (!f || fread(rgb, 1, (size_t)n, f) != (size_t)n) { printf("cannot read %s\n", argv[4]); return 1; }
        fclose(f);
        s.rgb = rgb;
        if (snapimg_from_rgb(&im, s.w, s.h, getrow, &s)) { printf("convert failed\n"); return 1; }
        snapimg_save(tmp, &im);
        f = fopen(tmp, "rb");
        fseek(f, 0, SEEK_END);
        printf("%dx%d -> %dx%d, %d colours, %ld bytes\n", s.w, s.h, im.w, im.h, im.ncol, ftell(f));
        fclose(f);
        roundtrip(&im, tmp);
        return fails != 0;
    }

    /* 512 x 384 with a black border of 24/40 left/right, 30/20 top/bottom,
     * two colour bands inside */
    s.w = 512; s.h = 384;
    rgb = (unsigned char *)calloc((size_t)s.w * s.h, 3);
    for (y = 30; y < 384 - 20; y++)
        for (x = 24; x < 512 - 40; x++) {
            unsigned char *p = rgb + ((long)y * s.w + x) * 3;
            p[0] = y < 150 ? 200 : 40;
            p[1] = 180;
            p[2] = x < 200 ? 30 : 220;
        }
    s.rgb = rgb;
    CHECK(snapimg_from_rgb(&im, s.w, s.h, getrow, &s) == 0, "convert");
    /* content 448 x 334 -> fits 320 x 240: width limited, 320 x 238 */
    CHECK(im.w == 320 && im.h == 238, "size after border cut and reduction");
    CHECK(im.ncol == 4, "4 colours");
    CHECK(im.pix[0] != im.pix[im.w * im.h - im.w], "top and bottom band differ");
    CHECK(im.pix[0] != im.pix[im.w - 1], "left and right band differ");
    roundtrip(&im, tmp);
    snapimg_free(&im);

    /* small picture is not enlarged; noise with > 256 colours */
    s.w = 100; s.h = 80;
    for (y = 0; y < s.h; y++)
        for (x = 0; x < s.w; x++) {
            unsigned char *p = rgb + ((long)y * s.w + x) * 3;
            p[0] = (unsigned char)(x * 37 + y * 11);
            p[1] = (unsigned char)(x * 5 + y * 71);
            p[2] = (unsigned char)(x * y);
        }
    rgb[0] = rgb[1] = rgb[2] = 1;      /* border colour nobody else has */
    CHECK(snapimg_from_rgb(&im, s.w, s.h, getrow, &s) == 0, "convert noise");
    CHECK(im.w <= 100 && im.h <= 80 && im.w >= 99 && im.h >= 79, "not enlarged");
    CHECK(im.ncol >= 2 && im.ncol <= 256, "palette reduced to 256");
    roundtrip(&im, tmp);
    snapimg_free(&im);

    /* all border: kept whole */
    memset(rgb, 0, (size_t)s.w * s.h * 3);
    CHECK(snapimg_from_rgb(&im, s.w, s.h, getrow, &s) == 0, "convert empty");
    CHECK(im.w == 100 && im.h == 80 && im.ncol == 1, "empty picture kept whole");
    roundtrip(&im, tmp);
    snapimg_free(&im);

    /* a damaged file is refused */
    {
        FILE *f = fopen(tmp, "wb");
        fwrite("A26I\1\0\10\0\10\0\1abc\202", 1, 16, f);
        fclose(f);
        CHECK(snapimg_load(tmp, &im) != 0, "truncated file refused");
    }
    free(rgb);
    remove(tmp);
    if (!fails) printf("snapimg ok\n");
    return fails != 0;
}
