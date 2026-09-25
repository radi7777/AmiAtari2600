/*
 * cpu_asm.c - C glue for the 68k assembler 6507 core.
 *
 * Slow-path callbacks keep a26_cycles in sync with the assembler's cycle
 * register (d5, mirrored in actx.cycles around each call), so the TIA,
 * RIOT and cartridge code sees exactly the same timing as with the C core.
 * A frame end (VSYNC -> a26_stop) or a JAM makes the core return by
 * pulling actx.target down to the current cycle.
 */
#include <stddef.h>
#include "cpu_asm.h"
#include "cpu.h"
#include "bus.h"

AsmCpu actx;
void (*cpu_asm_build_map)(AsmCpu *ctx);
int  (*cpu_asm_map_changed)(void);

/* compile-time layout checks (the assembler uses fixed offsets) */
typedef char check_flags[(sizeof(void *) != 4 || offsetof(AsmCpu, fc) == 52) ? 1 : -1];
typedef char check_tmp[(sizeof(void *) != 4 || offsetof(AsmCpu, tmp) == 58) ? 1 : -1];
typedef char check_pc[(sizeof(void *) != 4 || offsetof(AsmCpu, pcbias) == 64) ? 1 : -1];
typedef char check_map[(sizeof(void *) != 4 || offsetof(AsmCpu, map) == 72) ? 1 : -1];

/* only cartridge accesses (and, for 3F, TIA writes) can switch banks */
static void after_io(u32 addr)
{
    actx.cycles = a26_cycles;
    if (((addr & 0x1000) || actx.bank_on_tia) && cpu_asm_map_changed && cpu_asm_map_changed()) {
        cpu_asm_build_map(&actx);
        actx.pcend = 0;         /* code pointer may point into the old bank */
    }
    if (a26_stop || cpu.jammed)
        actx.target = actx.cycles;      /* leave the core after this instruction */
}

static u32 cb_read(u32 addr)
{
    u8 v;
    a26_cycles = actx.cycles;
    v = bus_read((u16)addr);
    after_io(addr);
    return v;
}

static void cb_write(u32 addr, u32 val)
{
    a26_cycles = actx.cycles;
    bus_write((u16)addr, (u8)val);
    after_io(addr);
}

/* execute one instruction with the C core */
static void cb_step(void)
{
    cpu.a = (u8)actx.a;
    cpu.x = (u8)actx.x;
    cpu.y = (u8)actx.y;
    cpu.s = (u8)actx.s;
    cpu.pc = (u16)actx.pc;
    cpu_set_p((u8)actx.p);
    a26_cycles = actx.cycles;
    cpu_run(a26_cycles + 1);
    actx.a = cpu.a;
    actx.x = cpu.x;
    actx.y = cpu.y;
    actx.s = cpu.s;
    actx.pc = cpu.pc;
    actx.p = cpu_get_p();
    after_io(0x1000);           /* the instruction may have touched anything */
}

u32 cpu_asm_run(u32 target)
{
    if (cpu.jammed) {
        a26_cycles = target;
        return 0;
    }
    actx.rd = cb_read;
    actx.wr = cb_write;
    actx.step = cb_step;
    actx.a = cpu.a;
    actx.x = cpu.x;
    actx.y = cpu.y;
    actx.s = cpu.s;
    actx.pc = cpu.pc;
    actx.p = cpu_get_p();
    actx.cycles = a26_cycles;
    actx.target = a26_stop ? a26_cycles : target;
    /* the map only changes on reset or bankswitch */
    if (cpu_asm_build_map && (actx.map_dirty ||
                              (cpu_asm_map_changed && cpu_asm_map_changed()))) {
        cpu_asm_build_map(&actx);
        actx.map_dirty = 0;
    }

    asmcpu_exec(&actx);

    cpu.a = (u8)actx.a;
    cpu.x = (u8)actx.x;
    cpu.y = (u8)actx.y;
    cpu.s = (u8)actx.s;
    cpu.pc = (u16)actx.pc;
    cpu_set_p((u8)actx.p);
    a26_cycles = actx.cycles;
    if (cpu.jammed) a26_cycles = target;
    return 0;
}
