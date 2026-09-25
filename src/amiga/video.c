/*
 * video.c - Amiga display output.
 *
 * Memory layout per buffer (chip RAM):
 *   bitplanes: height x 200 bytes, interleaved (5 x 40 bytes per line),
 *              BPLxMOD = 160
 *   copper:    bitplane pointers, then per display line
 *                [WAIT $FFDF (only before line 256)] WAIT line,$07
 *                VC_MAX_MOVES x MOVE COLORxx (unused ones -> NOOP)
 *
 * Two optimisations keep chip RAM traffic low on a 68030:
 *   - a fast-RAM shadow copy of each buffer's register-index lines;
 *     unchanged lines are not converted/written again (most 2600 screens
 *     are largely static from frame to frame)
 *   - only copper MOVE slots that changed are rewritten
 */
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "hw.h"
#include "video.h"
#include "vidconv.h"
#include "../core/palette.h"
#include "../core/tia.h"

#define DISPLAY_TOP     0x2C
#define COP_HEADER      (6 * 2 * 2)     /* bitplane pointers + BPLCON0 */
#define COP_LINE_WORDS  (2 + VC_MAX_MOVES * 2)
#define COP_WORDS       (COP_HEADER + 2 + VID_MAX_HEIGHT * COP_LINE_WORDS + 4)
#define PLANE_BYTES     (VID_MAX_HEIGHT * VC_LINE_STRIDE)

typedef struct {
    u8   *planes;                       /* chip */
    UWORD *cop;                         /* chip */
    UWORD *line_moves[VID_MAX_HEIGHT];  /* first MOVE of each line in cop */
    u8    nmoves[VID_MAX_HEIGHT];       /* moves currently in the list */
    u8   (*shadow)[VC_WIDTH];           /* fast RAM, register-index lines */
    u8    valid[VID_MAX_HEIGHT];        /* shadow line matches planes */
} Buffer;

static Buffer buf[2];
static int back;
static int height = 256;
static u8  black_line[VC_WIDTH];

int video_init(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        buf[i].planes = (u8 *)AllocMem(PLANE_BYTES, MEMF_CHIP | MEMF_CLEAR);
        buf[i].cop = (UWORD *)AllocMem(COP_WORDS * 2, MEMF_CHIP | MEMF_CLEAR);
        buf[i].shadow = (u8 (*)[VC_WIDTH])AllocMem(VID_MAX_HEIGHT * VC_WIDTH, MEMF_ANY | MEMF_CLEAR);
        if (!buf[i].planes || !buf[i].cop || !buf[i].shadow) {
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
        buf[i].planes = NULL;
        buf[i].cop = NULL;
        buf[i].shadow = NULL;
    }
}

static void build_copper(Buffer *b)
{
    UWORD *c = b->cop;
    ULONG pl = (ULONG)b->planes;
    int p, y, m;

    for (p = 0; p < 5; p++) {
        ULONG a = pl + (ULONG)p * VC_ROW_BYTES;
        *c++ = (UWORD)(COP_BPL1PTH + p * 4);     *c++ = (UWORD)(a >> 16);
        *c++ = (UWORD)(COP_BPL1PTH + p * 4 + 2); *c++ = (UWORD)(a & 0xFFFF);
    }
    *c++ = 0x100; *c++ = 0x5200;                 /* BPLCON0: 5 planes, colour */
    *c++ = COP_COLOR00; *c++ = 0x000;

    for (y = 0; y < height; y++) {
        int v = DISPLAY_TOP + y;
        if (v == 0x100) { *c++ = 0xFFDF; *c++ = 0xFFFE; }   /* cross line 255 */
        *c++ = (UWORD)(((v & 0xFF) << 8) | 0x07);
        *c++ = 0xFFFE;
        b->line_moves[y] = c;
        for (m = 0; m < VC_MAX_MOVES; m++) { *c++ = COP_NOOP; *c++ = 0; }
        b->nmoves[y] = 0;
        b->valid[y] = 0;
    }
    *c++ = 0xFFFF; *c++ = 0xFFFE;                /* end of list */
}

void video_set_mode(int pal, int pal_colours)
{
    height = pal ? 256 : 200;
    vc_set_palette(pal_colours ? palette_pal : palette_ntsc);

    hw->dmacon = DMAF_RASTER | DMAF_COPPER;
    hw->diwstrt = 0x2C81;
    hw->diwstop = pal ? 0x2CC1 : 0xF4C1;
    hw->ddfstrt = 0x0038;
    hw->ddfstop = 0x00D0;
    hw->bplcon0 = 0x5200;
    hw->bplcon1 = 0;
    hw->bplcon2 = 0;
    hw->bpl1mod = VC_LINE_STRIDE - VC_ROW_BYTES;
    hw->bpl2mod = VC_LINE_STRIDE - VC_ROW_BYTES;
    hw->color[0] = 0;

    memset(buf[0].planes, 0, PLANE_BYTES);
    memset(buf[1].planes, 0, PLANE_BYTES);
    memset(buf[0].shadow, 0, VID_MAX_HEIGHT * VC_WIDTH);
    memset(buf[1].shadow, 0, VID_MAX_HEIGHT * VC_WIDTH);
    build_copper(&buf[0]);
    build_copper(&buf[1]);
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
    static u8 slots[VC_WIDTH];
    VcMove moves[VC_MAX_MOVES];
    int y;

    vc_begin_frame();
    for (y = 0; y < height; y++) {
        int src = first + y;
        const u8 *line = (src >= 0 && src < fb_lines) ? tia_fb + src * VC_WIDTH : black_line;
        int n = vc_map_line(line, slots, moves);
        UWORD *mp = b->line_moves[y];
        int i;

        for (i = 0; i < n; i++) {
            mp[i * 2] = (UWORD)(COP_COLOR00 + moves[i].reg * 2);
            mp[i * 2 + 1] = moves[i].rgb;
        }
        for (; i < b->nmoves[y]; i++)
            mp[i * 2] = COP_NOOP;
        b->nmoves[y] = (u8)n;

        if (!b->valid[y] || memcmp(slots, b->shadow[y], VC_WIDTH) != 0) {
            vc_c2p_line(slots, b->planes + y * VC_LINE_STRIDE);
            memcpy(b->shadow[y], slots, VC_WIDTH);
            b->valid[y] = 1;
        }
    }
}

int video_present(void)
{
    int late = hw_vbl_pending();
    hw->cop1lc = (ULONG)buf[back].cop;
    /* the copper restarts from COP1LC at the next vertical blank */
    hw_wait_vbl();
    back ^= 1;
    return late;
}
