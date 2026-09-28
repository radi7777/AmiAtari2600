/*
 * tia.c - TIA emulation.
 *
 * Video is rendered lazily ("catch-up"): before any register access the
 * beam is advanced to the current colour clock (a26_cycles * 3) and the
 * pixels in between are drawn with the *old* register state. This gives
 * colour-clock accurate mid-line register changes without having to run
 * the TIA in lock-step with the CPU.
 *
 * Object graphics use precomputed mask tables (in the spirit of early
 * Stella): for every object a pointer into a 320-entry table is kept so
 * that mask[x] tells whether/which graphics bit is visible at pixel x.
 *
 * Rendering a span works in two passes (see render()):
 *   1. playfield + background in 4-pixel blocks (the playfield only
 *      changes every 4 pixels),
 *   2. the objects are "stamped" on top: for every active object only its
 *      pixel runs (copies x width, derived from the mask tables) get the
 *      full priority/collision evaluation. Everywhere else the result is by
 *      definition playfield or background without collisions.
 * Writes that have no visible effect (HMxx, HMCLR, unchanged values) do
 * not advance the beam at all. The original per-pixel renderer is kept as
 * a reference (A26_TIA_REFERENCE) and tests/tia_equiv.c checks that both
 * produce identical pixels and collisions.
 *
 * Audio is clocked twice per scanline (31.4 kHz); one summed sample per
 * scanline is stored (~15.7 kHz), which maps 1:1 to one Paula sample per
 * Amiga raster line.
 */
#include <string.h>
#ifdef A26_TRACE
#include <stdio.h>
int tia_trace;
#endif
#include "tia.h"
#include "bus.h"

Tia tia;

/* ---- lookup tables -------------------------------------------------- */

static u8  player_mask[2][8][320];  /* [no main copy][nusiz] bit mask (0x80 = first pixel) */
static u8  missile_mask[8][4][320]; /* 0/1 */
static u8  ball_mask[4][320];
static u32 pf_mask[2][TIA_WIDTH];   /* [reflect][x] -> bit in tia.pf */
static u8  reverse_bits[256];
static u8  prio_table[2][64];       /* [pfp][objects] -> colour index */
static u16 coll_table[64];
static int tables_ready;

/* pixel runs of each mask table: (offset, length) pairs, offset relative to
 * the object position. At most 3 copies -> 3 runs. */
#define MAX_RUNS 3
static u8  player_runs[2][8][MAX_RUNS * 2], player_nruns[2][8];
static u8  missile_runs[8][4][MAX_RUNS * 2], missile_nruns[8][4];
static u8  ball_runs[4][2], ball_nruns[4];

/* masks of locked missiles/ball (M0, M1, BL), absolute x, built on demand */
static u8  lock_mask[3][320];
#define LOCK_RUNS 8             /* 3 copies, each may wrap around the edge */
static u8  lock_runs[3][LOCK_RUNS * 2];

#ifdef A26_TIA_REFERENCE
int tia_use_reference;          /* 1 = original per-pixel renderer */
#endif

/* object bits used for the 6-bit "enabled" index */
#define O_PF 0x01
#define O_BL 0x02
#define O_M0 0x04
#define O_M1 0x08
#define O_P0 0x10
#define O_P1 0x20

/* colour indices */
#define C_BK 0
#define C_PF 1
#define C_BL 2
#define C_P0 3
#define C_P1 4

/* HMOVE displacement in pixels (+ = right) by HMxx nibble and the CPU
 * cycle of the HMOVE write within the line (0..75). Measured from
 * gopher2600 with tools/gen_hmove_rom.py; identical for all objects.
 * Cycles 0-2 behave like 3, 75 like 74. */
static const s8 hmove_disp[16][76] = {
    {   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,  -1,  -2,  -2,  -3,  -4,  -5,  -5,  -6,  -7,  -8,  -8,  -8 },
    {  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,   0,   1,   1,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,  -1,  -2,  -3,  -3,  -4,  -5,  -6,  -6,  -7,  -8,  -9,  -9,  -9 },
    {  -2,  -2,  -2,  -2,  -2,  -2,  -2,  -2,  -2,  -2,  -2,  -1,   0,   1,   1,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,  -1,  -1,  -2,  -3,  -4,  -4,  -5,  -6,  -7,  -7,  -8,  -9, -10, -10, -10 },
    {  -3,  -3,  -3,  -3,  -3,  -3,  -3,  -3,  -3,  -2,  -2,  -1,   0,   1,   1,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,  -1,  -2,  -2,  -3,  -4,  -5,  -5,  -6,  -7,  -8,  -8,  -9, -10, -11, -11, -11 },
    {  -4,  -4,  -4,  -4,  -4,  -4,  -4,  -4,  -3,  -2,  -2,  -1,   0,   1,   1,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,  -1,  -2,  -3,  -3,  -4,  -5,  -6,  -6,  -7,  -8,  -9,  -9, -10, -11, -12, -12, -12 },
    {  -5,  -5,  -5,  -5,  -5,  -5,  -5,  -4,  -3,  -2,  -2,  -1,   0,   1,   1,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
       -1,  -1,  -2,  -3,  -4,  -4,  -5,  -6,  -7,  -7,  -8,  -9, -10, -10, -11, -12, -13, -13, -13 },
    {  -6,  -6,  -6,  -6,  -6,  -5,  -5,  -4,  -3,  -2,  -2,  -1,   0,   1,   1,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  -1,
       -2,  -2,  -3,  -4,  -5,  -5,  -6,  -7,  -8,  -8,  -9, -10, -11, -11, -12, -13, -14, -14, -14 },
    {  -7,  -7,  -7,  -7,  -6,  -5,  -5,  -4,  -3,  -2,  -2,  -1,   0,   1,   1,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  -1,  -2,
       -3,  -3,  -4,  -5,  -6,  -6,  -7,  -8,  -9,  -9, -10, -11, -12, -12, -13, -14, -15, -15, -15 },
    {   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
        8,   8,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },
    {   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,
        7,   7,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  -1,  -1,  -1 },
    {   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,
        6,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  -1,  -2,  -2,  -2 },
    {   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  -1,  -2,  -3,  -3,  -3 },
    {   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  -1,  -1,  -2,  -3,  -4,  -4,  -4 },
    {   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  -1,  -2,  -2,  -3,  -4,  -5,  -5,  -5 },
    {   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  -1,  -2,  -3,  -3,  -4,  -5,  -6,  -6,  -6 },
    {   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   3,   4,   4,
        5,   6,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,  -1,  -1,  -2,  -3,  -4,  -4,  -5,  -6,  -7,  -7,  -7 }
};

