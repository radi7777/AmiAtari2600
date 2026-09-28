/*
 * vidconv.h - TIA line -> Amiga bitplanes + per-line palette (portable C).
 *
 * The TIA can show any of 128 colours per pixel, but a single scanline
 * rarely uses more than a handful. The Amiga runs a 32-colour lowres
 * screen split into two banks of 16 registers: even display lines use
 * COLOR00-15, odd lines COLOR16-31 (bitplane 5 is constant per line and
 * selects the bank). The copper loads the colours of a line during the
 * line before, which shows the other bank, so there is a whole raster
 * line of copper time for up to 15 colour loads.
 *
 * Register 0 of each bank always holds black (COLOR00 is also the border
 * colour), the other 15 are assigned per line in order of appearance.
 * A converted line therefore depends only on its own 160 TIA pixels and
 * its bank: if a line is unchanged since the buffer was last drawn, both
 * its bitplanes and its copper moves are still correct and the line can
 * be skipped entirely.
 *
 * Lines with more than 16 colours (rare) use the nearest loaded colour
 * for the extra ones.
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
#define VC_BANK_COLOURS 16
#define VC_MAX_MOVES    (VC_BANK_COLOURS - 1)       /* colour loads per line */

/* select the palette used for TIA colour -> RGB12 (region dependent) */
void vc_set_palette(const u32 *palette24);

#define VC_ROW_LONGS    (VC_ROW_BYTES / 4)
#define VC_LINE_LONGS   (4 * VC_ROW_LONGS)          /* planes 1-4 of a line */

/* Convert one TIA line (160 colour bytes) for display bank 0 or 1.
 *   moves:  receives copper MOVEs as longwords (register << 16 | $0RGB),
 *           one per colour register used besides black
 *   planes: receives planes 1-4, VC_ROW_LONGS longwords each (plane 5 is
 *           the constant bank select and is set up by the caller)
 * Returns the number of moves. */
int  vc_convert_line(const u8 *tia_line, int bank, u32 *moves, u32 *planes);

/* like vc_convert_line, but only computes the register index (0..15)
 * of every pixel; for tests */
int  vc_map_line(const u8 *tia_line, int bank, u32 *moves, u8 *slots);

/* chunky (160 register indices 0..15) -> planes 1-4 (plane-major
 * longwords, see vc_convert_line), each TIA pixel doubled horizontally */
void vc_c2p_line(const u8 *slots, u32 *planes);

/* statistics (for tuning and tests) */
extern u32 vc_stat_fallbacks;   /* pixels drawn with a substitute colour */

#endif
