/*
 * hw.c - system takeover and chipset control.
 *
 * The emulator runs with the OS "frozen": LoadView(NULL) + Forbid(), all
 * interrupts and DMA channels disabled, then only the DMA we need is
 * enabled again (bitplanes, copper, audio). Vertical sync is polled via
 * INTREQR (the VERTB request bit is set even with interrupts disabled).
 *
 * PAL/NTSC switching uses BEAMCON0 bit 5 (PAL), which exists on the ECS
 * 8372A Agnus (and AGA Alice). OCS Agnus chips (8370/8371) are fixed to
 * the mode they are wired for; on those we simply stay in native mode.
 */
#include <exec/execbase.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <proto/exec.h>
#include <proto/graphics.h>

#include "hw.h"

struct GfxBase *GfxBase;
HwInfo hwinfo;

static struct View *old_view;
static UWORD old_dmacon, old_intena, old_adkcon;
static int taken_over;

int hw_init(void)
{
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 33);
    if (!GfxBase) return -1;

    hwinfo.agnus_id = (hw->vposr >> 8) & 0x7F;
    /* bit 5 of the Agnus ID: ECS (8372A) or later; bit 1: AGA (Alice) */
    hwinfo.ecs = (hwinfo.agnus_id & 0x20) != 0;
    hwinfo.aga = hwinfo.ecs && (hwinfo.agnus_id & 0x02) != 0;
    hwinfo.native_pal = (GfxBase->DisplayFlags & PAL) != 0;
    return 0;
}

void hw_cleanup(void)
{
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
}

ULONG hw_lines(void)
{
    /* reading the high byte latches the counter until the low byte is read */
    ULONG hi = ciab->ciatodhi;
    ULONG mid = ciab->ciatodmid;
    ULONG lo = ciab->ciatodlow;
    return (hi << 16) | (mid << 8) | lo;
}

int hw_beam_line(void)
{
    return (int)((*(volatile ULONG *)0xDFF004 >> 8) & 0x1FF);
}

void hw_wait_lines(int n)
{
    while (n-- > 0) {
        UBYTE line = (UBYTE)(hw->vhposr >> 8);
        while ((UBYTE)(hw->vhposr >> 8) == line)
            ;
    }
}

void hw_clear_vbl(void)
{
    hw->intreq = INTF_VERTB;
}

int hw_vbl_pending(void)
{
    return (hw->intreqr & INTF_VERTB) != 0;
}

void hw_wait_vbl(void)
{
    hw_clear_vbl();
    while (!(hw->intreqr & INTF_VERTB))
        ;
    hw_clear_vbl();
}

void hw_takeover(void)
{
    int i;
    if (taken_over) return;

    old_view = GfxBase->ActiView;
    LoadView(NULL);
    WaitTOF();
    WaitTOF();
    OwnBlitter();
    WaitBlit();
    Forbid();

    old_dmacon = hw->dmaconr;
    old_intena = hw->intenar;
    old_adkcon = hw->adkconr;

    /* cut DMA in the vertical blank (poll the beam: VERTB may be
     * serviced by the OS interrupt handler, which is still running) */
    while (hw_beam_line() != 0x0C)
        ;
    hw->intena = 0x7FFF;
    hw->intreq = 0x7FFF;
    hw->dmacon = 0x7FFF;
    hw->adkcon = 0x7FFF;        /* no audio modulation, clean disk bits */

    /* silence sprites (their DMA is off, but data may still be latched) */
    for (i = 0; i < 8; i++) {
        hw->spr[i].ctl = 0;
        hw->spr[i].pos = 0;
        hw->spr[i].dataa = 0;
        hw->spr[i].datab = 0;
    }
    if (hwinfo.aga) {
        HW_REG(REG_FMODE) = 0;
        HW_REG(REG_BPLCON3) = 0x0C00;
    }
    taken_over = 1;
}

void hw_restore(void)
{
    if (!taken_over) return;

    hw->dmacon = 0x7FFF;
    hw->intena = 0x7FFF;
    hw->intreq = 0x7FFF;
    hw->adkcon = 0x7FFF;
    hw->aud[0].ac_vol = hw->aud[1].ac_vol = hw->aud[2].ac_vol = hw->aud[3].ac_vol = 0;

    if (hwinfo.ecs)
        hw->beamcon0 = hwinfo.native_pal ? 0x0020 : 0x0000;

    hw->cop1lc = (ULONG)GfxBase->copinit;
    hw->copjmp1 = 0;

    hw->adkcon = (UWORD)(0x8000 | old_adkcon);
    hw->dmacon = (UWORD)(DMAF_SETCLR | old_dmacon);
    hw->intena = (UWORD)(INTF_SETCLR | old_intena);

    Permit();
    DisownBlitter();
    LoadView(old_view);
    WaitTOF();
    WaitTOF();
    RethinkDisplay();
    taken_over = 0;
}

int hw_set_pal(int pal)
{
    if (!hwinfo.ecs)
        return hwinfo.native_pal;
    hw->beamcon0 = pal ? 0x0020 : 0x0000;
    return pal;
}