/* The model behind the table (it reproduces every entry): HMOVE written in
 * CPU cycle w starts a ripple counter that ticks every 4 colour clocks from
 * t0 = 3w + 2 rounded up to a multiple of 4 (relative to the line start).
 * An object gets an extra clock on each tick until the tick number equals
 * its count v = HM nibble ^ 8. An extra clock moves the object one pixel
 * left if it falls into the horizontal blank: before clock 70 on a line
 * with the HMOVE blank, before 68 otherwise, at 224 or later (end of the
 * line), or in the next line's blank. The HMOVE blank itself shifts the
 * objects 8 pixels right. Used for HMxx writes during the ripple. */
static int hmove_t0(int w)
{
    return ((3 * w + 2 + 3) / 4) * 4;
}

static int hmove_calc(int w, int count)
{
    int blank = w <= 20, t0 = hmove_t0(w), k, moved = 0;
    for (k = 0; k < count; k++) {
        int t = t0 + 4 * k;
        if (t < TIA_LINE_CC) {
            if (blank ? t < 70 : (t < TIA_HBLANK || t >= 224)) moved++;
        } else if (t - TIA_LINE_CC < TIA_HBLANK) {
            moved++;
        }
    }
    return (blank ? 8 : 0) - moved;
}

/* Drawing of a missile or the ball that is "locked" in HMOVE (an HMxx
 * write during the ripple skipped its stop value, see Cosmic Ark): it
 * gets an extra clock every 4 colour clocks, which moves it 17 pixels left
 * per line and changes its shape depending on its position modulo 4.
 * [size][pos & 3] -> first pixel offset, width. Measured with gopher2600. */
static const s8 lock_shape[4][4][2] = {
    { { 0, 0 }, { -1, 2 }, { 0, 1 }, { 0, 1 } },     /* 1 pixel */
    { { 0, 2 }, { -1, 3 }, { 0, 2 }, { 0, 1 } },     /* 2 pixels */
    { { 0, 4 }, { -1, 5 }, { 0, 4 }, { 0, 4 } },     /* 4 pixels */
    { { 0, 8 }, { -1, 9 }, { 0, 8 }, { 0, 8 } }      /* 8 pixels */
};

/* copy offsets for NUSIZ modes 0..7 */
static const u8 nusiz_copies[8][3] = {
    { 1, 0, 0 },    /* 0: one copy          (bit flags: copy at 0/16/32/64) */
    { 1, 1, 0 },    /* 1: two copies close  */
    { 1, 0, 1 },    /* 2: two copies medium */
    { 1, 1, 1 },    /* 3: three copies close */
    { 1, 0, 0 },    /* 4: two copies wide (handled below) */
    { 1, 0, 0 },    /* 5: double size */
    { 1, 0, 1 },    /* 6: three copies medium (handled below) */
    { 1, 0, 0 }     /* 7: quad size */
};

static int copy_at(int mode, int offset)
{
    switch (offset) {
    case 0:  return 1;
    case 16: return nusiz_copies[mode][1];
    case 32: return nusiz_copies[mode][2];
    case 64: return mode == 4 || mode == 6;
    }
    return 0;
}

/* runs of non-zero entries in mask[0..159] -> (offset, length) pairs */
static u8 find_runs(const u8 *mask, u8 *runs)
{
    int k = 0, n = 0;
    while (k < TIA_WIDTH) {
        if (mask[k]) {
            int start = k;
            while (k < TIA_WIDTH && mask[k]) k++;
            if (n < MAX_RUNS) {
                runs[n * 2] = (u8)start;
                runs[n * 2 + 1] = (u8)(k - start);
            }
            n++;
        } else {
            k++;
        }
    }
    return (u8)n;
}

static void build_tables(void)
{
    int mode, x, i, size, pfp, obj;

    for (i = 0; i < 256; i++) {
        int r = 0, b;
        for (b = 0; b < 8; b++)
            if (i & (1 << b)) r |= 0x80 >> b;
        reverse_bits[i] = (u8)r;
    }

    memset(player_mask, 0, sizeof(player_mask));
    memset(missile_mask, 0, sizeof(missile_mask));
    memset(ball_mask, 0, sizeof(ball_mask));

    /* index k = x - pos + 160, period 160 */
    for (mode = 0; mode < 8; mode++) {
        int scale = (mode == 5) ? 2 : (mode == 7) ? 4 : 1;
        int delay = (scale > 1) ? 1 : 0;   /* wide players start 1 pixel late */
        int copy;
        for (copy = 0; copy <= 64; copy += 16) {
            if (!copy_at(mode, copy)) continue;
            for (i = 0; i < 8 * scale; i++) {
                int d = (copy + delay + i) % TIA_WIDTH, nm;
                for (nm = 0; nm < 2; nm++) {
                    if (nm && copy == 0) continue;
                    player_mask[nm][mode][d] = (u8)(0x80 >> (i / scale));
                    player_mask[nm][mode][d + TIA_WIDTH] = player_mask[nm][mode][d];
                }
            }
            for (size = 0; size < 4; size++) {
                int w = 1 << size;
                /* missiles are not stretched by NUSIZ 5/7 */
                for (i = 0; i < w; i++) {
                    int d = (copy + i) % TIA_WIDTH;
                    missile_mask[mode][size][d] = 1;
                    missile_mask[mode][size][d + TIA_WIDTH] = 1;
                }
            }
        }
    }
    for (size = 0; size < 4; size++) {
        for (i = 0; i < (1 << size); i++) {
            ball_mask[size][i] = 1;
            ball_mask[size][i + TIA_WIDTH] = 1;
        }
    }

    for (x = 0; x < TIA_WIDTH; x++) {
        int pfx = x >> 2;
        if (pfx < 20) {
            pf_mask[0][x] = pf_mask[1][x] = 1UL << pfx;
        } else {
            pf_mask[0][x] = 1UL << (pfx - 20);
            pf_mask[1][x] = 1UL << (39 - pfx);
        }
    }

    for (pfp = 0; pfp < 2; pfp++) {
        for (obj = 0; obj < 64; obj++) {
            u8 c = C_BK;
            int pl0 = obj & (O_P0 | O_M0), pl1 = obj & (O_P1 | O_M1);
            if (pfp) {
                if (obj & O_BL) c = C_BL;
                else if (obj & O_PF) c = C_PF;
                else if (pl0) c = C_P0;
                else if (pl1) c = C_P1;
            } else {
                if (pl0) c = C_P0;
                else if (pl1) c = C_P1;
                else if (obj & O_BL) c = C_BL;
                else if (obj & O_PF) c = C_PF;
            }
            prio_table[pfp][obj] = c;
        }
    }

    for (obj = 0; obj < 64; obj++) {
        u16 c = 0;
#define BOTH(a, b) ((obj & (a)) && (obj & (b)))
        if (BOTH(O_M0, O_P1)) c |= CX_M0P1;
        if (BOTH(O_M0, O_P0)) c |= CX_M0P0;
        if (BOTH(O_M1, O_P0)) c |= CX_M1P0;
        if (BOTH(O_M1, O_P1)) c |= CX_M1P1;
        if (BOTH(O_P0, O_PF)) c |= CX_P0PF;
        if (BOTH(O_P0, O_BL)) c |= CX_P0BL;
        if (BOTH(O_P1, O_PF)) c |= CX_P1PF;
        if (BOTH(O_P1, O_BL)) c |= CX_P1BL;
        if (BOTH(O_M0, O_PF)) c |= CX_M0PF;
        if (BOTH(O_M0, O_BL)) c |= CX_M0BL;
        if (BOTH(O_M1, O_PF)) c |= CX_M1PF;
        if (BOTH(O_M1, O_BL)) c |= CX_M1BL;
        if (BOTH(O_BL, O_PF)) c |= CX_BLPF;
        if (BOTH(O_P0, O_P1)) c |= CX_P0P1;
        if (BOTH(O_M0, O_M1)) c |= CX_M0M1;
#undef BOTH
        coll_table[obj] = c;
    }
    for (mode = 0; mode < 8; mode++) {
        player_nruns[0][mode] = find_runs(player_mask[0][mode], player_runs[0][mode]);
        player_nruns[1][mode] = find_runs(player_mask[1][mode], player_runs[1][mode]);
        for (size = 0; size < 4; size++)
            missile_nruns[mode][size] = find_runs(missile_mask[mode][size], missile_runs[mode][size]);
    }
    for (size = 0; size < 4; size++)
        ball_nruns[size] = find_runs(ball_mask[size], ball_runs[size]);
    tables_ready = 1;
}

