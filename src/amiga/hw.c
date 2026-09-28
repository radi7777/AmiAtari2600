/*
 * hw.c - system takeover and chipset control.
 *
 * Two modes:
 *   system friendly (default): the OS keeps running its interrupts, so
 *     timers, keyboard and network survive. We take the display with
 *     LoadView(NULL) and our own copper list, run at a raised task
 *     priority, and count vertical blanks with our own interrupt server.
 *   kill OS (KILLOS): Forbid(), all interrupts and DMA off, then only the
 *     DMA we need back on; vertical sync is polled via INTREQR. Saves the
 *     OS interrupt load on slow machines.
 *
 * PAL/NTSC switching uses BEAMCON0 bit 5 (PAL), which exists on the ECS
 * 8372A Agnus (and AGA Alice). OCS Agnus chips (8370/8371) are fixed to
 * the mode they are wired for; on those we simply stay in native mode.
 */
#define __USE_NEW_TIMEVAL__     /* vbcc cannot parse the anonymous unions */
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <devices/timer.h>
#include <dos/dosextens.h>
#include <hardware/intbits.h>
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
static int killed_os;
static BYTE old_pri;

/* vertical blank counter, incremented by our VERTB interrupt server
 * (irq.s), which also signals the emulator task */
static volatile struct {
    ULONG count;
    struct Task *task;
    ULONG sigmask;
} vbl;
#define vbl_count (vbl.count)
static BYTE vbl_sig = -1;

/* microsecond timer for sleeping until a raster line */
static struct MsgPort *tmr_port;
static struct TimeRequest *tmr_req;
static int tmr_open;
int hw_no_timer;
extern int hw_is_pal;

/* ---- PC sampling profiler (see irq.s) ---- */
ULONG prof_base, prof_size, prof_other;
ULONG *prof_hist;
extern void prof_handler(void);
extern ULONG prof_get_vbr(void);
static APTR *prof_vbr;
static APTR prof_old_vec;

int hw_profile_start(void)
{
    struct Process *pr = (struct Process *)FindTask(NULL);
    struct CommandLineInterface *cli = (struct CommandLineInterface *)BADDR(pr->pr_CLI);
    ULONG *seg;
    if (!killed_os || !cli || !cli->cli_Module) return -1;
    seg = (ULONG *)BADDR(cli->cli_Module);      /* first hunk: CODE */
    prof_base = (ULONG)(seg + 1);
    prof_size = seg[-1] - 8;                     /* hunk size incl. header */
    prof_hist = (ULONG *)AllocMem((prof_size / 16 + 1) * 4, MEMF_ANY | MEMF_CLEAR);
    if (!prof_hist) return -1;
    prof_other = 0;
    prof_vbr = (APTR *)Supervisor((ULONG (*)())prof_get_vbr);
    prof_old_vec = prof_vbr[30];                 /* level 6 autovector */
    prof_vbr[30] = (APTR)prof_handler;
    CacheClearU();
    /* CIA-B timer A, continuous, ~2 kHz (E clock ~709 kHz) */
    ciab->ciacra = 0;
    ciab->ciaicr = 0x7F;
    ciab->ciatalo = (UBYTE)(355 & 0xFF);
    ciab->ciatahi = (UBYTE)(355 >> 8);
    ciab->ciaicr = 0x81;
    ciab->ciacra = 0x11;                         /* start, force load, continuous */
    hw->intreq = INTF_EXTER;
    hw->intena = INTF_SETCLR | INTF_INTEN | INTF_EXTER;
    return 0;
}

void hw_profile_stop(void)
{
    if (!prof_vbr) return;
    hw->intena = INTF_EXTER | INTF_INTEN;
    ciab->ciacra = 0;
    ciab->ciaicr = 0x01;
    hw->intreq = INTF_EXTER;
    prof_vbr[30] = prof_old_vec;
    CacheClearU();
    prof_vbr = NULL;
}
static ULONG vbl_seen;
static struct Interrupt vbl_int;

/* irq.s: increments *is_Data and returns with Z set */
extern void hw_vbl_server(void);

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

static ULONG read_tod(void)
{
    /* reading the high byte latches the counter until the low byte is read */
    ULONG hi = ciab->ciatodhi;
    ULONG mid = ciab->ciatodmid;
    ULONG lo = ciab->ciatodlow;
    return (hi << 16) | (mid << 8) | lo;
}

ULONG hw_lines(void)
{
    ULONG a, b;
    if (taken_over && !killed_os) {
        /* the OS reads the TOD itself, which breaks its latch: count
         * vertical blanks and add the beam position instead (long frames:
         * 313 lines PAL, 263 NTSC) */
        ULONG v, line;
        do {
            v = vbl_count;
            line = (ULONG)hw_beam_line();
        } while (v != vbl_count);
        return (v * (hw_is_pal ? 313 : 263) + line) & 0xFFFFFF;
    }
    /* the latch does not always protect against a carry into the middle
     * byte (seen as jumps of 256 lines): accept two reads that agree */
    a = read_tod();
    b = read_tod();
    while (((b - a) & 0xFFFFFF) > 1) {
        a = b;
        b = read_tod();
    }
    return b;
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
    if (killed_os)
        hw->intreq = INTF_VERTB;
    else
        vbl_seen = vbl_count;
}

