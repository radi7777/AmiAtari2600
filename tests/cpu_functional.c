/*
 * cpu_functional.c - runs Klaus Dormann's 6502 functional test against
 * src/core/cpu.c using a flat 64K RAM bus.
 *
 * The test binary is not part of the repository (GPL-3.0, separate
 * project); tests/run_tests.sh downloads it:
 *   https://github.com/Klaus2m5/6502_65C02_functional_tests
 *
 * Success = PC reaches $3469 (the "success" trap of the standard build).
 */
#include <stdio.h>
#include <stdlib.h>
#include "../src/core/cpu.h"
#include "../src/core/bus.h"

u32 a26_cycles;
int a26_stop;
u8  a26_databus;
static u8 mem[65536];

u8 bus_read(u16 a) { a26_cycles++; return a26_databus = mem[a]; }
void bus_write(u16 a, u8 v) { a26_cycles++; mem[a] = a26_databus = v; }

int main(int argc, char **argv)
{
    FILE *f;
    u16 last = 0xFFFF;
    unsigned long steps = 0;
    if (argc < 2) { fprintf(stderr, "usage: %s 6502_functional_test.bin\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f || fread(mem, 1, 65536, f) != 65536) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    fclose(f);
    mem[0xFFFC] = 0x00; mem[0xFFFD] = 0x04;         /* start at $0400 */
    cpu_reset();
    for (;;) {
        cpu_run(a26_cycles + 1);                    /* one instruction */
        steps++;
        if (cpu.pc == last) break;                  /* trapped (JMP *) */
        last = cpu.pc;
        if (cpu.jammed) break;
    }
    if (cpu.pc == 0x3469) {
        printf("6502 functional test PASSED (%lu instructions, %lu cycles)\n", steps, (unsigned long)a26_cycles);
        return 0;
    }
    printf("6502 functional test FAILED: trapped at $%04X (A=%02X X=%02X Y=%02X P=%02X S=%02X)\n",
           cpu.pc, cpu.a, cpu.x, cpu.y, cpu_get_p(), cpu.s);
    return 1;
}
