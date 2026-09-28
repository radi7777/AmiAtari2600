/*
 * video.c - Amiga display output.
 *
 * Memory layout per buffer (chip RAM):
 *   bitplanes: height x 200 bytes, interleaved (5 x 40 bytes per line),
 *              BPLxMOD = 160. Plane 5 selects the colour bank (0 on even,
 *              1 on odd lines) and is written once per mode change.
 *   copper:    bitplane pointers, then for each display line y
 *                [WAIT $FFDF (when crossing line 255)]
 *                WAIT (line y - 1),$07
 *                VC_MAX_MOVES x MOVE COLORxx for line y (unused -> NOOP)
 *              The moves run while line y - 1 shows the other bank.
 *
 * Chip RAM accesses are the expensive part (especially on accelerated
 * machines), so a line is only converted when its TIA pixels differ from
 * what this buffer shows. A fast-RAM copy of the TIA lines per buffer
 * makes that check cheap; a converted line depends on nothing else (see
 * vidconv.h). Converted lines are compared with fast-RAM copies of the
 * bitplane and copper longwords, and only the longwords that changed are
 * written to chip RAM (a moving sprite usually touches one or two).
 */
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "hw.h"
#include "video.h"
#include "vidconv.h"
#include "../core/palette.h"
#include "../core/tia.h"

#define TOP_PAL         0x2C
#define TOP_NTSC        0x1C
#define HEIGHT_PAL      256
#define HEIGHT_NTSC     224
#define COP_HEADER      (5 * 2 * 2 + 2 * 2)             /* bitplane pointers + BPLCON0 + COLOR00 */
#define COP_LINE_WORDS  (2 + VC_MAX_MOVES * 2)
#define COP_WORDS       (COP_HEADER + 2 + VID_MAX_HEIGHT * COP_LINE_WORDS + 2)
#define PLANE_BYTES     (VID_MAX_HEIGHT * VC_LINE_STRIDE)
#define PSHADOW_BYTES   (VID_MAX_HEIGHT * VC_LINE_LONGS * 4)
#define MSHADOW_BYTES   (VID_MAX_HEIGHT * VC_MAX_MOVES * 4)

typedef struct {
    u8   *planes;                       /* chip */
    UWORD *cop;                         /* chip */
    u32  *line_moves[VID_MAX_HEIGHT];   /* first MOVE of each line in cop */
    u8    nmoves[VID_MAX_HEIGHT];       /* moves currently in the list */
    u32  (*shadow)[VC_WIDTH / 4];       /* fast RAM: TIA line shown per line */
    u32  (*pshadow)[VC_LINE_LONGS];     /* fast RAM: planes 1-4 per line */
    u32  (*mshadow)[VC_MAX_MOVES];      /* fast RAM: copper moves per line */
    u8    valid[VID_MAX_HEIGHT];        /* shadow line matches planes + copper */
} Buffer;

static Buffer buf[2];
static int back;
static int height = HEIGHT_PAL;
static int top = TOP_PAL;
static u32 black_line[VC_WIDTH / 4];

u32 video_stat_lines;                   /* lines converted in the last frame */

/* framebuffer lines the TIA has drawn since each buffer was last
 * rendered (collected by video_note_frame); untouched lines need no
 * compare at all */
static u32 drawn_acc32[2][TIA_FB_LINES / 4];
static int last_first[2] = { -1, -1 };

void video_note_frame(void)
{
    u32 *a = drawn_acc32[0], *b = drawn_acc32[1];
    int i;
    for (i = 0; i < TIA_FB_LINES / 4; i++) {
        u32 d = tia_line_drawn32[i];
        a[i] |= d;
        b[i] |= d;
        tia_line_drawn32[i] = 0;
    }
}
u32 video_stat_writes;                  /* chip RAM longwords written */

int video_init(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        buf[i].planes = (u8 *)AllocMem(PLANE_BYTES, MEMF_CHIP | MEMF_CLEAR);
        buf[i].cop = (UWORD *)AllocMem(COP_WORDS * 2, MEMF_CHIP | MEMF_CLEAR);
        buf[i].shadow = (u32 (*)[VC_WIDTH / 4])AllocMem(VID_MAX_HEIGHT * VC_WIDTH, MEMF_ANY | MEMF_CLEAR);
        buf[i].pshadow = (u32 (*)[VC_LINE_LONGS])AllocMem(PSHADOW_BYTES, MEMF_ANY | MEMF_CLEAR);
        buf[i].mshadow = (u32 (*)[VC_MAX_MOVES])AllocMem(MSHADOW_BYTES, MEMF_ANY | MEMF_CLEAR);
        if (!buf[i].planes || !buf[i].cop || !buf[i].shadow || !buf[i].pshadow || !buf[i].mshadow) {
            video_free();
            return -1;
        }
    }
    return 0;
}

void video_free(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (buf[i].planes) FreeMem(buf[i].planes, PLANE_BYTES);
        if (buf[i].cop) FreeMem(buf[i].cop, COP_WORDS * 2);
        if (buf[i].shadow) FreeMem(buf[i].shadow, VID_MAX_HEIGHT * VC_WIDTH);
        if (buf[i].pshadow) FreeMem(buf[i].pshadow, PSHADOW_BYTES);
        if (buf[i].mshadow) FreeMem(buf[i].mshadow, MSHADOW_BYTES);
        buf[i].pshadow = NULL;
        buf[i].mshadow = NULL;
        buf[i].planes = NULL;
        buf[i].cop = NULL;
        buf[i].shadow = NULL;
    }
}

