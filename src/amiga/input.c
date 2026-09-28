/*
 * input.c - joysticks and keyboard.
 *
 * Joystick: JOYxDAT counters decode to directions, fire buttons are on
 * CIA-A port A (bit 6 = port 0, bit 7 = port 1, active low). Read
 * directly in both modes.
 *
 * Keyboard, system friendly mode: an input.device handler at high
 * priority records raw key and mouse events and swallows them, so the
 * Workbench behind the game sees nothing.
 *
 * Keyboard, kill-OS mode: with interrupts disabled we poll the CIA-A
 * serial port. Each received byte must be acknowledged by pulling KDAT low
 * for at least 85 us (serial port to output mode), otherwise the keyboard
 * controller resyncs and drops keys.
 */
#include <exec/interrupts.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <proto/exec.h>

#include "hw.h"
#include "input.h"
#include "../core/atari.h"

static u8 keys[128];
static u8 keys_prev[128];
static volatile u8 keys_irq[128];       /* written by the input handler */
static int use_handler;
static int kill_mode;

static struct MsgPort *in_port;
static struct IOStdReq *in_req;
static struct Interrupt in_handler;

static struct InputEvent *handler_code(__reg("a0") struct InputEvent *events,
                                       __reg("a1") APTR data)
{
    struct InputEvent *e;
    (void)data;
    for (e = events; e; e = e->ie_NextEvent) {
        if (e->ie_Class == IECLASS_RAWKEY) {
            UWORD c = e->ie_Code;
            if ((c & 0x7F) < 0x78)
                keys_irq[c & 0x7F] = (c & IECODE_UP_PREFIX) ? 0 : 1;
            e->ie_Class = IECLASS_NULL;
        } else if (e->ie_Class == IECLASS_RAWMOUSE) {
            e->ie_Class = IECLASS_NULL;
        }
    }
    return events;
}

void input_init(int kill_os)
{
    int i;
    for (i = 0; i < 128; i++) keys[i] = keys_prev[i] = keys_irq[i] = 0;
    kill_mode = kill_os;
    ciaa->ciaddra &= (UBYTE)~(CIAF_GAMEPORT0 | CIAF_GAMEPORT1);   /* fire lines = input */

    use_handler = 0;
    if (kill_os) return;        /* 1: poll the CIA, -1: no keyboard at all */
    in_port = CreateMsgPort();
    if (!in_port) return;
    in_req = (struct IOStdReq *)CreateIORequest(in_port, sizeof(struct IOStdReq));
    if (!in_req) { DeleteMsgPort(in_port); in_port = NULL; return; }
    if (OpenDevice((CONST_STRPTR)"input.device", 0, (struct IORequest *)in_req, 0)) {
        DeleteIORequest((struct IORequest *)in_req);
        DeleteMsgPort(in_port);
        in_req = NULL;
        in_port = NULL;
        return;
    }
    in_handler.is_Node.ln_Type = NT_INTERRUPT;
    in_handler.is_Node.ln_Pri = 100;            /* before Intuition (50) */
    in_handler.is_Node.ln_Name = (char *)"A26 input";
    in_handler.is_Data = NULL;
    in_handler.is_Code = (void (*)())handler_code;
    in_req->io_Command = IND_ADDHANDLER;
    in_req->io_Data = (APTR)&in_handler;
    DoIO((struct IORequest *)in_req);
    use_handler = 1;
}

void input_cleanup(void)
{
    if (!in_req) return;
    if (use_handler) {
        in_req->io_Command = IND_REMHANDLER;
        in_req->io_Data = (APTR)&in_handler;
        DoIO((struct IORequest *)in_req);
        use_handler = 0;
    }
    CloseDevice((struct IORequest *)in_req);
    DeleteIORequest((struct IORequest *)in_req);
    DeleteMsgPort(in_port);
    in_req = NULL;
    in_port = NULL;
}

static void poll_cia(void)
{
    int i;
    /* several bytes may arrive per frame; the CIA holds one at a time,
     * so poll a few times while waiting for the handshake */
    for (i = 0; i < 8; i++) {
        UBYTE icr = ciaa->ciaicr;           /* reading clears the flags */
        UBYTE code;
        if (!(icr & CIAICRF_SP)) break;
        code = ciaa->ciasdr;
        ciaa->ciacra |= CIACRAF_SPMODE;     /* acknowledge: KDAT low */
        hw_wait_lines(2);                   /* >= 85 us */
        ciaa->ciacra &= (UBYTE)~CIACRAF_SPMODE;
        code = (UBYTE)~code;
        code = (UBYTE)((code >> 1) | (code << 7));
        if ((code & 0x7F) < 0x78)           /* ignore special codes */
            keys[code & 0x7F] = (code & 0x80) ? 0 : 1;
        hw_wait_lines(1);
    }
}

void input_poll(void)
{
    int i;
    for (i = 0; i < 128; i++) keys_prev[i] = keys[i];
    if (use_handler) {
        for (i = 0; i < 128; i++) keys[i] = keys_irq[i];
    } else if (kill_mode > 0) {
        poll_cia();
    }
}

int key_down(int code)
{
    return keys[code & 0x7F];
}

int key_pressed(int code)
{
    return keys[code & 0x7F] && !keys_prev[code & 0x7F];
}

u8 joy_read(int port)
{
    UWORD d = port ? hw->joy1dat : hw->joy0dat;
    UBYTE fire = port ? CIAF_GAMEPORT1 : CIAF_GAMEPORT0;
    u8 bits = 0;

    if (d & 0x0002) bits |= JOY_RIGHT;
    if (d & 0x0200) bits |= JOY_LEFT;
    if (((d >> 1) ^ d) & 0x0001) bits |= JOY_DOWN;
    if (((d >> 9) ^ (d >> 8)) & 0x0001) bits |= JOY_UP;
    if (!(ciaa->ciapra & fire)) bits |= JOY_FIRE;
    return bits;
}
