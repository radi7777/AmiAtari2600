/*
 * input.h - joysticks (hardware registers) and keyboard (CIA polling).
 */
#ifndef A26_AMIGA_INPUT_H
#define A26_AMIGA_INPUT_H

#include "../core/types.h"

/* raw Amiga key codes */
#define KEY_SPACE   0x40
#define KEY_ESC     0x45
#define KEY_UP      0x4C
#define KEY_DOWN    0x4D
#define KEY_RIGHT   0x4E
#define KEY_LEFT    0x4F
#define KEY_F1      0x50
#define KEY_F2      0x51
#define KEY_F3      0x52
#define KEY_F4      0x53
#define KEY_F5      0x54
#define KEY_F6      0x55
#define KEY_F7      0x56
#define KEY_F8      0x57
#define KEY_F9      0x58
#define KEY_F10     0x59
#define KEY_HELP    0x5F
#define KEY_LALT    0x64
#define KEY_RALT    0x65
#define KEY_P       0x19

void input_init(int kill_os);   /* kill_os: poll the CIA, else input.device handler */
void input_cleanup(void);
void input_poll(void);          /* read keyboard; call once per frame */
int  key_down(int code);
int  key_pressed(int code);     /* went down since the last poll */
u8   joy_read(int port);        /* JOY_* bits (atari.h) from Amiga port 0/1 */

#endif