/* ---- derived state updates ------------------------------------------ */

static void upd_p0(void)
{
    u8 g = tia.vdelp0 ? tia.grp0_old : tia.grp0_new;
    tia.gp0 = tia.refp0 ? reverse_bits[g] : g;
    tia.p0_mask = &player_mask[tia.p0_nomain][tia.nusiz0 & 7][TIA_WIDTH - tia.pos_p0];
    tia.runs[0] = player_runs[tia.p0_nomain][tia.nusiz0 & 7];
    tia.nruns[0] = player_nruns[tia.p0_nomain][tia.nusiz0 & 7];
}

static void upd_p1(void)
{
    u8 g = tia.vdelp1 ? tia.grp1_old : tia.grp1_new;
    tia.gp1 = tia.refp1 ? reverse_bits[g] : g;
    tia.p1_mask = &player_mask[tia.p1_nomain][tia.nusiz1 & 7][TIA_WIDTH - tia.pos_p1];
    tia.runs[1] = player_runs[tia.p1_nomain][tia.nusiz1 & 7];
    tia.nruns[1] = player_nruns[tia.p1_nomain][tia.nusiz1 & 7];
}

static int copy_at(int mode, int offset);

/* build the mask of locked object i (0 = M0, 1 = M1, 2 = BL) */
static void build_lock_mask(int i, int mode, int size, int pos)
{
    u8 *m = lock_mask[i];
    int c, k, n, x;
    memset(m, 0, 320);
    for (c = 0; c <= 64; c += 16) {
        int p, st, w;
        if (!copy_at(mode, c)) continue;
        p = (pos + c) % TIA_WIDTH;
        st = lock_shape[size][p & 3][0];
        w = lock_shape[size][p & 3][1];
        for (k = 0; k < w; k++) {
            int x = (p + st + k + TIA_WIDTH) % TIA_WIDTH;
            m[x] = m[x + TIA_WIDTH] = 1;
        }
    }
    /* runs relative to the object position */
    n = 0;
    x = 0;
    while (x < TIA_WIDTH && n < LOCK_RUNS) {
        if (m[x]) {
            int st = x;
            while (x < TIA_WIDTH && m[x]) x++;
            lock_runs[i][n * 2] = (u8)((st - pos + TIA_WIDTH) % TIA_WIDTH);
            lock_runs[i][n * 2 + 1] = (u8)(x - st);
            n++;
        } else {
            x++;
        }
    }
    tia.lock_nruns[i] = (u8)n;
}

static void upd_m0(void)
{
    tia.m0_on = tia.enam0 && !tia.resmp0;
    tia.m0_mask = &missile_mask[tia.nusiz0 & 7][(tia.nusiz0 >> 4) & 3][TIA_WIDTH - tia.pos_m0];
    tia.runs[2] = missile_runs[tia.nusiz0 & 7][(tia.nusiz0 >> 4) & 3];
    tia.nruns[2] = missile_nruns[tia.nusiz0 & 7][(tia.nusiz0 >> 4) & 3];
    if (tia.hm_lock[2]) {
        build_lock_mask(0, tia.nusiz0 & 7, (tia.nusiz0 >> 4) & 3, tia.pos_m0);
        tia.m0_mask = lock_mask[0];
        tia.runs[2] = lock_runs[0];
        tia.nruns[2] = tia.lock_nruns[0];
    }
}

static void upd_m1(void)
{
    tia.m1_on = tia.enam1 && !tia.resmp1;
    tia.m1_mask = &missile_mask[tia.nusiz1 & 7][(tia.nusiz1 >> 4) & 3][TIA_WIDTH - tia.pos_m1];
    tia.runs[3] = missile_runs[tia.nusiz1 & 7][(tia.nusiz1 >> 4) & 3];
    tia.nruns[3] = missile_nruns[tia.nusiz1 & 7][(tia.nusiz1 >> 4) & 3];
    if (tia.hm_lock[3]) {
        build_lock_mask(1, tia.nusiz1 & 7, (tia.nusiz1 >> 4) & 3, tia.pos_m1);
        tia.m1_mask = lock_mask[1];
        tia.runs[3] = lock_runs[1];
        tia.nruns[3] = tia.lock_nruns[1];
    }
}

