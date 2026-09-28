/*
 * atari.c - system glue: reset, frame loop, input, region detection.
 *
 * Region detection: NTSC games produce ~262 scanlines per frame, PAL
 * games ~312. We average the line count of recent frames and switch
 * with hysteresis, so single odd frames (e.g. during bank switches or
 * loading screens) don't flip the Amiga display mode back and forth.
 */
#include "atari.h"
#include "bus.h"
#include "cpu.h"
#include "riot.h"
#ifdef A26_ASM_CPU
#include "cpu_asm.h"

static void build_asm_map(AsmCpu *c)
{
    cart_build_asm_map(c->map);
}

/* TIA write fast path for the asm core: no bus decoding and no bankswitch
 * check (TIA writes only switch banks on 3F carts, which use the normal
 * path). Parameters are u32 so vbcc and gcc agree on the calling convention. */
static void asm_tia_write(u32 addr, u32 val)
{
    a26_cycles = actx.cycles + 1;       /* the bus cycle of this write */
    a26_databus = (u8)val;
    tia_write((u16)addr, (u8)val);
    actx.cycles = a26_cycles;           /* WSYNC may have added cycles */
    if (a26_stop)
        actx.target = actx.cycles;
}
#endif

A26State a26;

/* u32 array: the TIA writes whole playfield blocks as aligned longwords */
static u32 framebuffer32[TIA_FB_LINES * TIA_WIDTH / 4];
#define framebuffer ((u8 *)framebuffer32)
static u8 audiobuffer[2 * TIA_MAX_LINES];

#define PAL_THRESHOLD   287     /* lines: >= PAL, < NTSC */
#define REGION_FRAMES   16      /* frames a new region must persist */

static int region_votes;

int a26_load(u8 *rom, u32 size, CartType type)
{
    int err = cart_init(rom, size, type);
    if (err) return err;
    a26.region_forced = 0;
    a26.region = REGION_NTSC;
    a26_reset();
    return 0;
}

void a26_reset(void)
{
    tia_init(framebuffer, audiobuffer);
    a26_stop = 0;
    cart_reset();
    riot_reset();
    tia_reset();
    cpu_reset();
#ifdef A26_ASM_CPU
    /* biased pointers: ram_base[$80..$FF] and stack_base[S] (S >= $80) */
    actx.ram_base = (u8 *)((unsigned long)riot.ram - 0x80);
    actx.stack_base = actx.ram_base;
    actx.mirror = 1;
    actx.map_dirty = 1;
    actx.bank_on_tia = (u8)cart_tia_hook;
    actx.tia_direct = (u8)!cart_tia_hook;
#ifdef A26_ASM_TIA
    {
        extern void tia_write_asm(u32 addr, u32 val);  /* src/amiga/tia_asm.s */
        actx.tiawr = tia_write_asm;
    }
#else
    actx.tiawr = asm_tia_write;
#endif
    cpu_asm_build_map = build_asm_map;
    cpu_asm_map_changed = cart_asm_map_changed;
#endif
    a26.lines_avg = (a26.region == REGION_PAL) ? 312 : 262;
    a26.frames = 0;
    region_votes = 0;
}

static void detect_region(void)
{
    int lines = tia.frame_lines;
    Region seen;

    if (lines < 200 || lines >= TIA_MAX_LINES) return;   /* no proper VSYNC */
    a26.lines_avg = (a26.lines_avg * 7 + lines) / 8;
    if (a26.region_forced) return;

    seen = (a26.lines_avg >= PAL_THRESHOLD) ? REGION_PAL : REGION_NTSC;
    if (seen != a26.region) {
        /* the very first frames decide quickly, later changes need to persist */
        if (++region_votes >= (a26.frames < 60 ? 4 : REGION_FRAMES)) {
            a26.region = seen;
            a26.region_changed = 1;
            region_votes = 0;
        }
    } else {
        region_votes = 0;
    }
}

void a26_run_frame(void)
{
    tia.frame_done = 0;
    while (!tia.frame_done) {
        a26_stop = 0;
        CPU_RUN(a26_cycles + 76u * 32u);
        tia_update();           /* may end the frame (max lines) */
    }
    a26_stop = 0;
    a26.frames++;
    detect_region();
}

void a26_set_joystick(int player, u8 bits)
{
    u8 nib = 0x0F;      /* active low: right, left, down, up = bit 3..0 */
    if (bits & JOY_RIGHT) nib &= (u8)~0x08;
    if (bits & JOY_LEFT)  nib &= (u8)~0x04;
    if (bits & JOY_DOWN)  nib &= (u8)~0x02;
    if (bits & JOY_UP)    nib &= (u8)~0x01;
    if (player == 0)
        riot.swcha_in = (u8)((riot.swcha_in & 0x0F) | (nib << 4));
    else
        riot.swcha_in = (u8)((riot.swcha_in & 0xF0) | nib);
    tia.fire[player & 1] = (bits & JOY_FIRE) ? 1 : 0;
}

void a26_set_switches(u8 bits)
{
    u8 v = 0x34;                        /* unused bits read as 1 on most units */
    if (!(bits & SW_RESET))  v |= 0x01;
    if (!(bits & SW_SELECT)) v |= 0x02;
    if (!(bits & SW_BW))     v |= 0x08;
    if (bits & SW_DIFF_P0)   v |= 0x40;
    if (bits & SW_DIFF_P1)   v |= 0x80;
    riot.swchb_in = v;
}

void a26_set_paddle(int n, int charge_lines)
{
    if (n >= 0 && n < 4) tia.paddle[n] = (s16)charge_lines;
}

void a26_force_region(int region)
{
    if (region < 0) {
        a26.region_forced = 0;
    } else {
        a26.region_forced = 1;
        if (a26.region != (Region)region) {
            a26.region = (Region)region;
            a26.region_changed = 1;
        }
    }
}

u8 *a26_framebuffer(void)
{
    return framebuffer;
}

int a26_audio(int ch, const u8 **samples)
{
    *samples = audiobuffer + (ch ? TIA_MAX_LINES : 0);
    return tia.audio_frame_len;
}
