/*
 * hw.h - direct Amiga custom chip / CIA access and system takeover.
 */
#ifndef A26_AMIGA_HW_H
#define A26_AMIGA_HW_H

#include <exec/types.h>
#include <hardware/custom.h>
#include <hardware/cia.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>

#define hw    ((volatile struct Custom *)0xDFF000)
#define ciaa  ((volatile struct CIA *)0xBFE001)
#define ciab  ((volatile struct CIA *)0xBFD000)

/* registers that are not in every NDK's struct Custom */
#define HW_REG(off) (*(volatile UWORD *)(0xDFF000 + (off)))
#define REG_BPLCON3 0x106
#define REG_FMODE   0x1FC

/* copper register offsets */
#define COP_BPL1PTH 0x0E0
#define COP_COLOR00 0x180
#define COP_NOOP    0x1FE

typedef struct {
    int agnus_id;       /* VPOSR bits 14..8 */
    int ecs;            /* 8372A (or AGA Alice): BEAMCON0 available */
    int aga;
    int native_pal;     /* display mode the machine booted in */
} HwInfo;

extern HwInfo hwinfo;

int  hw_init(void);             /* open graphics.library, detect chipset */
/* take the display; kill_os = also stop OS interrupts and DMA (see hw.c) */
void hw_takeover(int kill_os);
void hw_restore(void);          /* give everything back to the OS */
void hw_cleanup(void);          /* close libraries */

/* switch display timing via BEAMCON0 (ECS/AGA only); returns the mode in use */
int  hw_set_pal(int pal);

void hw_wait_vbl(void);         /* wait for the next vertical blank */
void hw_wait_line(int line);
extern int hw_no_timer;         /* diagnostics: never sleep in timer.device */    /* wait until the beam reaches line (this frame) */
int  hw_vbl_pending(void);      /* vertical blank happened since last clear */
void hw_clear_vbl(void);
void hw_wait_lines(int n);      /* busy-wait n raster lines (~64 us each) */
int  hw_beam_line(void);        /* current vertical beam position */
/* free-running raster line counter (CIA-B TOD, counts HSYNC, 24 bit).
 * One line = 64 us (PAL) / 63.5 us (NTSC). Use hw_lines_since(). */
ULONG hw_lines(void);
#define hw_lines_since(t) ((hw_lines() - (t)) & 0xFFFFFF)

#endif
