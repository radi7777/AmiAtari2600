/*
 * cpu_asm.h - interface between the C side and the 68k assembler 6507
 * core (src/amiga/cpu6507.s). Only used when built with A26_ASM_CPU.
 *
 * The layout of AsmCpu is shared with the assembler source (offsets are
 * repeated there as equates) - change both together. All pointers are
 * 32 bit: the assembler core only exists for 68k targets.
 *
 * Register use inside the core:
 *   d2 = A, d3 = X, d4 = Y (zero-extended), d5 = cycle counter,
 *   d6 = N source byte, d7 = Z source byte (Z set when 0),
 *   a2 = opcode jump table, a3 = PC, a4 = AsmCpu, a5 = read map,
 *   a6 = ram_base. C, V, D, I live as bytes in AsmCpu (0 / non-zero).
 *
 * Memory access:
 *   - map[page] gives a pointer so that map[page] + (s16)addr is the byte
 *     at 'addr' (pointer biased by the sign-extended page address). 0 means
 *     "slow page": the access goes through rd()/wr() (TIA, RIOT, hotspots,
 *     cartridge RAM).
 *   - zero page $80-$FF and the stack go straight to RAM.
 *   - opcodes not implemented in assembler call step(), which executes a
 *     single instruction with the C core (cpu.c).
 */
#ifndef A26_CPU_ASM_H
#define A26_CPU_ASM_H

#include "types.h"

typedef struct {
    u32 a, x, y, s, pc, p, cycles, target;   /*  0 .. 28 */
    u32 (*rd)(u32 addr);                     /* 32 */
    void (*wr)(u32 addr, u32 val);           /* 36 */
    void (*step)(void);                      /* 40 */
    u8  *ram_base;                           /* 44: ram_base[$80..$FF] = RAM */
    u8  *stack_base;                         /* 48: stack_base[S] for S >= $80 */
    u8  fc, fv, fd, fi;                      /* 52: flags while running */
    u8  mirror;                              /* 56: 1 = 2600 RAM mirroring in slow path */
    u8  map_dirty;                           /* 57: C side: rebuild map[] before running */
    u16 tmp, tmp2;                           /* 58, 60: scratch */
    u8  bank_on_tia;                         /* 62: C side: TIA writes can switch banks (3F) */
    u8  pad1;
    u32 map[256];                            /* 64 */
} AsmCpu;

extern AsmCpu actx;

/* build map[] for the current memory configuration */
extern void (*cpu_asm_build_map)(AsmCpu *ctx);
/* return non-zero when the mapping changed (bankswitch) since the last build */
extern int  (*cpu_asm_map_changed)(void);

u32  cpu_asm_run(u32 target);           /* same contract as cpu_run() */

/* implemented in cpu6507.s */
void asmcpu_exec(AsmCpu *ctx);

#endif
