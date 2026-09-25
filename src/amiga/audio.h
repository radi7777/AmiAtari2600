/*
 * audio.h - TIA sound through Paula.
 *
 * TIA channel 0 -> Paula 0 (left) + Paula 1 (right)
 * TIA channel 1 -> Paula 3 (left) + Paula 2 (right)
 * so both TIA voices are centred, like the mono 2600 output.
 * One sample per Amiga raster line (~15.6 kHz), one buffer per frame.
 */
#ifndef A26_AMIGA_AUDIO_H
#define A26_AMIGA_AUDIO_H

int  audio_init(void);          /* allocate chip RAM; 0 = ok */
void audio_free(void);
void audio_start(int pal);      /* (re)start DMA for the display mode */
void audio_stop(void);
/* queue the samples of the last emulated frame (played next frame) */
void audio_frame(void);

#endif
