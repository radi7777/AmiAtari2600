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
 * Audio is clocked twice per scanline (31.4 kHz); one summed sample per
 * scanline is stored (~15.7 kHz), which maps 1:1 to one Paula sample per
 * Amiga raster line.
 */
#include <string.h>
#include "tia.h"
#include "bus.h"

Tia tia;

/* ---- lookup tables -------------------------------------------------- */

static u8  player_mask[8][320];     /* bit mask (0x80 = first pixel) */
static u8  missile_mask[8][4][320]; /* 0/1 */
static u8  ball_mask[4][320];
static u32 pf_mask[2][TIA_WIDTH];   /* [reflect][x] -> bit in tia.pf */
static u8  reverse_bits[256];
static u8  prio_table[2][64];       /* [pfp][objects] -> colour index */
static u16 coll_table[64];
static int tables_ready;

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
                int d = (copy + delay + i) % TIA_WIDTH;
                player_mask[mode][d] = (u8)(0x80 >> (i / scale));
                player_mask[mode][d + TIA_WIDTH] = player_mask[mode][d];
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
    tables_ready = 1;
}

/* ---- derived state updates ------------------------------------------ */

static void upd_p0(void)
{
    u8 g = tia.vdelp0 ? tia.grp0_old : tia.grp0_new;
    tia.gp0 = tia.refp0 ? reverse_bits[g] : g;
    tia.p0_mask = &player_mask[tia.nusiz0 & 7][TIA_WIDTH - tia.pos_p0];
}

static void upd_p1(void)
{
    u8 g = tia.vdelp1 ? tia.grp1_old : tia.grp1_new;
    tia.gp1 = tia.refp1 ? reverse_bits[g] : g;
    tia.p1_mask = &player_mask[tia.nusiz1 & 7][TIA_WIDTH - tia.pos_p1];
}

static void upd_m0(void)
{
    tia.m0_on = tia.enam0 && !tia.resmp0;
    tia.m0_mask = &missile_mask[tia.nusiz0 & 7][(tia.nusiz0 >> 4) & 3][TIA_WIDTH - tia.pos_m0];
}

static void upd_m1(void)
{
    tia.m1_on = tia.enam1 && !tia.resmp1;
    tia.m1_mask = &missile_mask[tia.nusiz1 & 7][(tia.nusiz1 >> 4) & 3][TIA_WIDTH - tia.pos_m1];
}

static void upd_bl(void)
{
    tia.bl_on = tia.vdelbl ? tia.enabl_old : tia.enabl_new;
    tia.bl_mask = &ball_mask[(tia.ctrlpf >> 4) & 3][TIA_WIDTH - tia.pos_bl];
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

static u8 scratch_line[TIA_WIDTH];

static u8 *line_ptr(void)
{
    if (tia.fb && tia.line >= 0 && tia.line < TIA_FB_LINES)
        return tia.fb + tia.line * TIA_WIDTH;
    return scratch_line;
}

/* draw pixels [x0, x1) of the current line */
static void render(int x0, int x1)
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

static void end_line(void)
{
    audio_line();
    tia.hmove_blank = 0;
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
        int pressed = tia.fire[p];
        if (tia.vblank & 0x40) {
            if (pressed) tia.fire_latch[p] = 1;
            pressed = tia.fire_latch[p];
        }
        v = pressed ? 0x00 : 0x80;
    }
    return (u8)(v | (a26_databus & 0x3F));
}

void tia_write(u16 addr, u8 val)
{
    u32 cc = a26_cycles * 3u;
    u8 r = (u8)(addr & 0x3F);
    u32 delay = 0;

    switch (r) {
    case 0x01: delay = 1; break;                   /* VBLANK */
    case 0x04: case 0x05: delay = 8; break;        /* NUSIZx */
    case 0x0B: case 0x0C: delay = 1; break;        /* REFPx */
    case 0x0D: case 0x0E: case 0x0F: {             /* PFx: sampled every 4 pixels */
        static const u8 d[4] = { 4, 5, 2, 3 };
        u32 x = hpos_of(cc);
        delay = d[(x / 3) & 3];
        break;
    }
    }
    update_to(cc + delay);

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
        case 0x10: tia.pos_p0 = (u8)newx; upd_p0(); break;
        case 0x11: tia.pos_p1 = (u8)newx; upd_p1(); break;
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
    case 0x20: tia.hmp0 = val & 0xF0; break;
    case 0x21: tia.hmp1 = val & 0xF0; break;
    case 0x22: tia.hmm0 = val & 0xF0; break;
    case 0x23: tia.hmm1 = val & 0xF0; break;
    case 0x24: tia.hmbl = val & 0xF0; break;
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
        u32 hpos = hpos_of(cc);
#define MOVE(pos, hm) pos = (u8)(((int)(pos) - ((s8)(hm) >> 4) + TIA_WIDTH) % TIA_WIDTH)
        MOVE(tia.pos_p0, tia.hmp0);
        MOVE(tia.pos_p1, tia.hmp1);
        MOVE(tia.pos_m0, tia.hmm0);
        MOVE(tia.pos_m1, tia.hmm1);
        MOVE(tia.pos_bl, tia.hmbl);
#undef MOVE
        if (hpos < TIA_HBLANK)
            tia.hmove_blank = 1;
        upd_p0(); upd_p1(); upd_m0(); upd_m1(); upd_bl();
        break;
    }
    case 0x2B: /* HMCLR */
        tia.hmp0 = tia.hmp1 = tia.hmm0 = tia.hmm1 = tia.hmbl = 0;
        break;
    case 0x2C: /* CXCLR */
        tia.coll = 0;
        break;
    }
}
