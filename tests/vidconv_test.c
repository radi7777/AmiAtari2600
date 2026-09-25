/*
 * vidconv_test.c - verifies src/amiga/vidconv.c on the host.
 *
 * Every frame is converted to bitplanes + copper colour moves, then
 * decoded again by simulating the copper (moves applied before each line)
 * and the display (5 bitplanes). The result must equal the direct TIA
 * palette lookup for every pixel whenever no fallback was needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/amiga/vidconv.h"
#include "../src/core/palette.h"
#include "../src/core/atari.h"

#define LINES 256

static u8  planes[LINES * VC_LINE_STRIDE];
static u16 colreg[32];

static int decode_pixel(int y, int x)
{
    int bit = 7 - ((2 * x) & 7), p, s = 0;
    for (p = 0; p < VC_PLANES; p++) {
        u8 b = planes[y * VC_LINE_STRIDE + p * VC_ROW_BYTES + (x >> 2)];
        if (b & (1 << bit)) s |= 1 << p;
        /* the doubled pixel must carry the same bit */
        if (((b >> bit) & 1) != ((b >> (bit - 1)) & 1)) return -1;
    }
    return s;
}

/* returns number of wrong pixels */
static long check_frame(const u8 *lines, int nlines, const u32 *pal, long *fallbacks)
{
    static u8 slots[VC_WIDTH];
    VcMove moves[VC_MAX_MOVES];
    long bad = 0;
    int y, x, i;
    vc_begin_frame();
    for (y = 0; y < nlines; y++) {
        int n = vc_map_line(lines + y * VC_WIDTH, slots, moves);
        if (n > VC_MAX_MOVES) { printf("too many moves\n"); return 1; }
        vc_c2p_line(slots, planes + y * VC_LINE_STRIDE);
        for (i = 0; i < n; i++) colreg[moves[i].reg] = moves[i].rgb;
        for (x = 0; x < VC_WIDTH; x++) {
            int s = decode_pixel(y, x);
            u16 want = palette_to_rgb12(pal[lines[y * VC_WIDTH + x] >> 1]);
            if (s < 0 || colreg[s] != want) bad++;
        }
    }
    *fallbacks = (long)vc_stat_fallbacks;
    return bad;
}

static u32 rnd_state = 12345;
static u32 rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 16; }

int main(int argc, char **argv)
{
    static u8 frame[LINES * VC_WIDTH];
    long bad, fb;
    int fails = 0, y, x, f;

    vc_set_palette(palette_ntsc);

    /* 1. random lines with up to VC_MAX_MOVES distinct colours: must be exact */
    for (f = 0; f < 20; f++) {
        for (y = 0; y < LINES; y++) {
            int k = 1 + (int)(rnd() % VC_MAX_MOVES);
            u8 cols[VC_MAX_MOVES];
            int i;
            for (i = 0; i < k; i++) cols[i] = (u8)((rnd() & 0x7F) << 1);
            for (x = 0; x < VC_WIDTH; x++)
                frame[y * VC_WIDTH + x] = cols[(x * k / VC_WIDTH + (int)(rnd() % 3 == 0 ? rnd() % k : 0)) % k];
        }
        bad = check_frame(frame, LINES, palette_ntsc, &fb);
        if (bad || fb) { printf("random frame %d: %ld wrong pixels, %ld fallbacks\n", f, bad, fb); fails++; }
    }
    printf("%s  random lines (<= %d colours/line) reproduce exactly\n", fails ? "FAIL" : "ok  ", VC_MAX_MOVES);

    /* 2. overload: 40 colours per line must degrade gracefully */
    for (y = 0; y < LINES; y++)
        for (x = 0; x < VC_WIDTH; x++)
            frame[y * VC_WIDTH + x] = (u8)(((x / 4 + y) & 0x7F) << 1);
    bad = check_frame(frame, LINES, palette_ntsc, &fb);
    printf("ok    overload: %ld of %d pixels substituted (%ld fallbacks)\n", bad, LINES * VC_WIDTH, fb);

    /* 3. real emulator frames from a ROM */
    if (argc > 1) {
        FILE *fp = fopen(argv[1], "rb");
        static u8 rom[65536];
        u32 size = fp ? (u32)fread(rom, 1, sizeof(rom), fp) : 0;
        int errs = 0;
        if (fp) fclose(fp);
        if (!size || a26_load(rom, size, CART_UNKNOWN)) { printf("FAIL cannot load %s\n", argv[1]); return 1; }
        for (f = 0; f < 60; f++) {
            a26_run_frame();
            bad = check_frame(a26_framebuffer(), LINES, palette_ntsc, &fb);
            if (bad || fb) errs++;
        }
        printf("%s  %s: 60 frames converted exactly\n", errs ? "FAIL" : "ok  ", argv[1]);
        fails += errs;
    }
    return fails ? 1 : 0;
}
