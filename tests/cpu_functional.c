/*
 * cpu_functional.c - runs Klaus Dormann's 6502 functional test against
 * the C core (src/core/cpu.c) or, built with -DA26_ASM_CPU, against the
 * 68k assembler core (src/amiga/cpu6507.s, under qemu-m68k), using a flat
 * 64K RAM bus.
 *
 * The test binary is not part of the repository (GPL-3.0, separate
 * project); tests/run_tests.sh downloads it:
 *   https://github.com/Klaus2m5/6502_65C02_functional_tests
 *
 * Success = the test traps (jumps to itself) at $3469.
 */
#include <stdio.h>
#include <stdlib.h>
#include "../src/core/cpu.h"
#include "../src/core/bus.h"
#ifdef A26_ASM_CPU
#include "../src/core/cpu_asm.h"
#endif

u32 a26_cycles;
int a26_stop;
u8  a26_databus;
static u8 mem[65536];

u8 bus_read(u16 a) { a26_cycles++; return a26_databus = mem[a]; }
void bus_write(u16 a, u8 v) { a26_cycles++; mem[a] = a26_databus = v; }

#ifdef A26_ASM_CPU
/* flat memory: every page readable directly, no 2600 mirroring */
static void flat_map(AsmCpu *c)
{
    int p;
    for (p = 0; p < 256; p++)
        c->map[p] = (u32)(unsigned long)(mem + (p << 8)) - (u32)(s32)(s16)(p << 8);
}
#endif


int main(int argc, char **argv)
{
    FILE *f;
    if (argc < 2) { fprintf(stderr, "usage: %s 6502_functional_test.bin\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f || fread(mem, 1, 65536, f) != 65536) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    fclose(f);
    mem[0xFFFC] = 0x00; mem[0xFFFD] = 0x04;         /* start at $0400 */
    cpu_reset();
#ifdef A26_ASM_CPU
    actx.ram_base = mem;
    actx.stack_base = mem + 0x100;
    actx.mirror = 0;
    actx.map_dirty = 1;
    cpu_asm_build_map = flat_map;
    cpu_asm_map_changed = NULL;
#endif
    /* run in chunks; the test traps with "jmp *" / "bne *", so a PC that
     * does not move over a whole chunk means we are stuck in a trap */
    for (;;) {
        u16 before = cpu.pc;
        CPU_RUN(a26_cycles + 1000);
        if (cpu.jammed || cpu.pc == before || a26_cycles > 200000000u) break;
    }
    if (cpu.pc == 0x3469) {
        printf("6502 functional test PASSED (%s core, %lu cycles)\n",
#ifdef A26_ASM_CPU
               "68k asm",
#else
               "C",
#endif
               (unsigned long)a26_cycles);
        return 0;
    }
    printf("6502 functional test FAILED: trapped at $%04X (A=%02X X=%02X Y=%02X P=%02X S=%02X)\n",
           cpu.pc, cpu.a, cpu.x, cpu.y, cpu_get_p(), cpu.s);
    return 1;
}
