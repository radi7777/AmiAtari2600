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
    /* ---- hot rendering state; longword aligned, fixed layout: the 68k
     * renderer (src/amiga/tia_asm.s) uses these offsets, checked in tia.c */
    u32 line_start_cc;      /*  0 absolute colour clock of current line start */
    u32 last_cc;            /*  4 rendered up to this colour clock */
    int line;               /*  8 scanline within current frame */
    u8  *fb;                /* 12 TIA_FB_LINES * TIA_WIDTH bytes */
    u32 pf;                 /* 16 20 playfield bits in display order */
    const u8 *p0_mask, *p1_mask, *m0_mask, *m1_mask, *bl_mask;   /* 20..36 */
    /* pixel runs (offset, length pairs relative to the object position)
     * where the object can draw: P0, P1, M0, M1, BL */
    const u8 *runs[5];      /* 40..56 */
    const u8 *prio;         /* 60 64-entry object->colour index table */
    int cur_first_visible;  /* 64 */
    int cur_last_visible;   /* 68 */
    u16 coll;               /* 72 collision latches */
    u8  nruns[5];           /* 74..78 */
    u8  gp0, gp1;           /* 79, 80 current player graphics incl. VDEL + reflect */
    u8  m0_on, m1_on, bl_on;    /* 81..83 */
    u8  col_l[5], col_r[5]; /* 84..93 colour per index for left/right half */
    u8  vblank;             /* 94 */
    u8  ctrlpf;             /* 95 */
    u8  colubk;             /* 96 */
    u8  hmove_blank;        /* 97 */
    u8  pos_p0, pos_p1, pos_m0, pos_m1, pos_bl;     /* 98..102 */
    u8  pad_;

    /* frame result */
    int frame_done;         /* set when a frame completed (VSYNC) */
    int frame_lines;        /* number of lines of the last completed frame */
    int first_visible;      /* first line with VBLANK off (last frame) */
    int last_visible;       /* last line with VBLANK off (last frame) */
    u32 frame_count;

    /* registers */
    u8  vsync;
    u8  nusiz0, nusiz1;
    u8  colup0, colup1, colupf;
    u8  refp0, refp1;
    u8  pf0, pf1, pf2;
    u8  grp0_new, grp0_old, grp1_new, grp1_old;
    u8  enam0, enam1, enabl_new, enabl_old;
    u8  hmp0, hmp1, hmm0, hmm1, hmbl;
    u8  vdelp0, vdelp1, vdelbl;
    u8  resmp0, resmp1;

    /* other derived state */
    u8  p0_nomain, p1_nomain;   /* reset this line: main copy not drawn yet */
    s8  hm_disp[5];             /* HMOVE displacement P0, P1, M0, M1, BL */
    u8  hm_pending;             /* late HMOVE: apply at the end of the line */
    u8  hm_w;                   /* CPU cycle of the last HMOVE in its line */
    u32 hm_line_cc;             /* start of that line */
    u8  hm_v[5];                /* extra clocks each object is due */
    u8  hm_lock[5];             /* locked in HMOVE (see hm_changed) */
    u8  lock_nruns[3];

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

/* framebuffer: TIA_FB_LINES * TIA_WIDTH bytes, must be 4-byte aligned */
void tia_init(u8 *framebuffer, u8 *audiobuffer);
void tia_reset(void);
u8   tia_read(u16 addr);
void tia_write(u16 addr, u8 val);
void tia_update(void);          /* render up to the current CPU cycle */

#endif
