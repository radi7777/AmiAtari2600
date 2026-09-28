/*
 * palette.h - TIA colour palettes (NTSC / PAL) as 24-bit RGB.
 *
 * Index = TIA colour value >> 1 (0..127).
 */
#ifndef A26_PALETTE_H
#define A26_PALETTE_H

#include "types.h"

extern const u32 palette_ntsc[128];
extern const u32 palette_pal[128];

/* 24-bit RGB -> Amiga OCS/ECS 12-bit $0RGB (rounded) */
u16 palette_to_rgb12(u32 rgb);

#endif