static void build_buffer(Buffer *b)
{
    UWORD *c = b->cop;
    ULONG pl = (ULONG)b->planes;
    int p, y, m;

    /* plane 5: bank select, all ones on odd lines */
    memset(b->planes, 0, PLANE_BYTES);
    memset(b->pshadow, 0, PSHADOW_BYTES);
    for (y = 1; y < height; y += 2)
        memset(b->planes + y * VC_LINE_STRIDE + 4 * VC_ROW_BYTES, 0xFF, VC_ROW_BYTES);

    for (p = 0; p < 5; p++) {
        ULONG a = pl + (ULONG)p * VC_ROW_BYTES;
        *c++ = (UWORD)(COP_BPL1PTH + p * 4);     *c++ = (UWORD)(a >> 16);
        *c++ = (UWORD)(COP_BPL1PTH + p * 4 + 2); *c++ = (UWORD)(a & 0xFFFF);
    }
    *c++ = 0x100; *c++ = 0x5200;                 /* BPLCON0: 5 planes, colour */
    *c++ = COP_COLOR00; *c++ = 0x000;

    for (y = 0; y < height; y++) {
        int v = top + y - 1;
        if (v == 0x100) { *c++ = 0xFFDF; *c++ = 0xFFFE; }   /* cross line 255 */
        *c++ = (UWORD)(((v & 0xFF) << 8) | 0x07);
        *c++ = 0xFFFE;
        b->line_moves[y] = (u32 *)c;
        for (m = 0; m < VC_MAX_MOVES; m++) {
            *c++ = COP_NOOP; *c++ = 0;
            b->mshadow[y][m] = (u32)COP_NOOP << 16;
        }
        b->nmoves[y] = 0;
        b->valid[y] = 0;
    }
    *c++ = 0xFFFF; *c++ = 0xFFFE;                /* end of list */
}

void video_set_mode(int pal, int pal_colours)
{
    int stop;
    height = pal ? HEIGHT_PAL : HEIGHT_NTSC;
    top = pal ? TOP_PAL : TOP_NTSC;
    stop = top + height;                         /* V8 is implied as !V7 */
    vc_set_palette(pal_colours ? palette_pal : palette_ntsc);

    hw->dmacon = DMAF_RASTER | DMAF_COPPER;
    hw->diwstrt = (UWORD)((top << 8) | 0x81);
    hw->diwstop = (UWORD)(((stop & 0xFF) << 8) | 0xC1);
    hw->ddfstrt = 0x0038;
    hw->ddfstop = 0x00D0;
    hw->bplcon0 = 0x5200;
    hw->bplcon1 = 0;
    hw->bplcon2 = 0;
    hw->bpl1mod = VC_LINE_STRIDE - VC_ROW_BYTES;
    hw->bpl2mod = VC_LINE_STRIDE - VC_ROW_BYTES;
    hw->color[0] = 0;
    hw->color[16] = 0;

    build_buffer(&buf[0]);
    build_buffer(&buf[1]);
    back = 1;

    hw->cop1lc = (ULONG)buf[0].cop;
    hw->copjmp1 = 0;
    hw->dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_RASTER | DMAF_COPPER;
}

int video_height(void)
{
    return height;
}

void video_render(const u8 *tia_fb, int fb_lines, int first)
{
    Buffer *b = &buf[back];
    u32 moves[VC_MAX_MOVES];
    u32 pl[VC_LINE_LONGS];
    u32 converted = 0, written = 0;
    int y;

    const u8 *acc = (const u8 *)drawn_acc32[back];
    int same_first = last_first[back] == first;

    for (y = 0; y < height; y++) {
        int src = first + y;
        const u32 *line;
        u32 *ps, *ms, *mp;
        u8 *row;
        int n, i, p;

        if (src >= 0 && src < fb_lines) {
            /* not drawn since this buffer showed it: nothing to compare */
            if (same_first && b->valid[y] && !acc[src])
                continue;
            line = (const u32 *)(const void *)(tia_fb + src * VC_WIDTH);
            if (b->valid[y] && vc_same_line(line, b->shadow[y]))
                continue;
        } else {
            line = black_line;
            if (b->valid[y] && vc_same_line(line, b->shadow[y]))
                continue;
        }

        n = vc_convert_line((const u8 *)line, y & 1, moves, pl);
        for (i = n; i < b->nmoves[y]; i++)
            moves[i] = (u32)COP_NOOP << 16;
        /* copper: write only moves that changed */
        mp = b->line_moves[y];
        ms = b->mshadow[y];
        for (i = 0; i < n || i < b->nmoves[y]; i++)
            if (ms[i] != moves[i]) {
                ms[i] = moves[i];
                mp[i] = moves[i];
                written++;
            }
        b->nmoves[y] = (u8)n;
        /* bitplanes: write only longwords that changed */
        ps = b->pshadow[y];
        row = b->planes + y * VC_LINE_STRIDE;
        for (p = 0; p < 4; p++) {
            u32 *dst = (u32 *)(void *)(row + p * VC_ROW_BYTES);
            for (i = 0; i < VC_ROW_LONGS; i++, ps++)
                if (*ps != pl[p * VC_ROW_LONGS + i]) {
                    *ps = pl[p * VC_ROW_LONGS + i];
                    dst[i] = *ps;
                    written++;
                }
        }
        memcpy(b->shadow[y], line, VC_WIDTH);
        b->valid[y] = 1;
        converted++;
    }
    memset(drawn_acc32[back], 0, sizeof(drawn_acc32[back]));
    last_first[back] = first;
    video_stat_lines = converted;
    video_stat_writes = written;
}

int video_present(int wait)
{
    int late = hw_vbl_pending();
    hw->cop1lc = (ULONG)buf[back].cop;
    /* the copper restarts from COP1LC at the next vertical blank */
    if (wait)
        hw_wait_vbl();
    back ^= 1;
    return late;
}
