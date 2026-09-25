/*
 * video.h - Amiga display: 320x256 (PAL) / 320x200 (NTSC) lowres,
 * 5 interleaved bitplanes, double buffered, per-line palette via copper.
 */
#ifndef A26_AMIGA_VIDEO_H
#define A26_AMIGA_VIDEO_H

#include "../core/types.h"

#define VID_MAX_HEIGHT 256

int  video_init(void);                      /* allocate chip RAM; 0 = ok */
void video_free(void);
/* program the display window for PAL (256 lines) or NTSC (200 lines) and
 * select the matching TIA palette. Call with the system taken over. */
void video_set_mode(int pal, int pal_colours);
int  video_height(void);
/* convert TIA lines [first, first + height) into the back buffer */
void video_render(const u8 *tia_fb, int fb_lines, int first);
/* show the back buffer from the next frame on and (if wait) wait for that
 * frame. Returns 1 if the frame deadline was already missed (we are late). */
int  video_present(int wait);

#endif
