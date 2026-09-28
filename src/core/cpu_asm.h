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
 *   a2 = opcode jump table, a3 = PC as host pointer (see below),
 *   a4 = AsmCpu, a5 = read map,
 *   a6 = ram_base. C, V, D, I live as bytes in AsmCpu (0 / non-zero).
 *
 * Memory access:
 *   - map[page] gives a pointer so that map[page] + (s16)addr is the byte
 *     at 'addr' (pointer biased by the sign-extended page address). 0 means
 *     "slow page": the access goes through rd()/wr() (TIA, RIOT, hotspots,
 *     cartridge RAM).
 *   - zero page $80-$FF and the stack go straight to RAM.
 *   - TIA writes ($00-$7F with A12=0, not for 3F carts) call tiawr()
 *     directly, bypassing bus decoding and bankswitch checks.
 *   - opcodes not implemented in assembler call step(), which executes a
 *     single instruction with the C core (cpu.c).
 *
 * Program counter: while code runs from a fast page, a3 points directly at
 * the next code byte and pcend at the end of that 256 byte page, so an
 * opcode fetch is just "cmp.l pcend,a3 / move.b (a3)+,d0". pcbias is the
 * map entry of that page, i.e. PC = (a3 - pcbias) & $FFFF. In slow mode
 * (code in a slow page, or after a jump) pcbias = pcend = 0 and a3 holds
 * the PC itself; the next fetch looks the page up again. Whenever the map
 * is rebuilt (bankswitch) the C side clears pcend to force that lookup.
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
    u8  tia_direct;                          /* 63: TIA writes go straight to tiawr() */
    u32 pcbias;                              /* 64: PC = (a3 - pcbias) & $FFFF */
    u32 pcend;                               /* 68: end of the fast code page, 0 = slow/invalid */
    u32 map[256];                            /* 72 */
    void (*tiawr)(u32 addr, u32 val);        /* 1096: TIA write fast path */
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
