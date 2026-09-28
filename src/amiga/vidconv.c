/*
 * vidconv.c - TIA line -> Amiga bitplanes + per-line palette.
 * See vidconv.h for the idea.
 */
#include "vidconv.h"
#include "../core/palette.h"

u32 vc_stat_fallbacks;

static u16 rgb12[128];          /* TIA colour index -> $0RGB */

/* colour -> register of the current line; valid if stamp matches */
static u8  slot_stamp[128];
static u8  slot_of[128];
static u8  stamp;

/* c2p: a pair of TIA pixels (register a, b) -> for each of planes 1-4 a
 * nibble aabb, plane p in the low nibble of byte p (byte 0 = MSB) */
static u32 c2p_pair[256];
static int c2p_ready;

static void c2p_init(void)
{
    int a, b, p;
    for (a = 0; a < 16; a++)
        for (b = 0; b < 16; b++) {
            u32 v = 0;
            for (p = 0; p < 4; p++) {
                u32 nib = (u32)((((a >> p) & 1) ? 0xC : 0) | (((b >> p) & 1) ? 0x3 : 0));
                v |= nib << (24 - 8 * p);
            }
            c2p_pair[(a << 4) | b] = v;
        }
    c2p_ready = 1;
}

void vc_set_palette(const u32 *palette24)
{
    int i;
    for (i = 0; i < 128; i++) rgb12[i] = palette_to_rgb12(palette24[i]);
    if (!c2p_ready) c2p_init();
}

static int rgb_dist(u16 a, u16 b)
{
    int dr = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
    int dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15);
    int db = (int)(a & 15) - (int)(b & 15);
    return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

int vc_map_line(const u8 *tia_line, int bank, u32 *moves, u8 *slots)
{
    u8 col[VC_BANK_COLOURS];
    u32 reg_base = 0x180 + (u32)bank * VC_BANK_COLOURS * 2;
    int n = 1, x;
    u8 prev_c, prev_s;

    if (++stamp == 0) {
        for (x = 0; x < 128; x++) slot_stamp[x] = 0;
        stamp = 1;
    }
    /* register 0 of the bank is black */
    col[0] = 0;
    slot_stamp[0] = stamp;
    slot_of[0] = 0;
    prev_c = 0;
    prev_s = 0;

    for (x = 0; x < VC_WIDTH; x++) {
        u8 c = (u8)(tia_line[x] >> 1);
        if (c != prev_c) {
            prev_c = c;
            if (slot_stamp[c] != stamp) {
                slot_stamp[c] = stamp;
                if (n < VC_BANK_COLOURS) {
                    col[n] = c;
                    slot_of[c] = (u8)n;
                    moves[n - 1] = ((reg_base + (u32)n * 2) << 16) | rgb12[c];
                    n++;
                } else {
                    int s, best = 0, bd = 0x7FFFFFFF;
                    for (s = 0; s < VC_BANK_COLOURS; s++) {
                        int d = rgb_dist(rgb12[c], rgb12[col[s]]);
                        if (d < bd) { bd = d; best = s; }
                    }
                    slot_of[c] = (u8)best;
                }
            }
            prev_s = slot_of[c];
            if (n == VC_BANK_COLOURS && col[prev_s] != c) vc_stat_fallbacks++;
        } else if (n == VC_BANK_COLOURS && col[prev_s] != c) {
            vc_stat_fallbacks++;
        }
        slots[x] = prev_s;
    }
    return n - 1;
}

void vc_c2p_line(const u8 *s, u32 *planes)
{
    int g;
    /* 16 TIA pixels = 32 lowres pixels = one longword per plane */
    for (g = 0; g < VC_ROW_LONGS; g++) {
        /* v_k: byte p = plane p bits of TIA pixels 4k..4k+3 */
        u32 v0 = (c2p_pair[(s[0] << 4) | s[1]] << 4) | c2p_pair[(s[2] << 4) | s[3]];
        u32 v1 = (c2p_pair[(s[4] << 4) | s[5]] << 4) | c2p_pair[(s[6] << 4) | s[7]];
        u32 v2 = (c2p_pair[(s[8] << 4) | s[9]] << 4) | c2p_pair[(s[10] << 4) | s[11]];
        u32 v3 = (c2p_pair[(s[12] << 4) | s[13]] << 4) | c2p_pair[(s[14] << 4) | s[15]];
        /* 4x4 byte transpose */
        u32 t0 = (v0 & 0xFF00FF00UL) | ((v1 >> 8) & 0x00FF00FFUL);
        u32 t1 = ((v0 << 8) & 0xFF00FF00UL) | (v1 & 0x00FF00FFUL);
        u32 t2 = (v2 & 0xFF00FF00UL) | ((v3 >> 8) & 0x00FF00FFUL);
        u32 t3 = ((v2 << 8) & 0xFF00FF00UL) | (v3 & 0x00FF00FFUL);
        planes[0 * VC_ROW_LONGS + g] = (t0 & 0xFFFF0000UL) | (t2 >> 16);
        planes[1 * VC_ROW_LONGS + g] = (t1 & 0xFFFF0000UL) | (t3 >> 16);
        planes[2 * VC_ROW_LONGS + g] = (t0 << 16) | (t2 & 0xFFFFUL);
        planes[3 * VC_ROW_LONGS + g] = (t1 << 16) | (t3 & 0xFFFFUL);
        s += 16;
    }
}

int vc_convert_line(const u8 *tia_line, int bank, u32 *moves, u32 *planes)
{
    u8 slots[VC_WIDTH];
    int n = vc_map_line(tia_line, bank, moves, slots);
    vc_c2p_line(slots, planes);
    return n;
}