static void upd_bl(void)
{
    tia.bl_on = tia.vdelbl ? tia.enabl_old : tia.enabl_new;
    tia.bl_mask = &ball_mask[(tia.ctrlpf >> 4) & 3][TIA_WIDTH - tia.pos_bl];
    tia.runs[4] = ball_runs[(tia.ctrlpf >> 4) & 3];
    tia.nruns[4] = ball_nruns[(tia.ctrlpf >> 4) & 3];
    if (tia.hm_lock[4]) {
        build_lock_mask(2, 0, (tia.ctrlpf >> 4) & 3, tia.pos_bl);
        tia.bl_mask = lock_mask[2];
        tia.runs[4] = lock_runs[2];
        tia.nruns[4] = tia.lock_nruns[2];
    }
}

static void upd_pf(void)
{
    tia.pf = (u32)(tia.pf0 >> 4)
           | ((u32)reverse_bits[tia.pf1] << 4)
           | ((u32)tia.pf2 << 12);
}

static void upd_colors(void)
{
    u8 score = (tia.ctrlpf & 0x06) == 0x02;    /* score mode, not with PF priority */
    tia.col_l[C_BK] = tia.col_r[C_BK] = tia.colubk;
    tia.col_l[C_BL] = tia.col_r[C_BL] = tia.colupf;
    tia.col_l[C_P0] = tia.col_r[C_P0] = tia.colup0;
    tia.col_l[C_P1] = tia.col_r[C_P1] = tia.colup1;
    tia.col_l[C_PF] = score ? tia.colup0 : tia.colupf;
    tia.col_r[C_PF] = score ? tia.colup1 : tia.colupf;
    tia.prio = prio_table[(tia.ctrlpf >> 2) & 1];
}

/* ---- audio ------------------------------------------------------------ */
/* Circuit-level model of one TIA audio channel (two phases per tick). */

static void audio_phase0(TiaAudioChannel *c)
{
    if (c->clk_en) {
        c->noise_bit4 = c->noise & 0x01;
        switch (c->audc & 0x03) {
        case 0x00:
        case 0x01: c->pulse_hold = 0; break;
        case 0x02: c->pulse_hold = (c->noise & 0x1E) != 0x02; break;
        case 0x03: c->pulse_hold = !c->noise_bit4; break;
        }
        if ((c->audc & 0x03) == 0)
            c->noise_fb = ((c->pulse ^ c->noise) & 0x01) ||
                          !(c->noise || c->pulse != 0x0A) ||
                          !(c->audc & 0x0C);
        else
            c->noise_fb = (((c->noise & 0x04) ? 1 : 0) ^ (c->noise & 0x01)) ||
                          c->noise == 0;
    }
    c->clk_en = c->div == c->audf;
    if (c->div == c->audf || c->div == 0x1F)
        c->div = 0;
    else
        c->div++;
}

static u8 audio_phase1(TiaAudioChannel *c)
{
    if (c->clk_en) {
        u8 fb = 0;
        switch (c->audc >> 2) {
        case 0x00:
            fb = (((c->pulse & 0x02) ? 1 : 0) ^ (c->pulse & 0x01)) &&
                 c->pulse != 0x0A && (c->audc & 0x03);
            break;
        case 0x01: fb = !(c->pulse & 0x08); break;
        case 0x02: fb = !c->noise_bit4; break;
        case 0x03: fb = !((c->pulse & 0x02) || !(c->pulse & 0x0E)); break;
        }
        c->noise >>= 1;
        if (c->noise_fb) c->noise |= 0x10;
        if (!c->pulse_hold) {
            c->pulse = (u8)(~(c->pulse >> 1) & 0x07);
            if (fb) c->pulse |= 0x08;
        }
    }
    return (u8)((c->pulse & 0x01) * c->audv);
}

/* one scanline = two audio ticks; returns the summed output (0..30) */
static u8 audio_channel_line(TiaAudioChannel *c)
{
    u8 s;
    audio_phase0(c);
    s = audio_phase1(c);
    audio_phase0(c);
    return (u8)(s + audio_phase1(c));
}

static void audio_line(void)
{
    u8 s0, s1;
    int i = tia.audio_len;
    /* a silent channel is not clocked: its divider/polynomial phase does
     * not matter until the volume is raised again */
    s0 = tia.ach[0].audv ? audio_channel_line(&tia.ach[0]) : 0;
    s1 = tia.ach[1].audv ? audio_channel_line(&tia.ach[1]) : 0;
    if (tia.audio_buf && i < TIA_MAX_LINES) {
        tia.audio_buf[i] = s0;
        tia.audio_buf[TIA_MAX_LINES + i] = s1;
        tia.audio_len = i + 1;
    }
}

/* ---- rendering -------------------------------------------------------- */

static u32 scratch_line32[TIA_WIDTH / 4];   /* lines beyond the framebuffer */
#define scratch_line ((u8 *)scratch_line32)

static u8 *line_ptr(void)
{
    if (tia.fb && tia.line >= 0 && tia.line < TIA_FB_LINES)
        return tia.fb + tia.line * TIA_WIDTH;
    return scratch_line;
}

#ifdef A26_TIA_REFERENCE
/* original per-pixel renderer, kept as reference for tests/tia_equiv.c */
static void render_ref(int x0, int x1)
{
    u8 *out = line_ptr();
    int x;

    if (tia.vblank & 0x02) {
        memset(out + x0, 0, (size_t)(x1 - x0));
        return;
    }
    if (tia.cur_first_visible < 0) tia.cur_first_visible = tia.line;
    tia.cur_last_visible = tia.line;

    if (tia.hmove_blank && x0 < 8) {
        int e = x1 < 8 ? x1 : 8;
        memset(out + x0, 0, (size_t)(e - x0));
        x0 = e;
    }

    {
        const u32 *pfm = pf_mask[tia.ctrlpf & 1];
        const u8 *p0m = tia.p0_mask, *p1m = tia.p1_mask;
        const u8 *m0m = tia.m0_mask, *m1m = tia.m1_mask, *blm = tia.bl_mask;
        const u8 *prio = tia.prio;
        u32 pf = tia.pf;
        u8 gp0 = tia.gp0, gp1 = tia.gp1;
        u8 m0 = tia.m0_on, m1 = tia.m1_on, bl = tia.bl_on;
        u16 coll = tia.coll;

        if (!gp0 && !gp1 && !m0 && !m1 && !bl) {
            /* fast path: playfield + background only. The playfield
             * changes every 4 pixels, so whole 4-pixel blocks are filled. */
            const u8 cbk = tia.colubk, cl = tia.col_l[C_PF], cr = tia.col_r[C_PF];
            u8 *o;
            x = x0;
            while (x < x1 && (x & 3)) {             /* unaligned head */
                out[x] = (pf & pfm[x]) ? ((x < 80) ? cl : cr) : cbk;
                x++;
            }
            o = out + x;
            while (x + 4 <= x1) {                   /* whole blocks */
                u8 c = (pf & pfm[x]) ? ((x < 80) ? cl : cr) : cbk;
                o[0] = c; o[1] = c; o[2] = c; o[3] = c;
                o += 4;
                x += 4;
            }
            while (x < x1) {                        /* tail */
                out[x] = (pf & pfm[x]) ? ((x < 80) ? cl : cr) : cbk;
                x++;
            }
            return;
        }

        for (x = x0; x < x1; x++) {
            u8 o = 0;
            if (pf & pfm[x]) o = O_PF;
            if (gp0 & p0m[x]) o |= O_P0;
            if (gp1 & p1m[x]) o |= O_P1;
            if (m0 && m0m[x]) o |= O_M0;
            if (m1 && m1m[x]) o |= O_M1;
            if (bl && blm[x]) o |= O_BL;
            coll |= coll_table[o];
            out[x] = (x < 80) ? tia.col_l[prio[o]] : tia.col_r[prio[o]];
        }
        tia.coll = coll;
    }
}