int hw_vbl_pending(void)
{
    if (killed_os)
        return (hw->intreqr & INTF_VERTB) != 0;
    return vbl_count != vbl_seen;
}

void hw_wait_vbl(void)
{
    hw_clear_vbl();
    if (!killed_os && vbl_sig >= 0) {
        /* sleep: other tasks (network, ...) can run meanwhile */
        while (!hw_vbl_pending())
            Wait(1UL << vbl_sig);
    } else {
        while (!hw_vbl_pending())
            ;
    }
    hw_clear_vbl();
}

void hw_wait_line(int line)
{
    int now = hw_beam_line();
    if (line <= now || hw_vbl_pending())
        return;
    /* sleep for most of the time (a raster line is ~64 us), then poll the
     * beam for the last few lines */
    if (!killed_os && tmr_open && line - now > 8) {
        tmr_req->tr_node.io_Command = TR_ADDREQUEST;
        tmr_req->tr_time.tv_secs = 0;
        tmr_req->tr_time.tv_micro = (ULONG)(line - now - 6) * 64;
        DoIO((struct IORequest *)tmr_req);
    }
    while (hw_beam_line() < line && !hw_vbl_pending())
        ;
}

void hw_takeover(int kill_os)
{
    int i;
    if (taken_over) return;

    old_view = GfxBase->ActiView;
    LoadView(NULL);
    WaitTOF();
    WaitTOF();
    OwnBlitter();
    WaitBlit();

    old_dmacon = hw->dmaconr;
    old_intena = hw->intenar;
    old_adkcon = hw->adkconr;
    killed_os = kill_os;

    if (!kill_os) {
        struct Task *me = FindTask(NULL);
        vbl_int.is_Node.ln_Type = NT_INTERRUPT;
        vbl_int.is_Node.ln_Pri = 0;
        vbl_int.is_Node.ln_Name = (char *)"A26 vblank";
        vbl_sig = AllocSignal(-1);
        vbl.task = me;
        vbl.sigmask = vbl_sig >= 0 ? 1UL << vbl_sig : 0;
        vbl_int.is_Data = (APTR)&vbl;
        vbl_int.is_Code = hw_vbl_server;
        AddIntServer(INTB_VERTB, &vbl_int);
        tmr_port = CreateMsgPort();
        tmr_req = tmr_port ? (struct TimeRequest *)CreateIORequest(tmr_port, sizeof(struct TimeRequest)) : NULL;
        tmr_open = !hw_no_timer && tmr_req && !OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tmr_req, 0);
        /* above normal tasks, below input.device and the network stack */
        old_pri = SetTaskPri(me, 1);
        hw->dmacon = DMAF_SPRITE | DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
        hw->adkcon = 0x00FF;    /* no audio modulation (disk bits untouched) */
        goto sprites;
    }
    Forbid();

    /* cut DMA in the vertical blank (poll the beam: VERTB may be
     * serviced by the OS interrupt handler, which is still running) */
    while (hw_beam_line() != 0x0C)
        ;
    hw->intena = 0x7FFF;
    hw->intreq = 0x7FFF;
    hw->dmacon = 0x7FFF;
    hw->adkcon = 0x7FFF;        /* no audio modulation, clean disk bits */

sprites:
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

    if (!killed_os) {
        hw->dmacon = DMAF_RASTER | DMAF_COPPER | DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
        hw->aud[0].ac_vol = hw->aud[1].ac_vol = hw->aud[2].ac_vol = hw->aud[3].ac_vol = 0;
        RemIntServer(INTB_VERTB, &vbl_int);
        vbl.sigmask = 0;
        if (vbl_sig >= 0) { FreeSignal(vbl_sig); vbl_sig = -1; }
        if (tmr_open) CloseDevice((struct IORequest *)tmr_req);
        if (tmr_req) DeleteIORequest((struct IORequest *)tmr_req);
        if (tmr_port) DeleteMsgPort(tmr_port);
        tmr_open = 0;
        tmr_req = NULL;
        tmr_port = NULL;
        SetTaskPri(FindTask(NULL), old_pri);
        if (hwinfo.ecs)
            hw->beamcon0 = hwinfo.native_pal ? 0x0020 : 0x0000;
        hw->cop1lc = (ULONG)GfxBase->copinit;
        hw->copjmp1 = 0;
        hw->dmacon = (UWORD)(DMAF_SETCLR | (old_dmacon & (DMAF_RASTER | DMAF_COPPER | DMAF_SPRITE)));
        goto finish;
    }

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
finish:
    DisownBlitter();
    LoadView(old_view);
    WaitTOF();
    WaitTOF();
    taken_over = 0;
}

int hw_is_pal;

int hw_set_pal(int pal)
{
    if (!hwinfo.ecs) {
        hw_is_pal = hwinfo.native_pal;
        return hw_is_pal;
    }
    hw->beamcon0 = pal ? 0x0020 : 0x0000;
    hw_is_pal = pal;
    return pal;
}
