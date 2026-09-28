/*
 * audio.c - Paula output for the TIA sound channels.
 *
 * Paula plays a buffer of LEN words in a loop and latches AUDxLC/AUDxLEN
 * at the start of each loop. So we simply write the next frame's buffer
 * address every frame: it is picked up as soon as the current buffer has
 * played, without interrupts. The period is about one raster line per
 * sample. Since it is an integer, a fixed buffer length would drift
 * against the display (NTSC: 96 colour clocks per frame, a repeated
 * buffer every ~10 s; PAL every ~5 s). So the length is chosen per frame
 * with an accumulator: on average one buffer lasts exactly one frame.
 *
 * The emulated frame's samples (one per scanline) are stretched to the
 * buffer length with nearest-neighbour.
 */
#include <exec/memory.h>
#include <proto/exec.h>

#include "hw.h"
#include "audio.h"
#include "../core/atari.h"

#define BUF_MAX 320

static BYTE *chipbuf;           /* [2 double][2 channel][BUF_MAX] */
static int  cur;
static int  buf_len;            /* samples in the buffer being built (even) */
static long frame_cc2;          /* colour clocks per display frame x 2 */
static long period;             /* Paula period (colour clocks per sample) */
static long acc2;               /* accumulated colour clocks x 2 */
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
    int i;

    audio_stop();
    /* colour clocks per display frame (x 2: NTSC lines are 227.5 clocks),
     * non-interlaced long frames: PAL 313 x 227, NTSC 263 x 227.5 */
    frame_cc2 = pal ? 2L * 227L * 313L : 455L * 263L;
    buf_len = pal ? 312 : 262;
    period = frame_cc2 / (2L * buf_len);
    acc2 = 0;

    for (i = 0; i < 4 * BUF_MAX; i++) chipbuf[i] = 0;
    set_channels(0);
    for (i = 0; i < 4; i++) {
        hw->aud[i].ac_len = (UWORD)(buf_len / 2);
        hw->aud[i].ac_per = (UWORD)period;          /* length: set per frame */
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
    /* this buffer's length: whole sample pairs (Paula counts words) */
    {
        long words;
        acc2 += frame_cc2;
        words = acc2 / (4L * period);
        if (words > BUF_MAX / 2) words = BUF_MAX / 2;
        acc2 -= words * 4L * period;
        buf_len = (int)(words * 2);
    }
    for (ch = 0; ch < 2; ch++) {
        int n = a26_audio(ch, &src);
        /* chip RAM writes are slow on accelerated machines: build the
         * buffer in fast RAM and copy it with longword writes */
        u32 tmp[BUF_MAX / 4];
        BYTE *t = (BYTE *)tmp;
        u32 *dst = (u32 *)BUF(cur, ch);
        int i;
        if (n <= 0) {
            for (i = 0; i < buf_len; i++) t[i] = 0;
        } else if (n == buf_len) {
            for (i = 0; i < buf_len; i++) t[i] = level[src[i]];
        } else {
            /* 16.16 fixed point stretch */
            u32 step = ((u32)n << 16) / (u32)buf_len, pos = 0;
            for (i = 0; i < buf_len; i++, pos += step)
                t[i] = level[src[pos >> 16]];
        }
        for (i = 0; i < (buf_len + 3) / 4; i++)
            dst[i] = tmp[i];
    }
    set_channels(cur);          /* latched when the playing buffer ends */
    for (ch = 0; ch < 4; ch++)
        hw->aud[ch].ac_len = (UWORD)(buf_len / 2);
    cur ^= 1;
}