#endif

static u32 rep4(u8 c)
{
    u32 v = c;
    v |= v << 8;
    return v | (v << 16);
}

/* playfield + background for [x0, x1).
 * The playfield has 40 blocks of 4 pixels: blocks 0-19 show pf bits 0-19,
 * blocks 20-39 show bits 0-19 again (bits 19-0 when reflected). Whole
 * blocks are written as one longword (line buffers are 4-byte aligned). */
static void fill_playfield(u8 *out, int x0, int x1)
{
    const u32 pf = tia.pf;
    const int reflect = tia.ctrlpf & 1;
    const u8 cbk = tia.colubk, cl = tia.col_l[C_PF], cr = tia.col_r[C_PF];
    int x = x0, blk, last;
    u32 *o, bk4, l4, r4;

    if (!pf) {                                  /* no playfield: plain fill */
        memset(out + x0, cbk, (size_t)(x1 - x0));
        return;
    }
#define PF_ON(b) (((b) < 20) ? (pf >> (b)) & 1 : (pf >> (reflect ? 39 - (b) : (b) - 20)) & 1)
    while (x < x1 && (x & 3)) {                 /* unaligned head */
        out[x] = PF_ON(x >> 2) ? ((x < 80) ? cl : cr) : cbk;
        x++;
    }
    if (x >= x1) return;
    bk4 = rep4(cbk);
    l4 = rep4(cl);
    r4 = rep4(cr);
    o = (u32 *)(void *)(out + x);
    last = x1 >> 2;                             /* first block not fully inside */
    blk = x >> 2;
    if (blk < 20) {                             /* left half: bits 0..19 */
        int end = last < 20 ? last : 20;
        u32 m = pf >> blk;
        for (; blk < end; blk++, m >>= 1)
            *o++ = (m & 1) ? l4 : bk4;
    }
    if (blk < last) {                           /* right half */
        if (!reflect) {
            u32 m = pf >> (blk - 20);
            for (; blk < last; blk++, m >>= 1)
                *o++ = (m & 1) ? r4 : bk4;
        } else {
            u32 m = 1UL << (39 - blk);
            for (; blk < last; blk++, m >>= 1)
                *o++ = (pf & m) ? r4 : bk4;
        }
    }
    for (x = blk << 2; x < x1; x++)             /* tail */
        out[x] = PF_ON(x >> 2) ? ((x < 80) ? cl : cr) : cbk;
#undef PF_ON
}

/* full priority + collision evaluation for pixels [a, b) */
static void eval_pixels(u8 *out, int a, int b)
{
    const u32 *pfm = pf_mask[tia.ctrlpf & 1];
    const u8 *p0m = tia.p0_mask, *p1m = tia.p1_mask;
    const u8 *m0m = tia.m0_mask, *m1m = tia.m1_mask, *blm = tia.bl_mask;
    const u8 *prio = tia.prio;
    const u32 pf = tia.pf;
    const u8 gp0 = tia.gp0, gp1 = tia.gp1;
    const u8 m0 = tia.m0_on, m1 = tia.m1_on, bl = tia.bl_on;
    u16 coll = tia.coll;
    int x;

    for (x = a; x < b; x++) {
        u8 o = 0;
        if (pf & pfm[x]) o = O_PF;
        if (gp0 & p0m[x]) o |= O_P0;
        if (gp1 & p1m[x]) o |= O_P1;
        if (m0 && m0m[x]) o |= O_M0;
        if (m1 && m1m[x]) o |= O_M1;
        if (bl && blm[x]) o |= O_BL;
        coll |= coll_table[o];
        out[x] = (x < 80) ? tia.col_l[prio[o]] : tia.col_r[prio[o]];
    }
    tia.coll = coll;
}

/* stamp one object: evaluate its runs, clipped to [x0, x1) */
static void stamp_object(u8 *out, int obj, int pos, int x0, int x1)
{
    const u8 *r = tia.runs[obj];
    int n = tia.nruns[obj];
    while (n--) {
        int s = pos + r[0], e;
        if (s >= TIA_WIDTH) s -= TIA_WIDTH;
        e = s + r[1];
        r += 2;
        if (e > TIA_WIDTH) {                    /* wraps around the right edge */
            int a = s > x0 ? s : x0;
            int b = (e - TIA_WIDTH) < x1 ? (e - TIA_WIDTH) : x1;
            if (a < x1) eval_pixels(out, a, x1);
            if (x0 < b) eval_pixels(out, x0, b);
            continue;
        }
        if (s < x0) s = x0;
        if (e > x1) e = x1;
        if (s < e) eval_pixels(out, s, e);
    }
}

/* draw pixels [x0, x1) of the current line */
static void render(int x0, int x1)
{
    u8 *out = line_ptr();

#ifdef A26_TIA_REFERENCE
    if (tia_use_reference) { render_ref(x0, x1); return; }
#endif
    if (tia.vblank & 0x02) {
        memset(out + x0, 0, (size_t)(x1 - x0));
        return;
    }
    if (tia.cur_first_visible < 0) tia.cur_first_visible = tia.line;
    tia.cur_last_visible = tia.line;

    if (tia.hmove_blank && x0 < 8) {
        int e = x1 < 8 ? x1 : 8;
        memset(out + x0, 0, (size_t)(e - x0));
        x0 = e;
        if (x0 >= x1) return;
    }

    fill_playfield(out, x0, x1);
    if (tia.gp0) stamp_object(out, 0, tia.pos_p0, x0, x1);
    if (tia.gp1) stamp_object(out, 1, tia.pos_p1, x0, x1);
    if (tia.m0_on) stamp_object(out, 2, tia.pos_m0, x0, x1);
    if (tia.m1_on) stamp_object(out, 3, tia.pos_m1, x0, x1);
    if (tia.bl_on) stamp_object(out, 4, tia.pos_bl, x0, x1);
}

