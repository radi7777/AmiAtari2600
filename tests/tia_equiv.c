/*
 * tia_equiv.c - checks the optimised TIA renderer against the original
 * per-pixel renderer (both compiled from src/core/tia.c with
 * -DA26_TIA_REFERENCE).
 *
 * A pseudo-random but 2600-like stream of TIA writes (object positioning,
 * HMOVE, VDEL, NUSIZ, playfield, colours, collision reads, VSYNC ...) is
 * generated from a seed and fed to the TIA twice: once with the reference
 * renderer, once with the optimised one. Every completed frame and every
 * collision/register read must be identical.
 */
#include <stdio.h>
#include <string.h>
#include "../src/core/tia.h"
#include "../src/core/bus.h"

extern int tia_use_reference;

#define FRAMES 60
static u32 fb32[TIA_FB_LINES * TIA_WIDTH / 4];   /* 4-byte aligned */
#define fb ((u8 *)fb32)
static u8 ab[2 * TIA_MAX_LINES];

static u32 rng;
static u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

typedef struct {
    u32 frame_hash[FRAMES + 8];
    int frames;
    u32 reads[20000];
    int nreads;
} Log;

static u32 hash_fb(void)
{
    u32 h = 2166136261u;
    int i;
    for (i = 0; i < TIA_FB_LINES * TIA_WIDTH; i++) h = (h ^ fb[i]) * 16777619u;
    return h;
}

static void w(u8 reg, u8 val)
{
    a26_cycles += 1 + rnd() % 4;        /* instructions take a few cycles */
    tia_write(reg, val);
}

static void run(int reference, u32 seed, Log *log)
{
    u32 last_frame;
    memset(log, 0, sizeof(*log));
    memset(fb32, 0, sizeof(fb32));
    tia_use_reference = reference;
    rng = seed;
    a26_cycles = 1000;
    tia_init(fb, ab);
    tia_reset();
    last_frame = tia.frame_count;

    while (log->frames < FRAMES) {
        u32 r = rnd() % 100;
        tia_update();
        if (tia.line >= 262) {                  /* end the frame */
            w(0x00, 0x02); w(0x02, 0); w(0x02, 0); w(0x02, 0); w(0x00, 0x00);
        } else if (r < 18) {
            w(0x02, 0);                         /* WSYNC */
            if (rnd() % 3 == 0) w(0x2A, 0);     /* HMOVE at line start */
        } else if (r < 26) {
            w((u8)(0x10 + rnd() % 5), 0);       /* RESP0/1, RESM0/1, RESBL */
        } else if (r < 36) {
            w((u8)(0x1B + rnd() % 2), (u8)rnd());       /* GRP0/1 */
        } else if (r < 42) {
            w((u8)(0x1D + rnd() % 3), (u8)rnd());       /* ENAM0/1, ENABL */
        } else if (r < 47) {
            w((u8)(0x20 + rnd() % 5), (u8)rnd());       /* HMxx */
        } else if (r < 49) {
            w(0x2A, 0);                                 /* HMOVE mid line */
        } else if (r < 52) {
            w((u8)(0x04 + rnd() % 2), (u8)rnd());       /* NUSIZ */
        } else if (r < 62) {
            w((u8)(0x06 + rnd() % 4), (u8)rnd());       /* colours */
        } else if (r < 70) {
            w((u8)(0x0D + rnd() % 3), (u8)rnd());       /* PF0-2 */
        } else if (r < 73) {
            w(0x0A, (u8)rnd());                         /* CTRLPF */
        } else if (r < 76) {
            w((u8)(0x0B + rnd() % 2), (u8)rnd());       /* REFP */
        } else if (r < 79) {
            w((u8)(0x25 + rnd() % 3), (u8)rnd());       /* VDEL */
        } else if (r < 81) {
            w((u8)(0x28 + rnd() % 2), (u8)rnd());       /* RESMP */
        } else if (r < 83) {
            w(0x2B, 0);                                 /* HMCLR */
        } else if (r < 85) {
            w(0x2C, 0);                                 /* CXCLR */
        } else if (r < 87) {
            w(0x01, (u8)((rnd() % 8 == 0) ? 0x02 : 0)); /* VBLANK, mostly off */
        } else if (r < 97) {
            a26_cycles += 1 + rnd() % 4;                /* collision / input read */
            if (log->nreads < 20000)
                log->reads[log->nreads++] = tia_read((u16)(rnd() % 14)) | ((u32)tia.line << 8);
        } else {
            a26_cycles += rnd() % 40;                   /* idle */
        }
        if (tia.frame_count != last_frame) {
            last_frame = tia.frame_count;
            log->frame_hash[log->frames++] = hash_fb();
        }
    }
}

static Log ref, opt;

int main(void)
{
    u32 seed;
    int fails = 0;
    for (seed = 1; seed <= 40; seed++) {
        int i;
        run(1, seed, &ref);
        run(0, seed, &opt);
        for (i = 0; i < FRAMES; i++)
            if (ref.frame_hash[i] != opt.frame_hash[i]) {
                printf("seed %lu: frame %d differs\n", (unsigned long)seed, i);
                fails++;
                break;
            }
        if (ref.nreads != opt.nreads || memcmp(ref.reads, opt.reads, sizeof(u32) * (size_t)ref.nreads)) {
            printf("seed %lu: register reads differ\n", (unsigned long)seed);
            fails++;
        }
    }
    printf("%s  TIA renderer equivalence: 40 seeds x %d frames, pixels + collision reads\n",
           fails ? "FAIL" : "ok  ", FRAMES);
    return fails ? 1 : 0;
}
