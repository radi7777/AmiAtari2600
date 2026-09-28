/*
 * bench.c - freestanding benchmark for tools/m68kprof.
 *
 * Runs the emulation core (and optionally the Amiga video conversion,
 * without chip RAM) for a number of frames. Built with vbcc exactly like
 * the Amiga program; no OS calls. Parameters and results are exchanged
 * through a small block at address $100 (see m68kprof.c).
 */
#include "../../src/core/atari.h"
#include "../../src/core/palette.h"
#include "../../src/amiga/vidconv.h"

#define P ((volatile u32 *)0x100)
enum { P_ROMSIZE, P_FRAMES, P_VIDEO, P_ROMADDR, P_LINES, P_CONVERTED, P_WRITES, P_MARK };

#define HEIGHT 224
static u32 shadow[HEIGHT][VC_WIDTH / 4];
static u32 pshadow[HEIGHT][VC_LINE_LONGS];
static u32 mshadow[HEIGHT][VC_MAX_MOVES];
static u32 chip[HEIGHT][VC_LINE_LONGS + VC_MAX_MOVES];
static u8  valid[HEIGHT];
static u8  nmoves[HEIGHT];

/* same algorithm as video_render() in src/amiga/video.c */
static void bench_render(const u8 *fb, int first)
{
    u32 moves[VC_MAX_MOVES], pl[VC_LINE_LONGS];
    int y, i;
    for (y = 0; y < HEIGHT; y++) {
        const u32 *line = (const u32 *)(const void *)(fb + (first + y) * VC_WIDTH);
        int n;
        if (valid[y] && !tia_line_drawn[first + y]) continue;
        if (valid[y] && vc_same_line(line, shadow[y])) continue;
        n = vc_convert_line((const u8 *)line, y & 1, moves, pl);
        for (i = n; i < nmoves[y]; i++) moves[i] = 0x01FE0000UL;
        for (i = 0; i < n || i < nmoves[y]; i++)
            if (mshadow[y][i] != moves[i]) { mshadow[y][i] = moves[i]; chip[y][VC_LINE_LONGS + i] = moves[i]; P[P_WRITES]++; }
        nmoves[y] = (u8)n;
        for (i = 0; i < VC_LINE_LONGS; i++)
            if (pshadow[y][i] != pl[i]) { pshadow[y][i] = pl[i]; chip[y][i] = pl[i]; P[P_WRITES]++; }
        for (i = 0; i < VC_WIDTH / 4; i++) shadow[y][i] = line[i];
        valid[y] = 1;
        P[P_CONVERTED]++;
    }
}

int main(void)
{
    u32 f;
    if (a26_load((u8 *)P[P_ROMADDR], P[P_ROMSIZE], CART_UNKNOWN)) return 1;
    vc_set_palette(palette_ntsc);
    for (f = 0; f < P[P_FRAMES]; f++) {
        P[P_MARK] = f;                  /* m68kprof starts counting at frame P_MARK */
        a26_run_frame();
        if (P[P_VIDEO]) bench_render(a26_framebuffer(), 30);
        { int i; for (i = 0; i < TIA_FB_LINES / 4; i++) tia_line_drawn32[i] = 0; }
    }
    P[P_LINES] = (u32)tia.frame_lines;
    return 0;
}