static void end_frame(void)
{
    tia.frame_lines = tia.line;
    tia.first_visible = tia.cur_first_visible;
    tia.last_visible = tia.cur_last_visible;
    tia.cur_first_visible = -1;
    tia.cur_last_visible = -1;
    tia.audio_frame_len = tia.audio_len;
    tia.audio_len = 0;
    tia.line = 0;
    tia.frame_count++;
    tia.frame_done = 1;
    a26_stop = 1;
}

static void apply_hmove(void)
{
#define MOVE(pos, d) pos = (u8)(((int)(pos) + (d) + TIA_WIDTH) % TIA_WIDTH)
    MOVE(tia.pos_p0, tia.hm_disp[0]);
    MOVE(tia.pos_p1, tia.hm_disp[1]);
    MOVE(tia.pos_m0, tia.hm_disp[2]);
    MOVE(tia.pos_m1, tia.hm_disp[3]);
    MOVE(tia.pos_bl, tia.hm_disp[4]);
#undef MOVE
    tia.hm_pending = 0;
    upd_p0(); upd_p1(); upd_m0(); upd_m1(); upd_bl();
}

/* locked objects move 17 pixels left per line (measured with gopher2600) */
static void move_locked(void)
{
    u8 *pos[5];
    int i;
    pos[0] = &tia.pos_p0; pos[1] = &tia.pos_p1; pos[2] = &tia.pos_m0;
    pos[3] = &tia.pos_m1; pos[4] = &tia.pos_bl;
    for (i = 0; i < 5; i++) {
        if (!tia.hm_lock[i]) continue;
        *pos[i] = (u8)((*pos[i] + TIA_WIDTH - 17) % TIA_WIDTH);
    }
    upd_p0(); upd_p1(); upd_m0(); upd_m1(); upd_bl();
}

/* HMxx write while the HMOVE ripple may still run: an object that has not
 * reached its stop tick continues to the new value, or locks if the new
 * value's tick has already passed. A locked object stops on a value of 8
 * (count 0). */
static void hm_changed(int i, u8 hm, u32 cc)
{
    s32 tw = (s32)(cc - tia.hm_line_cc) - 2;    /* takes effect 2 clocks later */
    int t0, k_done, v_new = (hm >> 4) ^ 8;
    int d;
    u8 *pos[5];

    if (tia.hm_lock[i]) {
        if (v_new == 0) { tia.hm_lock[i] = 0; goto update; }
        return;
    }
    if (tia.hm_w > 20) return;          /* only early HMOVEs are modelled here */
    t0 = hmove_t0(tia.hm_w);
    if (tw < t0 || tw > t0 + 64) return;  /* ripple not running */
    k_done = (int)((tw - t0 + 3) / 4);  /* ticks before tw */
    if (tia.hm_v[i] < k_done) return;   /* already stopped */
    if (v_new >= k_done) {
        d = hmove_calc(tia.hm_w, v_new);
    } else {
        d = hmove_calc(tia.hm_w, 16);
        tia.hm_lock[i] = 1;
    }
    tia.hm_v[i] = (u8)v_new;
    pos[0] = &tia.pos_p0; pos[1] = &tia.pos_p1; pos[2] = &tia.pos_m0;
    pos[3] = &tia.pos_m1; pos[4] = &tia.pos_bl;
    *pos[i] = (u8)((*pos[i] + d - tia.hm_disp[i] + 2 * TIA_WIDTH) % TIA_WIDTH);
    tia.hm_disp[i] = (s8)d;
update:
    upd_p0(); upd_p1(); upd_m0(); upd_m1(); upd_bl();
}

static void end_line(void)
{
    audio_line();
    tia.hmove_blank = 0;
    if (tia.hm_pending) apply_hmove();
    if (tia.hm_lock[0] | tia.hm_lock[1] | tia.hm_lock[2] | tia.hm_lock[3] | tia.hm_lock[4])
        move_locked();
    if (tia.p0_nomain) { tia.p0_nomain = 0; upd_p0(); }
    if (tia.p1_nomain) { tia.p1_nomain = 0; upd_p1(); }
    tia.line_start_cc += TIA_LINE_CC;
    tia.line++;
    if (tia.line >= TIA_MAX_LINES)
        end_frame();
}

static void update_to(u32 target)
{
    while ((s32)(target - tia.last_cc) > 0) {
        u32 pos = tia.last_cc - tia.line_start_cc;
        u32 end = target - tia.line_start_cc;
        if (end > TIA_LINE_CC) end = TIA_LINE_CC;
        if (end > TIA_HBLANK) {
            int x0 = pos > TIA_HBLANK ? (int)(pos - TIA_HBLANK) : 0;
            render(x0, (int)(end - TIA_HBLANK));
        }
        tia.last_cc += end - pos;
        if (end == TIA_LINE_CC)
            end_line();
    }
}

void tia_update(void)
{
    update_to(a26_cycles * 3u);
}

/* horizontal position (colour clocks since line start) of 'cc' */
static u32 hpos_of(u32 cc)
{
    s32 h = (s32)(cc - tia.line_start_cc);
    while (h < 0) h += TIA_LINE_CC;
    while (h >= TIA_LINE_CC) h -= TIA_LINE_CC;
    return (u32)h;
}

/* ---- public ----------------------------------------------------------- */

void tia_init(u8 *framebuffer, u8 *audiobuffer)
{
    if (!tables_ready) build_tables();
    tia.fb = framebuffer;
    tia.audio_buf = audiobuffer;
}

void tia_reset(void)
{
    u8 *fb = tia.fb, *ab = tia.audio_buf;
    int i;
    if (!tables_ready) build_tables();
    memset(&tia, 0, sizeof(tia));
    tia.fb = fb;
    tia.audio_buf = ab;
    tia.line_start_cc = tia.last_cc = a26_cycles * 3u;
    tia.cur_first_visible = tia.cur_last_visible = -1;
    tia.first_visible = 40;
    tia.last_visible = 231;
    tia.frame_lines = 262;
    for (i = 0; i < 4; i++) tia.paddle[i] = -1;
    upd_p0(); upd_p1(); upd_m0(); upd_m1(); upd_bl(); upd_pf(); upd_colors();
}

