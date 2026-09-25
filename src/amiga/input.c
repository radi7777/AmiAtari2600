/*
 * input.c - joystick + keyboard without the OS.
 *
 * Joystick: JOYxDAT counters decode to directions, fire buttons are on
 * CIA-A port A (bit 6 = port 0, bit 7 = port 1, active low).
 *
 * Keyboard: with interrupts disabled we poll the CIA-A serial port.
 * Each received byte must be acknowledged by pulling KDAT low for at
 * least 85 us (serial port to output mode), otherwise the keyboard
 * controller resyncs and drops keys.
 */
#include "hw.h"
#include "input.h"
#include "../core/atari.h"

static u8 keys[128];
static u8 keys_prev[128];

void input_init(void)
{
    int i;
    for (i = 0; i < 128; i++) keys[i] = keys_prev[i] = 0;
    ciaa->ciaddra &= (UBYTE)~(CIAF_GAMEPORT0 | CIAF_GAMEPORT1);   /* fire lines = input */
}

void input_poll(void)
{
    int i;
    for (i = 0; i < 128; i++) keys_prev[i] = keys[i];

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
