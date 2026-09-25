/*
 * bus.h - 6507 address bus of the Atari 2600.
 *
 * Every bus_read()/bus_write() costs exactly one CPU cycle; the CPU core
 * adds the remaining (internal/dummy) cycles itself. a26_cycles is the
 * master clock of the whole machine (1 CPU cycle = 3 TIA colour clocks).
 *
 * Address decoding (A12..A0):
 *   A12=1               -> cartridge ($1000-$1FFF)
 *   A12=0, A7=0         -> TIA
 *   A12=0, A7=1, A9=0   -> RIOT RAM (128 bytes)
 *   A12=0, A7=1, A9=1   -> RIOT I/O + timer
 */
#ifndef A26_BUS_H
#define A26_BUS_H

#include "types.h"

extern u32 a26_cycles;      /* CPU cycle counter (wraps, use differences) */
extern int a26_stop;        /* request cpu_run() to return early */
extern u8  a26_databus;     /* last value seen on the data bus */

u8   bus_read(u16 addr);
void bus_write(u16 addr, u8 val);

#endif
