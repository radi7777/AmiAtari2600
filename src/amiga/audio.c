/*
 * audio.c - Paula output for the TIA sound channels.
 *
 * Paula plays a buffer of LEN words in a loop and latches AUDxLC/AUDxLEN
 * at the start of each loop. So we simply write the next frame's buffer
 * address every frame: it is picked up as soon as the current buffer has
 * played, without interrupts. The buffer length is one Amiga frame at a
 * period of ~one raster line per sample, so buffers and frames line up.
 *
 * The emulated frame may have a slightly different number of scanlines
 * (e.g. 262 vs. 263, or a game with an odd line count); the TIA samples
 * are stretched to the fixed buffer length with nearest-neighbour.
 */
#include <exec/memory.h>
#include <proto/exec.h>

#include "hw.h"
#include "audio.h"
#include "../core/atari.h"

#define BUF_MAX 320

static BYTE *chipbuf;           /* [2 double][2 channel][BUF_MAX] */
static int  cur;
static int  buf_len;            /* samples per buffer (even) */
static int  running;
static BYTE level[31];          /* TIA sum 0..30 -> signed sample */

#define BUF(d, ch) (chipbuf + ((d) * 2 + (ch)) * BUF_MAX)

int audio_init(void)
{
    int i;
    chipbuf = (BYTE *)AllocMem(4 * BUF_MAX, MEMF_CHIP | MEMF_CLEAR);
    if (!chipbuf) return -1;
    for (i = 0; i <= 30; i++) level[i] = (BYTE)(i * 4 - 60);
    return 0;
}

void audio_free(void)
{
    if (chipbuf) FreeMem(chipbuf, 4 * BUF_MAX);
    chipbuf = NULL;
}

static void set_channels(int d)
{
    hw->aud[0].ac_ptr = (UWORD *)BUF(d, 0);
    hw->aud[1].ac_ptr = (UWORD *)BUF(d, 0);
    hw->aud[2].ac_ptr = (UWORD *)BUF(d, 1);
    hw->aud[3].ac_ptr = (UWORD *)BUF(d, 1);
}

void audio_start(int pal)
{
    /* colour clocks per frame / samples per frame */
    long frame_cc = pal ? 227L * 313L : 227L * 263L + 131L;
    int i, period;

    audio_stop();
    buf_len = pal ? 312 : 262;
    period = (int)(frame_cc / buf_len);

    for (i = 0; i < 4 * BUF_MAX; i++) chipbuf[i] = 0;
    set_channels(0);
    for (i = 0; i < 4; i++) {
        hw->aud[i].ac_len = (UWORD)(buf_len / 2);
        hw->aud[i].ac_per = (UWORD)period;
        hw->aud[i].ac_vol = 64;
    }
    hw->dmacon = DMAF_SETCLR | DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    cur = 1;
    running = 1;
}

void audio_stop(void)
{
    int i;
    hw->dmacon = DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    for (i = 0; i < 4; i++) hw->aud[i].ac_vol = 0;
    running = 0;
}

void audio_frame(void)
{
    const u8 *src;
    int ch;

    if (!running) return;
    for (ch = 0; ch < 2; ch++) {
        int n = a26_audio(ch, &src);
        BYTE *dst = BUF(cur, ch);
        int i;
        if (n <= 0) {
            for (i = 0; i < buf_len; i++) dst[i] = 0;
            continue;
        }
        if (n == buf_len) {
            for (i = 0; i < buf_len; i++) dst[i] = level[src[i]];
        } else {
            /* 16.16 fixed point stretch */
            u32 step = ((u32)n << 16) / (u32)buf_len, pos = 0;
            for (i = 0; i < buf_len; i++, pos += step)
                dst[i] = level[src[pos >> 16]];
        }
    }
    set_channels(cur);          /* latched when the playing buffer ends */
    cur ^= 1;
}
