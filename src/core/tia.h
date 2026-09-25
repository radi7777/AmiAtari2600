/*
 * tia.h - TIA (Television Interface Adaptor) video + audio + input.
 *
 * Output: one byte per TIA pixel (160 per line) holding the TIA colour
 * value (hue << 4 | lum << 1). Colour 0 = black. The frontend maps these
 * to its own palette (see palette.h).
 */
#ifndef A26_TIA_H
#define A26_TIA_H

#include "types.h"

#define TIA_WIDTH       160
#define TIA_LINE_CC     228     /* colour clocks per scanline */
#define TIA_HBLANK      68
#define TIA_FB_LINES    320     /* lines stored per frame (PAL needs ~312) */
#define TIA_MAX_LINES   400     /* force a frame end if VSYNC never comes */

/* collision bits in Tia.coll */
#define CX_M0P1 0x0001
#define CX_M0P0 0x0002
#define CX_M1P0 0x0004
#define CX_M1P1 0x0008
#define CX_P0PF 0x0010
#define CX_P0BL 0x0020
#define CX_P1PF 0x0040
#define CX_P1BL 0x0080
#define CX_M0PF 0x0100
#define CX_M0BL 0x0200
#define CX_M1PF 0x0400
#define CX_M1BL 0x0800
#define CX_BLPF 0x1000
#define CX_P0P1 0x4000
#define CX_M0M1 0x8000

typedef struct {
    u8  audc, audf, audv;
    u8  div;            /* frequency divider counter */
    u8  clk_en;
    u8  noise;          /* 5 bit */
    u8  pulse;          /* 4 bit */
    u8  noise_fb;
    u8  pulse_hold;
    u8  noise_bit4;
} TiaAudioChannel;

typedef struct {
    /* timing */
    u32 line_start_cc;      /* absolute colour clock of current line start */
    u32 last_cc;            /* rendered up to this colour clock */
    int line;               /* scanline within current frame */

    /* frame result */
    u8  *fb;                /* TIA_FB_LINES * TIA_WIDTH bytes */
    int frame_done;         /* set when a frame completed (VSYNC) */
    int frame_lines;        /* number of lines of the last completed frame */
    int first_visible;      /* first line with VBLANK off (last frame) */
    int last_visible;       /* last line with VBLANK off (last frame) */
    int cur_first_visible, cur_last_visible;
    u32 frame_count;

    /* registers */
    u8  vsync, vblank;
    u8  nusiz0, nusiz1;
    u8  colup0, colup1, colupf, colubk;
    u8  ctrlpf;
    u8  refp0, refp1;
    u8  pf0, pf1, pf2;
    u8  grp0_new, grp0_old, grp1_new, grp1_old;
    u8  enam0, enam1, enabl_new, enabl_old;
    u8  hmp0, hmp1, hmm0, hmm1, hmbl;
    u8  vdelp0, vdelp1, vdelbl;
    u8  resmp0, resmp1;
    u8  pos_p0, pos_p1, pos_m0, pos_m1, pos_bl;

    /* derived state */
    u32 pf;                 /* 20 playfield bits in display order */
    u8  gp0, gp1;           /* current player graphics incl. VDEL + reflect */
    u8  m0_on, m1_on, bl_on;
    const u8 *p0_mask, *p1_mask, *m0_mask, *m1_mask, *bl_mask;
    const u8 *prio;         /* 64-entry object->colour index table */
    u8  col_l[5], col_r[5]; /* colour per index for left/right half */
    u8  hmove_blank;

    u16 coll;               /* collision latches */

    /* input */
    u8  fire[2];            /* 1 = pressed (INPT4 / INPT5) */
    u8  fire_latch[2];
    s16 paddle[4];          /* charge time in scanlines, -1 = not connected */
    u32 dump_release_cc;

    /* audio */
    TiaAudioChannel ach[2];
    u8  *audio_buf;         /* 2 * TIA_MAX_LINES samples: sum of 2 ticks (0..30) per line */
    int audio_len;          /* samples produced in current frame */
    int audio_frame_len;    /* samples of last completed frame */
} Tia;

extern Tia tia;

void tia_init(u8 *framebuffer, u8 *audiobuffer);
void tia_reset(void);
u8   tia_read(u16 addr);
void tia_write(u16 addr, u8 val);
void tia_update(void);          /* render up to the current CPU cycle */

#endif