u8 tia_read(u16 addr)
{
    u8 v = 0;
    u8 r = (u8)(addr & 0x0F);

    tia_update();
    if (r < 8) {
        u16 c = tia.coll >> (r * 2);
        v = (u8)(((c & 1) ? 0x80 : 0) | ((c & 2) ? 0x40 : 0));
    } else if (r < 12) {
        int p = r - 8;
        if (!(tia.vblank & 0x80) && tia.paddle[p] >= 0) {
            u32 lines = (a26_cycles * 3u - tia.dump_release_cc) / TIA_LINE_CC;
            if (lines >= (u32)tia.paddle[p]) v = 0x80;
        }
    } else if (r < 14) {
        int p = r - 12;
        int pressed;
        if (a26_input_hook) a26_input_hook();
        pressed = tia.fire[p];
        if (tia.vblank & 0x40) {
            if (pressed) tia.fire_latch[p] = 1;
            pressed = tia.fire_latch[p];
        }
        v = pressed ? 0x00 : 0x80;
    }
    return (u8)(v | (a26_databus & 0x3F));
}

/* Writes that cannot change any pixel or collision need no catch-up:
 * returns 2 = value unchanged (write is a no-op), 1 = only affects later
 * events (HMxx / HMCLR are applied by HMOVE, which catches up itself),
 * 0 = normal write.
 * Registers with a write delay (VBLANK, NUSIZx, REFPx, PFx) are never
 * skipped: their catch-up renders a few pixels ahead, which also defers
 * the effect of following writes - skipping them would change timing. */
static int write_class(u8 r, u8 val)
{
    switch (r) {
    case 0x06: return (val & 0xFE) == tia.colup0 ? 2 : 0;
    case 0x07: return (val & 0xFE) == tia.colup1 ? 2 : 0;
    case 0x08: return (val & 0xFE) == tia.colupf ? 2 : 0;
    case 0x09: return (val & 0xFE) == tia.colubk ? 2 : 0;
    case 0x0A: return val == tia.ctrlpf ? 2 : 0;
    case 0x1D: return ((val & 0x02) != 0) == tia.enam0 ? 2 : 0;
    case 0x1E: return ((val & 0x02) != 0) == tia.enam1 ? 2 : 0;
    case 0x1F: return ((val & 0x02) != 0) == tia.enabl_new ? 2 : 0;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x2B:
        return 1;
    case 0x25: return (val & 0x01) == tia.vdelp0 ? 2 : 0;
    case 0x26: return (val & 0x01) == tia.vdelp1 ? 2 : 0;
    case 0x27: return (val & 0x01) == tia.vdelbl ? 2 : 0;
    case 0x28: return ((val & 0x02) != 0) == tia.resmp0 ? 2 : 0;
    case 0x29: return ((val & 0x02) != 0) == tia.resmp1 ? 2 : 0;
    }
    return 0;
}

/* an HMxx write matters if the ripple of this line's HMOVE may still run
 * or an object is locked */
#define HM_LOCKED() (tia.hm_lock[0] | tia.hm_lock[1] | tia.hm_lock[2] | tia.hm_lock[3] | tia.hm_lock[4])
#define HM_WATCH(cc) ((u32)((cc) - tia.hm_line_cc) < 140u || HM_LOCKED())

