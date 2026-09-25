/*
 * vidconv.h - TIA line -> Amiga bitplanes + per-line palette (portable C).
 *
 * The TIA can show any of 128 colours per pixel, but a single scanline
 * rarely uses more than a handful. We run a 32-colour lowres screen and
 * treat the 32 colour registers as a cache: for every line the colours it
 * needs are looked up; missing ones are loaded into a free register by a
 * copper MOVE at the start of that line (evicting the least recently used
 * register not needed by the line itself).
 *
 * The copper only has time for a limited number of MOVEs in the
 * horizontal blank before the display fetch starts, so at most
 * VC_MAX_MOVES new colours are loaded per line; beyond that (and when all
 * 32 registers are busy on the same line) the nearest loaded colour is
 * used instead.
 *
 * Nothing in here touches Amiga hardware, so it is unit-tested on the host
 * (tests/vidconv_test.c).
 */
#ifndef A26_VIDCONV_H
#define A26_VIDCONV_H

#include "../core/types.h"

#define VC_WIDTH        160     /* TIA pixels per line */
#define VC_PLANES       5
#define VC_ROW_BYTES    40      /* 320 lowres pixels / 8 */
#define VC_LINE_STRIDE  (VC_PLANES * VC_ROW_BYTES)  /* interleaved bitplanes */
#define VC_MAX_MOVES    10      /* colour loads per line (copper budget) */

typedef struct {
    u16 reg;        /* colour register number 0..31 */
    u16 rgb;        /* $0RGB */
} VcMove;

/* select the palette used for TIA colour -> RGB12 (region dependent) */
void vc_set_palette(const u32 *palette24);
/* reset colour register allocation at the start of each frame */
void vc_begin_frame(void);
/* map one TIA line (160 colour bytes) to register indices.
 * Returns the number of copper moves required before this line. */
int  vc_map_line(const u8 *tia_line, u8 *slots, VcMove *moves);
/* chunky (160 register indices) -> 5 interleaved bitplanes, each TIA pixel
 * doubled horizontally. dst points to the line's first plane row. */
void vc_c2p_line(const u8 *slots, u8 *dst);

/* statistics of the last frame (for tuning) */
extern u32 vc_stat_fallbacks;   /* pixels drawn with a substitute colour */
extern u32 vc_stat_moves;       /* copper colour loads */

#endif
