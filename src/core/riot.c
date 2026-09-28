/*
 * riot.c - MOS 6532 RIOT emulation.
 *
 * The interval timer is evaluated lazily: we remember the cycle of the
 * last timer write and compute INTIM on demand. After writing N with
 * interval I in cycle w, a read in cycle r > w returns
 * N - 1 - (r - w - 1) / I; after passing zero the timer wraps to $FF,
 * sets the interrupt flag and decrements once per cycle. Reading INTIM
 * clears the flag again (except in the cycle it reads $FF). This follows
 * gopher2600, which is verified against hardware test ROMs.
 */
#include "riot.h"
#include "bus.h"

Riot riot;
int  riot_clean_start;

void riot_reset(void)
{
    int i;
    /* RAM powers up with random-ish contents on real hardware; a fixed
     * pattern keeps runs reproducible. */
    for (i = 0; i < 128; i++)
        riot.ram[i] = riot_clean_start ? 0 : (u8)(i * 0x6B + 0x35);
    riot.swcha_in = 0xFF;
    riot.swchb_in = 0x0B;           /* colour, no reset/select, difficulty B */
    riot.swacnt = riot.swbcnt = 0;
    riot.swa_out = riot.swb_out = 0;
    riot.timer_set_cycle = a26_cycles;
    riot.timer_start = (s32)((u32)((a26_cycles * 7u) & 0xFF) << 10); /* arbitrary start */
    riot.timer_shift = 10;
    riot.timint_cleared = 0;
    riot.pa7_flag = 1;
    if (riot_clean_start)
        riot.timer_start = 1024;    /* INTIM = 0 for 1024 cycles, as gopher2600 */
}

static s32 timer_value(void)
{
    return riot.timer_start - (s32)(a26_cycles - riot.timer_set_cycle);
}

u8 riot_read(u16 addr)
{
    if (!(addr & 0x0200))
        return riot.ram[addr & 0x7F];

    switch (addr & 0x07) {
    case 0x00:  /* SWCHA */
        if (a26_input_hook) a26_input_hook();
        return (u8)((riot.swcha_in & ~riot.swacnt) | (riot.swa_out & riot.swacnt));
    case 0x01:  /* SWACNT */
        return riot.swacnt;
    case 0x02:  /* SWCHB */
        return (u8)((riot.swchb_in & ~riot.swbcnt) | (riot.swb_out & riot.swbcnt));
    case 0x03:  /* SWBCNT */
        return riot.swbcnt;
    case 0x04:
    case 0x06: { /* INTIM */
        s32 t = timer_value();
        if (t >= 0)
            return (u8)(t >> riot.timer_shift);
        if (t < -1)
            riot.timint_cleared = 1;
        return (u8)t;
    }
    default: {   /* 0x05/0x07: TIMINT: bit 7 timer flag, bit 6 PA7 edge */
        s32 t = timer_value();
        u8 v = (u8)((t < 0 && !riot.timint_cleared) ? 0x80 : 0x00);
        if (riot.pa7_flag) v |= 0x40;
        riot.pa7_flag = 0;
        return v;
    }
    }
}

void riot_write(u16 addr, u8 val)
{
    if (!(addr & 0x0200)) {
        riot.ram[addr & 0x7F] = val;
        return;
    }
    if (addr & 0x0010) {
        /* $294-$297 (A4=1): timer write, A1..A0 = interval */
        if (addr & 0x0004) {
            static const u8 shifts[4] = { 0, 3, 6, 10 };
            riot.timer_shift = shifts[addr & 0x03];
            riot.timer_start = (s32)val << riot.timer_shift;
            riot.timer_set_cycle = a26_cycles;
            riot.timint_cleared = 0;
        }
        /* $290-$293 with A4=1, A2=0: edge detect control (unused) */
        return;
    }
    switch (addr & 0x03) {
    case 0x00: riot.swa_out = val; break;
    case 0x01: riot.swacnt = val; break;
    case 0x02: riot.swb_out = val; break;
    case 0x03: riot.swbcnt = val; break;
    }
}
