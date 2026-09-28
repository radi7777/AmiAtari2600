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
    for (p = 0; p < 4; p++) {
        u8 b = planes[y * VC_LINE_STRIDE + p * VC_ROW_BYTES + (x >> 2)];
        if (b & (1 << bit)) s |= 1 << p;
        /* the doubled pixel must carry the same bit */
        if (((b >> bit) & 1) != ((b >> (bit - 1)) & 1)) return -1;
    }
    return s + (y & 1) * VC_BANK_COLOURS;   /* plane 5 = bank */
}

/* returns number of wrong pixels */
static long check_frame(const u8 *lines, int nlines, const u32 *pal, long *fallbacks)
{
    u32 moves[VC_MAX_MOVES];
    long bad = 0;
    int y, x, i;
    vc_stat_fallbacks = 0;
    colreg[0] = colreg[16] = 0;
    for (y = 0; y < nlines; y++) {
        u32 pl[VC_LINE_LONGS];
        int n = vc_convert_line(lines + y * VC_WIDTH, y & 1, moves, pl);
        for (i = 0; i < VC_LINE_LONGS; i++) {       /* store big endian, like chip RAM */
            u8 *d = planes + y * VC_LINE_STRIDE + (i / VC_ROW_LONGS) * VC_ROW_BYTES + (i % VC_ROW_LONGS) * 4;
            d[0] = (u8)(pl[i] >> 24); d[1] = (u8)(pl[i] >> 16); d[2] = (u8)(pl[i] >> 8); d[3] = (u8)pl[i];
        }
        if (n > VC_MAX_MOVES) { printf("too many moves\n"); return 1; }
        for (i = 0; i < n; i++) {
            int reg = (int)((moves[i] >> 16) - 0x180) / 2;
            if (reg < 0 || reg >= 32 || (reg & 15) == 0 || (reg >> 4) != (y & 1)) {
                printf("bad move register %d on line %d\n", reg, y);
                return 1;
            }
            colreg[reg] = (u16)moves[i];
        }
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
            int k = 1 + (int)(rnd() % VC_BANK_COLOURS);
            u8 cols[VC_BANK_COLOURS];
            int i;
            for (i = 0; i < k; i++) cols[i] = (u8)((rnd() & 0x7F) << 1);
            if (k == VC_BANK_COLOURS) cols[0] = 0;   /* black is always loaded */
            for (x = 0; x < VC_WIDTH; x++)
                frame[y * VC_WIDTH + x] = cols[(x * k / VC_WIDTH + (int)(rnd() % 3 == 0 ? rnd() % k : 0)) % k];
        }
        bad = check_frame(frame, LINES, palette_ntsc, &fb);
        if (bad || fb) { printf("random frame %d: %ld wrong pixels, %ld fallbacks\n", f, bad, fb); fails++; }
    }
    printf("%s  random lines (<= %d colours/line) reproduce exactly\n", fails ? "FAIL" : "ok  ", VC_BANK_COLOURS);

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
