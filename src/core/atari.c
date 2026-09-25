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

A26State a26;

static u8 framebuffer[TIA_FB_LINES * TIA_WIDTH];
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
        cpu_run(a26_cycles + 76u * 32u);
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
