/*
 * cpu.h - MOS 6507 (NMOS 6502 core, 13 address lines) emulation.
 */
#ifndef A26_CPU_H
#define A26_CPU_H

#include "types.h"

typedef struct {
    u16 pc;
    u8  a, x, y, s;
    u8  p;          /* only valid after cpu_get_p(); flags are kept unpacked */
    u8  jammed;     /* set when a KIL/JAM opcode was executed */
} Cpu;

extern Cpu cpu;

void cpu_reset(void);
/* Run instructions until a26_cycles reaches 'target' or a stop is
 * requested (a26_stop != 0). Returns the number of instructions run. */
u32  cpu_run(u32 target);
u8   cpu_get_p(void);
void cpu_set_p(u8 p);

/* the frame loop calls CPU_RUN: the C core, or the 68k assembler core
 * (src/amiga/cpu6507.s) when built with -DA26_ASM_CPU */
#ifdef A26_ASM_CPU
u32  cpu_asm_run(u32 target);
#define CPU_RUN cpu_asm_run
#else
#define CPU_RUN cpu_run
#endif

#endif
