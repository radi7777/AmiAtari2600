/*
 * riot.h - MOS 6532 RIOT: 128 bytes RAM, interval timer, two I/O ports.
 *
 * Port A (SWCHA): joysticks, active low
 *   bit 7..4 = P0 right, left, down, up
 *   bit 3..0 = P1 right, left, down, up
 * Port B (SWCHB): console switches
 *   bit 0 = RESET (0 = pressed), bit 1 = SELECT (0 = pressed),
 *   bit 3 = colour (1) / B&W (0), bit 6 = P0 difficulty (1 = A/pro),
 *   bit 7 = P1 difficulty (1 = A/pro)
 */
#ifndef A26_RIOT_H
#define A26_RIOT_H

#include "types.h"

typedef struct {
    u8  ram[128];
    u8  swcha_in;       /* external joystick lines (set by frontend) */
    u8  swchb_in;       /* external console switches (set by frontend) */
    u8  swacnt, swbcnt; /* data direction registers */
    u8  swa_out, swb_out;
    /* timer: value is computed lazily from the cycle counter */
    u32 timer_set_cycle;
    s32 timer_start;    /* (value << shift) at the time of writing */
    u8  timer_shift;    /* 0, 3, 6, 10 for 1, 8, 64, 1024 */
} Riot;

extern Riot riot;

void riot_reset(void);
u8   riot_read(u16 addr);
void riot_write(u16 addr, u8 val);

#endif
