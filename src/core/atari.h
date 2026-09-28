/*
 * atari.h - Atari 2600 system: public API used by the frontends
 * (Amiga and host test harness).
 *
 * Usage:
 *   a26_load(rom, size, CART_UNKNOWN);   // autodetect bankswitching
 *   for (;;) {
 *       a26_set_input(...);
 *       a26_run_frame();
 *       // a26_framebuffer(): TIA_FB_LINES x 160 colour bytes
 *       // tia.first_visible / tia.frame_lines: vertical layout
 *       // a26_audio(): per-scanline samples for both channels
 *   }
 */
#ifndef A26_ATARI_H
#define A26_ATARI_H

#include "types.h"
#include "cart.h"
#include "tia.h"

typedef enum { REGION_NTSC = 0, REGION_PAL = 1 } Region;

/* joystick bits for a26_set_joystick() (1 = active) */
#define JOY_UP    0x01
#define JOY_DOWN  0x02
#define JOY_LEFT  0x04
#define JOY_RIGHT 0x08
#define JOY_FIRE  0x10

/* console switches for a26_set_switches() (1 = active/pressed) */
#define SW_RESET   0x01
#define SW_SELECT  0x02
#define SW_BW      0x04     /* black & white instead of colour */
#define SW_DIFF_P0 0x08     /* left difficulty A (pro) */
#define SW_DIFF_P1 0x10     /* right difficulty A (pro) */

typedef struct {
    Region region;          /* current (detected or forced) region */
    int    region_forced;   /* 1 = do not autodetect */
    int    region_changed;  /* set when detection switched region; frontend clears */
    int    lines_avg;       /* averaged scanlines per frame */
    u32    frames;
} A26State;

extern A26State a26;

int  a26_load(u8 *rom, u32 size, CartType type);  /* 0 = ok, also resets */
void a26_reset(void);
void a26_run_frame(void);

void a26_set_joystick(int player, u8 bits);
void a26_set_switches(u8 bits);
void a26_set_paddle(int n, int charge_lines);     /* -1 = disconnected */
void a26_force_region(int region);                 /* -1 = autodetect */

u8  *a26_framebuffer(void);
/* samples for channel ch (0/1) of the last frame; returns count.
 * Each sample is the sum of two TIA audio ticks, range 0..30. */
int  a26_audio(int ch, const u8 **samples);

#endif