void tia_write(u16 addr, u8 val)
{
    u32 cc = a26_cycles * 3u;
    u8 r = (u8)(addr & 0x3F);
    u32 delay = 0;
    int wc = write_class(r, val);

#ifdef A26_TRACE
    if (tia_trace)
        printf("TIA f%lu l%3d cyc%3lu px%4ld  %02X <- %02X\n", (unsigned long)tia.frame_count,
               tia.line + (int)((cc - tia.line_start_cc) / TIA_LINE_CC),
               (unsigned long)((cc - tia.line_start_cc) % TIA_LINE_CC / 3),
               (long)((cc - tia.line_start_cc) % TIA_LINE_CC) - 68, r, val);
#endif
#ifdef A26_TIA_REFERENCE
    if (tia_use_reference) wc = 0;
#endif
    if (wc == 1 && HM_WATCH(cc)) wc = 0;   /* may move objects now: catch up first */
    if (wc == 2) return;
    if (wc == 1) goto apply;

    switch (r) {
    case 0x01: delay = 1; break;                   /* VBLANK */
    case 0x04: case 0x05: delay = 8; break;        /* NUSIZx */
    case 0x0B: case 0x0C: delay = 1; break;        /* REFPx */
    case 0x1B: case 0x1C: delay = 1; break;        /* GRPx */
    case 0x1D: case 0x1E: case 0x1F: delay = 1; break; /* ENAMx, ENABL */
    case 0x0D: case 0x0E: case 0x0F: {             /* PFx: sampled every 4 pixels */
        static const u8 d[4] = { 4, 5, 2, 3 };
        u32 x = hpos_of(cc);
        delay = d[(x / 3) & 3];
        break;
    }
    }
    update_to(cc + delay);

apply:
    switch (r) {
    case 0x00: /* VSYNC */
        if ((val & 0x02) && !(tia.vsync & 0x02))
            end_frame();
        tia.vsync = val;
        break;
    case 0x01: /* VBLANK */
        if ((tia.vblank & 0x80) && !(val & 0x80))
            tia.dump_release_cc = cc;
        if (!(val & 0x40))
            tia.fire_latch[0] = tia.fire_latch[1] = 0;
        tia.vblank = val;
        break;
    case 0x02: { /* WSYNC: halt the CPU until the start of the next line */
        u32 pos = tia.last_cc - tia.line_start_cc;
        if (pos > 0 && pos < TIA_LINE_CC)
            a26_cycles += (TIA_LINE_CC - pos) / 3;
        break;
    }
    case 0x03: /* RSYNC: restart horizontal sync (rarely used) */
        tia.line_start_cc = tia.last_cc - (TIA_LINE_CC - 3);
        break;
    case 0x04: tia.nusiz0 = val; upd_p0(); upd_m0(); break;
    case 0x05: tia.nusiz1 = val; upd_p1(); upd_m1(); break;
    case 0x06: tia.colup0 = val & 0xFE; upd_colors(); break;
    case 0x07: tia.colup1 = val & 0xFE; upd_colors(); break;
    case 0x08: tia.colupf = val & 0xFE; upd_colors(); break;
    case 0x09: tia.colubk = val & 0xFE; upd_colors(); break;
    case 0x0A: tia.ctrlpf = val; upd_colors(); upd_bl(); break;
    case 0x0B: tia.refp0 = (val & 0x08) != 0; upd_p0(); break;
    case 0x0C: tia.refp1 = (val & 0x08) != 0; upd_p1(); break;
    case 0x0D: tia.pf0 = val; upd_pf(); break;
    case 0x0E: tia.pf1 = val; upd_pf(); break;
    case 0x0F: tia.pf2 = val; upd_pf(); break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: { /* RESP0/1, RESM0/1, RESBL */
        u32 hpos = hpos_of(cc);
        int newx;
        int obj_delay = (r <= 0x11) ? 5 : 4;
        if (hpos < TIA_HBLANK)
            newx = (r <= 0x11) ? 3 : 2;
        else
            newx = (int)((hpos - TIA_HBLANK + obj_delay) % TIA_WIDTH);
        switch (r) {
        /* the main copy of a player starts when its counter wraps, so
         * after a reset it only appears from the next line on */
        case 0x10: tia.pos_p0 = (u8)newx; tia.p0_nomain = 1; upd_p0(); break;
        case 0x11: tia.pos_p1 = (u8)newx; tia.p1_nomain = 1; upd_p1(); break;
        case 0x12: tia.pos_m0 = (u8)newx; upd_m0(); break;
        case 0x13: tia.pos_m1 = (u8)newx; upd_m1(); break;
        case 0x14: tia.pos_bl = (u8)newx; upd_bl(); break;
        }
        break;
    }
    case 0x15: tia.ach[0].audc = val & 0x0F; break;
    case 0x16: tia.ach[1].audc = val & 0x0F; break;
    case 0x17: tia.ach[0].audf = val & 0x1F; break;
    case 0x18: tia.ach[1].audf = val & 0x1F; break;
    case 0x19: tia.ach[0].audv = val & 0x0F; break;
    case 0x1A: tia.ach[1].audv = val & 0x0F; break;
    case 0x1B: /* GRP0 */
        tia.grp0_new = val;
        tia.grp1_old = tia.grp1_new;
        upd_p0(); upd_p1();
        break;
    case 0x1C: /* GRP1 */
        tia.grp1_new = val;
        tia.grp0_old = tia.grp0_new;
        tia.enabl_old = tia.enabl_new;
        upd_p0(); upd_p1(); upd_bl();
        break;
    case 0x1D: tia.enam0 = (val & 0x02) != 0; upd_m0(); break;
    case 0x1E: tia.enam1 = (val & 0x02) != 0; upd_m1(); break;
    case 0x1F: tia.enabl_new = (val & 0x02) != 0; upd_bl(); break;
    case 0x20: tia.hmp0 = val & 0xF0; if (HM_WATCH(cc)) hm_changed(0, tia.hmp0, cc); break;
    case 0x21: tia.hmp1 = val & 0xF0; if (HM_WATCH(cc)) hm_changed(1, tia.hmp1, cc); break;
    case 0x22: tia.hmm0 = val & 0xF0; if (HM_WATCH(cc)) hm_changed(2, tia.hmm0, cc); break;
    case 0x23: tia.hmm1 = val & 0xF0; if (HM_WATCH(cc)) hm_changed(3, tia.hmm1, cc); break;
    case 0x24: tia.hmbl = val & 0xF0; if (HM_WATCH(cc)) hm_changed(4, tia.hmbl, cc); break;
    case 0x25: tia.vdelp0 = val & 0x01; upd_p0(); break;
    case 0x26: tia.vdelp1 = val & 0x01; upd_p1(); break;
    case 0x27: tia.vdelbl = val & 0x01; upd_bl(); break;
    case 0x28: case 0x29: { /* RESMP0/1: lock missile to player centre */
        int m = r - 0x28;
        u8 nus = m ? tia.nusiz1 : tia.nusiz0;
        u8 on = (val & 0x02) != 0;
        u8 was = m ? tia.resmp1 : tia.resmp0;
        if (was && !on) {
            int middle = ((nus & 7) == 5) ? 8 : ((nus & 7) == 7) ? 16 : 4;
            if (m) tia.pos_m1 = (u8)((tia.pos_p1 + middle) % TIA_WIDTH);
            else   tia.pos_m0 = (u8)((tia.pos_p0 + middle) % TIA_WIDTH);
        }
        if (m) { tia.resmp1 = on; upd_m1(); }
        else   { tia.resmp0 = on; upd_m0(); }
        break;
    }
    case 0x2A: { /* HMOVE */
        /* The effect depends on the cycle of the write (hmove_disp): early
         * in the line the objects move in the (extended) horizontal blank,
         * in the middle nothing happens, and late in the line the extra
         * clocks land in the next line's blank. */
        u32 w = hpos_of(cc) / 3;
        if (w > 75) w = 75;
        tia.hm_w = (u8)w;
        tia.hm_line_cc = cc - hpos_of(cc);
        tia.hm_v[0] = (u8)((tia.hmp0 >> 4) ^ 8);
        tia.hm_v[1] = (u8)((tia.hmp1 >> 4) ^ 8);
        tia.hm_v[2] = (u8)((tia.hmm0 >> 4) ^ 8);
        tia.hm_v[3] = (u8)((tia.hmm1 >> 4) ^ 8);
        tia.hm_v[4] = (u8)((tia.hmbl >> 4) ^ 8);
        tia.hm_lock[0] = tia.hm_lock[1] = tia.hm_lock[2] = tia.hm_lock[3] = tia.hm_lock[4] = 0;
        tia.hm_disp[0] = hmove_disp[tia.hmp0 >> 4][w];
        tia.hm_disp[1] = hmove_disp[tia.hmp1 >> 4][w];
        tia.hm_disp[2] = hmove_disp[tia.hmm0 >> 4][w];
        tia.hm_disp[3] = hmove_disp[tia.hmm1 >> 4][w];
        tia.hm_disp[4] = hmove_disp[tia.hmbl >> 4][w];
        if (w <= 20)
            tia.hmove_blank = 1;
        if (w < 54)
            apply_hmove();
        else
            tia.hm_pending = 1;         /* applied at the end of the line */
        break;
    }
    case 0x2B: /* HMCLR */
        tia.hmp0 = tia.hmp1 = tia.hmm0 = tia.hmm1 = tia.hmbl = 0;
        if (HM_WATCH(cc)) {
            int i;
            for (i = 0; i < 5; i++) hm_changed(i, 0, cc);
        }
        break;
    case 0x2C: /* CXCLR */
        tia.coll = 0;
        break;
    }
}
