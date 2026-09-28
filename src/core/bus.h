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

/* Called right before the emulated program reads the joystick ports
 * (SWCHA, INPT4/INPT5), so a frontend can sample its input as late as
 * possible. May be NULL. */
extern void (*a26_input_hook)(void);

/* Code bytes at the PC following the instruction that is currently
 * reading (NULL if unknown); *pc receives that PC. Set by the CPU core,
 * used by the RIOT to skip timer wait loops. */
extern const u8 *(*a26_next_code)(u16 *pc);

u8   bus_read(u16 addr);
void bus_write(u16 addr, u8 val);

#endif
